#include "UI/ACERetailTextEntry.h"
#include "Widgets/SVirtualWindow.h"
#include "Widgets/Layout/SDPIScaler.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Protocol/ACECharacterTitleNames.inl"
#include "ACEInventoryRules.h"
#include "ACERetailChat.h"
#include "ACEInputBindings.h"
#include "RetailCustomKeymap.inl"
#include "GameFramework/PlayerInput.h"
#include "ACECameraSettings.h"
#include "ACERuntimeOptions.h"
#include "UI/ACEFrameRateWidget.h"
#include "Components/CheckBox.h"
#include "UI/ACEVideoSettingsWidget.h"
#include "Components/ComboBoxString.h"
#include "Components/Slider.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableText.h"
#include "Engine/UserInterfaceSettings.h"
#include "UI/ACEUIFontStyles.h"
#include "Dat/ACEDatTextLayout.h"
#include "Components/Button.h"
#include "Misc/ConfigCacheIni.h"
#include "ACEPlayerController.h"
#include "ACEMouseCursorWidget.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEWorldEntityActor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ACEScriptComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Components/Image.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/MeshComponent.h"
#include "ACERetailPaperDoll.h"
#include "ProceduralMeshComponent.h"
#include "ShaderCompiler.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Components/ScrollBox.h"
#include "Components/ProgressBar.h"
#include "Components/Border.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUICharSelectBinder.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACERetailTextBlock.h"
#include "UI/ACEAppraisalFormatting.h"
#include "UI/ACERetailVendorCategories.h"
#include "UI/ACERetailTextEntry.h"
#include "UI/ACERetailKeySelector.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "Input/HittestGrid.h"
#include "Types/PaintArgs.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CanvasPanel.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/GarbageCollection.h"
#include "Framework/Application/SlateApplication.h"

// Kept separate from the large screenshot suite to stay within MSVC optimizer limits.
static FORCENOINLINE void CheckVendorFundsUI(FAutomationTestBase& Test,TMap<int32,int32>& Qualities,
    const TArray<TObjectPtr<UTextBlock>>& Notices,const TObjectPtr<UTextBlock>& PurseLabel,
    TFunctionRef<void()> ClickBuy,TFunctionRef<void()> Refresh,TFunctionRef<bool(uint32)> HasAction)
{
    Qualities.Add(20,0);ClickBuy();
    Test.TestFalse(TEXT("Custom pyreal stock cannot spend bank funds through normal Buy"),HasAction(ACEGameAction::Buy));
    Test.TestTrue(TEXT("Normal Buy displays the retail insufficient funds warning"),
        Notices.ContainsByPredicate([](const auto& Line)
            {return Line && Line->GetText().ToString()==TEXT("You don't have enough money");}));
    Qualities.Add(20,1000000);Refresh();
    Test.TestTrue(TEXT("CoinValue update refreshes the vendor purse caption"),
        PurseLabel && PurseLabel->GetText().ToString().Contains(TEXT("1,000,000p")));
    ClickBuy();
    Test.TestTrue(TEXT("Custom pyreal stock Buy button sends a retail Buy action"),HasAction(ACEGameAction::Buy));
}

static FORCENOINLINE void CheckTradeAcceptance(FAutomationTestBase& Test,UACEClientSubsystem* Client,UACEUIElementManager* Manager,
    TFunctionRef<void(uint32,uint32)> Receive,TFunctionRef<void(const TCHAR*)> Click,
    TFunctionRef<void()> ResetPackets,TFunctionRef<bool(uint32)> HasAction)
{
    ResetPackets();Click(TEXT("Trade_ClearAllButon"));
    Test.TestTrue(TEXT("Authored Clear All button sends ResetTrade"),HasAction(ACEGameAction::ResetTrade));
    ResetPackets();Click(TEXT("TradeSelfTradeButton"));
    Test.TestTrue(TEXT("Authored Trade button sends AcceptTrade"),HasAction(ACEGameAction::AcceptTrade));
    Receive(ACEGameEvent::AcceptTrade,Client->GetPlayerGuid());
    ResetPackets();Click(TEXT("TradeSelfTradeButton"));
    Test.TestTrue(TEXT("Accepted trade button cancels confirmation"),HasAction(ACEGameAction::DeclineTrade));
    Receive(ACEGameEvent::AcceptTrade,Client->GetTradePartnerGuid());
    Test.TestTrue(TEXT("Partner acceptance lights authored indicator"),Manager->FindElementByName(TEXT("TradeOtherTradeIndicator"))->bVisible);
    Receive(ACEGameEvent::AcceptTrade,Client->GetPlayerGuid());
    Test.TestTrue(TEXT("Both trade acceptance indicators coexist"),Manager->FindElementByName(TEXT("TradeSelfTradeButton"))->bHighlighted && Manager->FindElementByName(TEXT("TradeOtherTradeIndicator"))->bVisible);
    ResetPackets();Click(TEXT("TradeSelfTradeButton"));
    Test.TestTrue(TEXT("Own acceptance can be withdrawn while partner remains accepted"),HasAction(ACEGameAction::DeclineTrade) && !Client->HasAcceptedTrade() && Client->HasPartnerAcceptedTrade());
}

static FORCENOINLINE void CheckCameraPreview(FAutomationTestBase& Test, UACEUIGameplayBinder* Gameplay,
    UACEVideoSettingsWidget* Video, TFunctionRef<void(const TCHAR*)> ChangePage)
{
    if (auto* Fov=Cast<USlider>(Video->WidgetTree->FindWidget(TEXT("FieldOfViewDegrees"))))
    {
        Test.TestTrue(TEXT("FOV exposes retail's full preference range"),Fov->GetMinValue()==10.f && Fov->GetMaxValue()==160.f);
        Fov->SetValue(120.f); Fov->OnValueChanged.Broadcast(120.f);
        Test.TestEqual(TEXT("Visible options preview the FOV before Apply"),Gameplay->GetCameraFieldOfViewDegrees(),120.f);
        Test.TestEqual(TEXT("Preview does not save the preference"),ACERuntimeOptions::Get(TEXT("FieldOfViewDegrees")),90.f);
        Test.TestTrue(TEXT("Preview still has pending changes"),Video->HasPendingChanges());
        ChangePage(TEXT("ChatPage"));
        Test.TestEqual(TEXT("Leaving the FOV page restores the applied lens"),Gameplay->GetCameraFieldOfViewDegrees(),90.f);
        ChangePage(TEXT("ConfigPage"));
        Video->ResetVideo();
        Test.TestEqual(TEXT("Reset restores the preview immediately"),Gameplay->GetCameraFieldOfViewDegrees(),90.f);
        // Exercise Apply's preference path without resizing the engine
        // window or reloading its global config branch in a UI fixture.
        Fov->SetValue(110.f);Video->ApplyRuntimeOptions();Video->ResetVideo();
        Test.TestEqual(TEXT("Apply saves the previewed lens"),ACERuntimeOptions::Get(TEXT("FieldOfViewDegrees")),110.f);
        FConfigFile SavedFov;SavedFov.Read(GGameUserSettingsIni);float SavedDegrees=0;
        SavedFov.GetFloat(TEXT("ACE.Presentation"),TEXT("FieldOfViewDegrees"),SavedDegrees);
        Test.TestEqual(TEXT("Applied FOV survives settings-file reload"),SavedDegrees,110.f);
        Fov->SetValue(130.f);Video->ResetVideo();
        Test.TestEqual(TEXT("Reset uses the most recently applied lens"),Gameplay->GetCameraFieldOfViewDegrees(),110.f);
        ACERuntimeOptions::Set(TEXT("FieldOfViewDegrees"),90.f);Video->ResetVideo();
    }
    else Test.AddError(TEXT("FOV slider is missing"));
    if(auto* Slope=Cast<UCheckBox>(Video->WidgetTree->FindWidget(TEXT("CameraAlignToSlope"))))
    {
        Slope->SetIsChecked(false);Test.TestTrue(TEXT("Slope checkbox participates in pending changes"),Video->HasPendingChanges());
        Video->ApplyRuntimeOptions();Video->ResetVideo();
        Test.TestEqual(TEXT("Slope checkbox saves and reloads its preference"),ACERuntimeOptions::Get(TEXT("CameraAlignToSlope")),0.f);
        Test.TestFalse(TEXT("Saved slope option is reflected in the UI"),Slope->IsChecked());
        ACERuntimeOptions::Set(TEXT("CameraAlignToSlope"),1);Video->ResetVideo();
    }
    else Test.AddError(TEXT("Camera slope checkbox is missing"));
}

static FORCENOINLINE void CheckKeyboardCapture(FAutomationTestBase& Test, UACERetailKeySelector* Key)
{
            const auto CaptureSlate=Key->TakeWidget();
            const auto SlateButton=CaptureSlate->GetChildren()->GetChildAt(0);
            Test.TestTrue(TEXT("Only the retail label paints; the capture control text is collapsed"),SlateButton->GetChildren()->GetChildAt(0)->GetVisibility()==EVisibility::Collapsed);
            const FKeyEvent EnterKey(EKeys::Enter,FModifierKeysState(),0,false,0,0);
            CaptureSlate->OnKeyDown(Key->GetCachedGeometry(),EnterKey);CaptureSlate->OnKeyUp(Key->GetCachedGeometry(),EnterKey);
            Test.TestTrue(TEXT("Activating a mapping button starts key capture"),Key->GetIsSelectingKey());
            Test.TestTrue(TEXT("Synthetic right Shift release is filtered"),Key->FilterCaptureKey(FKeyEvent(EKeys::RightShift,FModifierKeysState(),0,false,0,0),false));
            Test.TestTrue(TEXT("An orphan modifier release cannot finish key capture"),Key->GetIsSelectingKey());
            Key->FilterCaptureKey(FKeyEvent(EKeys::F7,FModifierKeysState(),0,false,0,0),true);
            Test.TestTrue(TEXT("Matching key release is consumed by its capture owner"),Key->FilterCaptureKey(FKeyEvent(EKeys::F7,FModifierKeysState(),0,false,0,0),false));
            Key->RefreshBinding();
            Test.TestEqual(TEXT("Captured key updates the draft"),ACEInputBindings::Get(EKeys::W,0).Key,EKeys::F7);
            Test.TestEqual(TEXT("Retail label shows the captured binding"),Key->RetailLabel->GetText().ToString(),FString(TEXT("F7")));
            for(float Delta:{1.f,-1.f})
            {
                CaptureSlate->OnKeyDown(Key->GetCachedGeometry(),EnterKey);CaptureSlate->OnKeyUp(Key->GetCachedGeometry(),EnterKey);
                const FPointerEvent Wheel(0,FVector2D::ZeroVector,FVector2D::ZeroVector,TSet<FKey>(),EKeys::Invalid,Delta,FModifierKeysState());
                Test.TestTrue(TEXT("Wheel capture consumes the event instead of scrolling the key list"),Key->FilterCaptureWheel(Wheel));
                Test.TestFalse(TEXT("Wheel completes binding capture"),Key->GetIsSelectingKey());
                Test.TestEqual(TEXT("Wheel direction becomes the selected hotkey"),ACEInputBindings::Get(EKeys::W,0).Key,Delta>0?EKeys::MouseScrollUp:EKeys::MouseScrollDown);
            }
            CaptureSlate->OnKeyDown(Key->GetCachedGeometry(),EnterKey);CaptureSlate->OnKeyUp(Key->GetCachedGeometry(),EnterKey);
            const FAnalogInputEvent Stick(EKeys::Gamepad_RightX,FModifierKeysState(),0,false,0,0,-.9f);
            Test.TestTrue(TEXT("Controller stick direction is captured by the key editor"),Key->FilterCaptureAnalog(Stick));
            Test.TestFalse(TEXT("Controller capture ends normally"),Key->GetIsSelectingKey());
            Test.TestEqual(TEXT("Captured stick direction maps to the requested action"),ACEInputBindings::Get(EKeys::W,0).Key,EKeys::Gamepad_RightStick_Left);
            CaptureSlate->OnKeyDown(Key->GetCachedGeometry(),EnterKey);CaptureSlate->OnKeyUp(Key->GetCachedGeometry(),EnterKey);
            const FKeyEvent Button(EKeys::Gamepad_FaceButton_Left,FModifierKeysState(),0,false,0,0);
            Key->FilterCaptureKey(Button,true);Key->FilterCaptureKey(Button,false);
            Test.TestEqual(TEXT("Controller face buttons can also be rebound"),ACEInputBindings::Get(EKeys::W,0).Key,EKeys::Gamepad_FaceButton_Left);
            for(const FKey& Modifier:{EKeys::LeftShift,EKeys::RightShift,EKeys::LeftControl,EKeys::RightControl,EKeys::LeftAlt,EKeys::RightAlt})
            {
                CaptureSlate->OnKeyDown(Key->GetCachedGeometry(),EnterKey);CaptureSlate->OnKeyUp(Key->GetCachedGeometry(),EnterKey);
                const FKeyEvent Event(Modifier,FModifierKeysState(),0,false,0,0);
                Key->FilterCaptureKey(Event,true);
                Test.TestTrue(TEXT("Unpressed key cannot replace the pressed modifier"),Key->FilterCaptureKey(FKeyEvent(EKeys::F8,FModifierKeysState(),0,false,0,0),false));
                Test.TestTrue(TEXT("Physical modifier release is consumed by its capture owner"),Key->FilterCaptureKey(Event,false));
                Key->RefreshBinding();
                Test.TestEqual(TEXT("Capture preserves each modifier's physical left/right identity"),ACEInputBindings::Get(EKeys::W,0).Key,Modifier);
            }
}

static FORCENOINLINE void CheckVendorRowArtwork(FAutomationTestBase& Test,const TArray<TObjectPtr<UBorder>>& Rows,
    const TArray<int32>& Indices,int32 SelectedFilter,UACEUIResourceResolver* Resolver)
{
    for(int32 Row=0;Row<Indices.Num()&&Row<Rows.Num();++Row)
    {
        const bool Selected=Indices.IsValidIndex(Row)&&Indices[Row]==SelectedFilter;
        Test.TestTrue(TEXT("Vendor category rows use retail green normal/selected textures"),
            Rows[Row]->Background.GetResourceObject()==Resolver->ResolveTexture(Selected?0x060012B4:0x060012B3));
    }
}

struct FACERetailAugmentationUIFixture
{
    static void Check(FAutomationTestBase& Test, UACEUIGameplayBinder* Gameplay,
        UACEClientSubsystem* Client, UACEDatSubsystem* Dat, FACEPlayerVitals& V);
};

FORCENOINLINE void FACERetailAugmentationUIFixture::Check(FAutomationTestBase& Test,
    UACEUIGameplayBinder* Gameplay, UACEClientSubsystem* Client, UACEDatSubsystem* Dat, FACEPlayerVitals& V)
{
    const auto SavedVitals = V;
    const int32 SavedSkill = Gameplay->SelectedSkillId;
    const int32 SavedAttribute = Gameplay->SelectedAttributeRow;
    const int32 SavedAttributeScroll = Gameplay->AttributeScrollOffset;
    const int32 SavedSkillScroll = Gameplay->SkillListScrollOffset;
    V.Strength = V.Endurance = V.Coordination = V.Quickness = V.Focus = V.Self = 155;
    V.StatQualityInts.Reset(); V.Skills.Reset();
    for (int32 Id : {39, 44, 47, 34})
    {
        FACESkillInfo Skill; Skill.SkillId = Id;
        Skill.AdvancementClass = Id == 39 ? 3 : Id == 34 ? 1 : 2;
        Skill.InitLevel = Id == 39 ? 10 : 5; Skill.Ranks = 50;
        V.Skills.Add(Skill);
    }
    for (int32 Property : {218, 219, 220, 221, 222, 223, 300, 301, 302})
        V.StatQualityInts.Add(Property, 1);
    V.StatQualityInts.Add(365, 3);
    Dat->RecomputePlayerStats(V, {}); Client->OnVitalsUpdated.Broadcast(V);
    Gameplay->SkillListScrollOffset = 0; Gameplay->SyncSkillPanelTab(TEXT("SkillPage"));
    for (int32 Id : {39, 44, 47, 34})
    {
        Gameplay->SelectedSkillId = Id; Gameplay->RefreshSkillOverlays();
        const int32 Row = Gameplay->SkillRowIds.IndexOfByKey(Id);
        if (Test.TestTrue(TEXT("Augmented skill has a visible row"), Row != INDEX_NONE))
        {
            const auto Color = Gameplay->SkillRowValues[Row]->GetColorAndOpacity().GetSpecifiedColor();
            Test.TestTrue(TEXT("Innate skill augmentations paint neutral ivory"), Color.Equals(FLinearColor(.95f, .95f, .9f, 1.f)));
            Test.TestFalse(TEXT("Innate skill augmentations have no (+N) suffix"), Gameplay->AttrFooterTitle->GetText().ToString().Contains(TEXT(" (+")));
        }
    }
    V.StatQualityInts.Add(326, 1); V.StatQualityInts.Add(344, 3);
    Dat->RecomputePlayerStats(V, {}); Client->OnVitalsUpdated.Broadcast(V);
    for (int32 Id : {39, 44, 47, 34})
    {
        Gameplay->SelectedSkillId = Id; Gameplay->RefreshSkillOverlays();
        const int32 Row = Gameplay->SkillRowIds.IndexOfByKey(Id);
        if (Test.TestTrue(TEXT("Augmented trained, specialized and untrained skills have rows"), Row != INDEX_NONE))
        {
            const auto Color = Gameplay->SkillRowValues[Row]->GetColorAndOpacity().GetSpecifiedColor();
            Test.TestTrue(TEXT("Jack/spec luminance alone paint neutral ivory"), Color.Equals(FLinearColor(.95f, .95f, .9f, 1.f)));
            Test.TestFalse(TEXT("Permanent gains have no positive footer suffix"), Gameplay->AttrFooterTitle->GetText().ToString().Contains(TEXT(" (+")));
        }
    }
    Gameplay->SelectedSkillId = 39;
    auto CheckCooking = [&](const TCHAR* Title, int32 Modifier)
    {
        Client->OnVitalsUpdated.Broadcast(V); Gameplay->RefreshSkillOverlays();
        Test.TestEqual(TEXT("Skill footer includes permanent gains in its baseline"),
            Gameplay->AttrFooterTitle->GetText().ToString(), FString(Title));
        const int32 Row = Gameplay->SkillRowIds.IndexOfByKey(39);
        if (Test.TestTrue(TEXT("Cooking remains visible"), Row != INDEX_NONE))
        {
            const auto Color = Gameplay->SkillRowValues[Row]->GetColorAndOpacity().GetSpecifiedColor();
            Test.TestTrue(TEXT("Row color agrees with actual spell/vitae contribution"),
                Modifier > 0 ? Color.G > Color.R : Modifier < 0 ? Color.R > Color.G : Color.Equals(FLinearColor(.95f, .95f, .9f, 1.f)));
        }
    };
    CheckCooking(TEXT("Cooking: 177"), 0);
    FACEActiveEnchantment Buff; Buff.SpellId = 3; Buff.SpellCategory = 102;
    Buff.StatModType = 0x8010; Buff.StatModKey = 39; Buff.StatModValue = 40;
    Dat->RecomputePlayerStats(V, {Buff}); CheckCooking(TEXT("Cooking: 217 (+40)"), 40);
    Buff.StatModValue = -3;
    Dat->RecomputePlayerStats(V, {Buff}); CheckCooking(TEXT("Cooking: 174 (-3)"), -3);
    FACEActiveEnchantment Vitae; Vitae.bVitae = true; Vitae.StatModValue = .9f;
    Dat->RecomputePlayerStats(V, {Vitae}); CheckCooking(TEXT("Cooking: 160 (-17)"), -17);
    Dat->RecomputePlayerStats(V, {}); CheckCooking(TEXT("Cooking: 177"), 0);
    V.StatQualityInts.Remove(326); V.StatQualityInts.Remove(344);
    Dat->RecomputePlayerStats(V, {}); CheckCooking(TEXT("Cooking: 166"), 0);
    Gameplay->AttributeScrollOffset = 0; Gameplay->SyncSkillPanelTab(TEXT("AttributePage"));
    for (int32 Row = 0; Row < 6; ++Row)
    {
        Gameplay->SelectedAttributeRow = Row; Gameplay->RefreshAttributeOverlays();
        const auto Color = Gameplay->AttributeRowValues[Row]->GetColorAndOpacity().GetSpecifiedColor();
        Test.TestTrue(TEXT("Innate attributes stay neutral even with Jack/spec augmentations"), Color.Equals(FLinearColor(.95f, .95f, .9f, 1.f)));
        Test.TestEqual(TEXT("Innate attribute value is counted once in the row"), Gameplay->AttributeRowValues[Row]->GetText().ToString(), FString(TEXT("155")));
        Test.TestFalse(TEXT("Innate attribute footer has no bonus suffix"), Gameplay->AttrFooterTitle->GetText().ToString().Contains(TEXT(" (+")));
    }
    Gameplay->SelectedSkillId = SavedSkill; Gameplay->SelectedAttributeRow = SavedAttribute;
    Gameplay->AttributeScrollOffset = SavedAttributeScroll; Gameplay->SkillListScrollOffset = SavedSkillScroll;
    V = SavedVitals; Client->OnVitalsUpdated.Broadcast(V); Gameplay->RefreshAttributeOverlays();
}

static FORCENOINLINE void CheckButtonDragArtwork(FAutomationTestBase& Test, UACEUIElementManager* Layout)
{
    // Isolate the authored buttons so release/cancel never closes a real window
    // or logs out the fixture. Input still traverses the actual capture manager.
    auto* Input=NewObject<UACEUIElementManager>();Input->Initialize();
    const FVector2D Size(Input->GetCanvasWidth(),Input->GetCanvasHeight());
    const FVector2D Inside(24,24),Outside(150,150);
    int32 Activations=0;
    Input->OnElementActivated.AddLambda([&](TSharedPtr<FACEUIElement>){++Activations;});
    for(const TCHAR* Name:{TEXT("PeaceModeButton"),TEXT("MeleeModeButton"),TEXT("MissileModeButton"),TEXT("MagicModeButton"),
        TEXT("PanelButton_UseSelectedButton"),TEXT("PanelButton_ExamineSelectedButton"),TEXT("ClosePanelButton"),
        TEXT("CloseChatWindowButton"),TEXT("LogoffButton"),TEXT("LockUI")})
    {
        const auto Source=Layout->FindElementByName(Name);
        if(!Test.TestTrue(Name,Source.IsValid()))continue;
        for(bool Locked:{false,true})
        {
            Input->ClearRoots();Activations=0;
            auto Button=MakeShared<FACEUIElement>(*Source);
            Button->Parent.Reset();Button->Children.Reset();Button->X=16;Button->Y=16;Button->Width=32;Button->Height=32;
            Button->LayoutOffsetX=0;Button->LayoutOffsetY=0;Button->bVisible=true;Button->bActivatable=true;Button->bGhosted=false;
            Input->AddRoot(Button);Input->SetUiLocked(Locked);
            auto CheckArt=[&](const TCHAR* Stage)
            {
                const auto* State=Button->ResolvePaintState(Input->GetCaptureElement()==Button,Input->GetHoverElement()==Button,false);
                Test.TestTrue(FString::Printf(TEXT("%s retains artwork %s (%s)"),Name,Stage,Locked?TEXT("locked"):TEXT("unlocked")),
                    Button->bVisible && (State && State->bHasMedia ? State->ImageFileId!=0 : Button->ImageFileId!=0));
            };
            CheckArt(TEXT("before press"));
            Input->NotifyMouseDown(Inside,Size,EKeys::LeftMouseButton);
            Test.TestTrue(TEXT("Button receives capture"),Input->GetCaptureElement()==Button);CheckArt(TEXT("pressed"));
            Input->NotifyMouseMove(Outside,Size);CheckArt(TEXT("dragged outside"));
            Input->NotifyMouseMove(Inside,Size);CheckArt(TEXT("dragged back inside"));
            Input->NotifyMouseUp(Outside,Size,EKeys::LeftMouseButton);CheckArt(TEXT("released outside"));
            Test.TestEqual(TEXT("Releasing outside does not activate a button"),Activations,0);
            Input->NotifyMouseDown(Inside,Size,EKeys::LeftMouseButton);CheckArt(TEXT("pressed again"));
            Input->CancelPointerCapture();CheckArt(TEXT("capture cancelled"));
            Input->NotifyMouseUp(Inside,Size,EKeys::LeftMouseButton);
            Test.TestEqual(TEXT("Cancelled capture does not activate on release"),Activations,0);
            Input->NotifyMouseDown(Inside,Size,EKeys::LeftMouseButton);
            Input->NotifyMouseMove(Outside,Size);Input->NotifyMouseMove(Inside,Size);
            Input->NotifyMouseUp(Inside,Size,EKeys::LeftMouseButton);CheckArt(TEXT("released inside"));
            Test.TestEqual(TEXT("Returning inside activates exactly once on release"),Activations,1);
        }
    }
    Input->OnElementActivated.Clear();Input->Shutdown();
}

struct FACEChatUIFixture
{
    using FCapture = TFunctionRef<TArray<FColor>(const FString&,float,FVector2D)>;
    static FORCENOINLINE void Check(FAutomationTestBase& Test, UACEUIGameplayBinder* Gameplay,
        UACEUICanvasWidget* Canvas, UACEUIElementManager* Manager, FIntPoint ScreenSize,
        const FString& LongChat, FCapture Capture)
    {
        auto CaptureScreen=[&](const FString& Name,float Scale=1.f) { return Capture(Name,Scale,FVector2D::ZeroVector); };
        auto CheckControlHit=[&](const TSharedPtr<FACEUIElement>& Element,const TCHAR* Message,float Scale=1.f)
        {
            if(!FApp::CanEverRender())return;
            FWidgetRenderer Renderer(true,true);FHittestGrid Grid;
            const auto Window=SNew(SVirtualWindow).Size(FVector2D(ScreenSize));
            Window->SetContent(SNew(SDPIScaler).DPIScale(Scale)[Canvas->TakeWidget()]);
            auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(ScreenSize),TF_Bilinear,true);
            for(int32 Pass=0;Pass<3;++Pass)
            {
                ++GFrameCounter;Canvas->NativeTick(Canvas->GetCachedGeometry(),0.f);
                Renderer.DrawWindow(Target,Grid,Window,1.f,FVector2D(ScreenSize),0.f);FlushRenderingCommands();
            }
            auto* Art=Canvas->ImageWidgets.FindRef(Element->InstanceId).Get();
            if(!Test.TestNotNull(TEXT("Chat control has a rendered hit surface"),Art))return;
            const auto& G=Art->GetCachedGeometry();
            for(const FVector2D Fraction:{FVector2D(.05,.05),FVector2D(.95,.05),FVector2D(.5,.5),FVector2D(.05,.95),FVector2D(.95,.95)})
            {
                bool HitControl=false;
                for(const auto& Hit:Grid.GetBubblePath(G.LocalToAbsolute(G.GetLocalSize()*Fraction),0,false))
                    HitControl |= Hit.Widget==Art->GetCachedWidget();
                Test.TestTrue(Message,HitControl);
            }
        };
    {
        // Exercise real DAT resize grips, reflow and scroll routing in all five windows.
        TGuardValue<FString> SettingsPath(GGameUserSettingsIni,FPaths::ProjectSavedDir()/TEXT("Automation/ChatResizeFixture.ini"));
        FConfigFile Config; Config.NoSave=false; Config.bCanSaveAllSections=true;
        GConfig->SetFile(GGameUserSettingsIni,&Config);
        const bool Locked=Manager->IsUiLocked(); Manager->SetUiLocked(false);
        for (int32 W=0; W<=4; ++W)
        {
            const FString Name=W ? FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"),W) : TEXT("RootGameplay_FloatyMainChat_Field");
            auto Root=Manager->FindElementByName(Name);
            const int32 SavedW=Root->UserResizeW,SavedH=Root->UserResizeH;
            const FIntPoint SavedDrag(Root->UserDragX,Root->UserDragY);
            Gameplay->SetFloatyVisible(Name,true); Manager->BringFloatyToFront(Root);
            Root->UserDragX+=600-Root->GetScreenOrigin().X;
            Root->UserDragY+=100-Root->GetScreenOrigin().Y; Root->RecomputeLayoutOffset();
            Gameplay->ClearChatLog(W);
            for (int32 I=0; I<15; ++I) Gameplay->AppendChatLineToLog(W,LongChat,FLinearColor::White,FString());
            CaptureScreen(FString::Printf(TEXT("Chat%dBeforeResize"),W));
            auto* Log=Gameplay->GetChatLogWidget(W);
            const float NarrowRowH=Log->GetChildAt(0)->GetDesiredSize().Y;
            const int32 Width=Root->Width,Height=Root->Height;
            auto Drag=[&](const TCHAR* GripName,FVector2D Delta)
            {
                const auto Grip=Manager->FindElementUnder(Name,GripName);
                const FVector2D Start=Canvas->LayoutToViewport(FVector2D(Grip->GetScreenOrigin())+FVector2D(2,2));
                Manager->NotifyMouseDown(Start,FVector2D(ScreenSize),EKeys::LeftMouseButton);
                Manager->NotifyMouseMove(Start+Delta,FVector2D(ScreenSize));
                Manager->NotifyMouseUp(Start+Delta,FVector2D(ScreenSize),EKeys::LeftMouseButton);
            };
            Drag(W ? TEXT("RightBorder") : TEXT("MainChatRightBorder"),FVector2D(180,0));
            Test.TestEqual(TEXT("Side grip widens chat"),Root->Width,Width+180);
            Test.TestEqual(TEXT("Side grip preserves height"),Root->Height,Height);
            Drag(W ? TEXT("BottomBorder") : TEXT("MainChatBottomBorder"),FVector2D(0,80));
            Test.TestEqual(TEXT("Bottom grip resizes height"),Root->Height,Height+80);
            CaptureScreen(FString::Printf(TEXT("Chat%dWidened"),W));
            auto LogEl=Manager->FindElementUnder(Name,TEXT("ChatLog"));
            auto Bar=Manager->FindElementUnder(Name,TEXT("ChatLogScrollbar"));
            const auto LogSlot=CastChecked<UCanvasPanelSlot>(Log->Slot);
            Test.TestTrue(TEXT("All chat logs use DAT margins without a second numbered-button gutter"),
                LogSlot->GetPosition().Equals(Canvas->LayoutToViewport(FVector2D(LogEl->GetScreenOrigin())
                    +FVector2D(LogEl->TextMargins.Left,LogEl->TextMargins.Top)),.01));
            Test.TestEqual(TEXT("Chat wrapping uses the complete retail text width"),Gameplay->ChatRowWidths[W],
                float(LogEl->Width)-LogEl->TextMargins.Left-LogEl->TextMargins.Right);
            Test.TestTrue(TEXT("Chat rows rewrap to the wider window"),Log->GetChildAt(0)->GetDesiredSize().Y<NarrowRowH);
            Test.TestTrue(TEXT("Scroll region follows content height"),FMath::Abs(LogEl->Height-Bar->Height)<=1);
            Test.TestEqual(TEXT("Scrollbar remains inset from right border"),Bar->GetScreenOrigin().X+Bar->Width,Root->GetScreenOrigin().X+Root->Width-5);
            const auto Up=Manager->FindElementUnder(Name,TEXT("ScrollBar_Up"));
            const auto Down=Manager->FindElementUnder(Name,TEXT("ScrollBar_Down"));
            Test.TestTrue(TEXT("Both arrow controls remain visible"),Up->bVisible && Down->bVisible);
            Test.TestEqual(TEXT("Top arrow has no duplicated reflow offset"),Up->GetScreenOrigin().Y,Bar->GetScreenOrigin().Y);
            Test.TestEqual(TEXT("Bottom arrow remains within bar"),Down->GetScreenOrigin().Y+Down->Height,Bar->GetScreenOrigin().Y+Bar->Height);
            const auto Mid=Manager->FindElementUnder(Name,TEXT("widget_mid_field"));
            Test.TestTrue(TEXT("Blue thumb has drawable area within the scrollbar"),Mid && Mid->Parent.Pin()->bVisible && Mid->Height>0 &&
                Mid->GetScreenOrigin().Y>=Bar->GetScreenOrigin().Y+16 && Mid->GetScreenOrigin().Y+Mid->Height<=Bar->GetScreenOrigin().Y+Bar->Height-16);
            const FVector2D LogPoint=Canvas->LayoutToViewport(FVector2D(LogEl->GetScreenOrigin())+FVector2D(12,12));
            Test.TestEqual(TEXT("Wheel routing finds the correct chat"),Gameplay->ChatWindowAtPointer(LogPoint),W);
            const float Before=Log->GetScrollOffset(); Gameplay->ScrollChatLog(-28.f,W);
            Test.TestTrue(TEXT("Chat scrolls independently upward"),Log->GetScrollOffset()<Before);
            const FVector2D BarPoint=Canvas->LayoutToViewport(FVector2D(Bar->GetScreenOrigin())+FVector2D(8,18));
            Test.TestTrue(TEXT("Every chat thumb accepts dragging"),Gameplay->TryBeginScrollbarDrag(BarPoint));
            Gameplay->TryFinishScrollbarDrag();
            Test.TestTrue(TEXT("Dragging reaches older messages"),Log->GetScrollOffset()<Log->GetScrollOffsetOfEnd()*.2f);
            Gameplay->SyncChatJumpIndicator(W);
            Test.TestFalse(TEXT("Scrolling alone does not report unread messages"),Manager->FindElementUnder(Name,TEXT("ChatLogNewNonVisibleTextIndicator"))->bVisible);
            const float Browsing=Log->GetScrollOffset(); Gameplay->AppendChatLineToLog(W,LongChat,FLinearColor::White,FString());
            CaptureScreen(FString::Printf(TEXT("Chat%dScrolled"),W));
            Test.TestTrue(TEXT("New messages preserve the browsed position"),FMath::Abs(Log->GetScrollOffset()-Browsing)<1.f);
            const auto JumpIndicator=Manager->FindElementUnder(Name,TEXT("ChatLogNewNonVisibleTextIndicator"));
            if(Test.TestTrue(TEXT("Every chat has the retail jump-to-bottom arrow"),JumpIndicator.IsValid()))
            {
                Gameplay->SyncChatJumpIndicator(W);
                Test.TestTrue(TEXT("Unread messages show the arrow in their own chat window"),JumpIndicator->bVisible);
                const auto* ArrowMedia=JumpIndicator->ResolvePaintState(false,false,false);
                Test.TestTrue(TEXT("Visible arrow selects its retail artwork rather than a blank default"),ArrowMedia && ArrowMedia->bHasMedia && ArrowMedia->ImageFileId==0x06005F0E);
                const auto ArrowOrigin=JumpIndicator->GetScreenOrigin(),LogOrigin=LogEl->GetScreenOrigin();
                Test.TestTrue(TEXT("Arrow remains within the log after resizing"),ArrowOrigin.X==LogOrigin.X && ArrowOrigin.Y>=LogOrigin.Y && ArrowOrigin.Y+16<=LogOrigin.Y+LogEl->Height);
                CaptureScreen(FString::Printf(TEXT("Chat%dUnreadControl"),W));
                CheckControlHit(JumpIndicator,TEXT("Unread arrow takes precedence over selectable chat text"));
                Gameplay->ScrollChatLog(100000,W);
                Test.TestFalse(TEXT("Returning to the bottom hides that window's arrow"),JumpIndicator->bVisible);
            }
            Manager->SaveFloatyLayout();
            Root->UserResizeW=0; Root->UserResizeH=0; UACEUIElementManager::ApplyFloatyResizeLayout(Root);
            Manager->LoadFloatyLayout();
            Test.TestEqual(TEXT("Chat width survives save/reload"),Root->Width,Width+180);
            Test.TestEqual(TEXT("Chat height survives save/reload"),Root->Height,Height+80);
            Drag(W ? TEXT("RightBorder") : TEXT("MainChatRightBorder"),FVector2D(-180,0));
            CaptureScreen(FString::Printf(TEXT("Chat%dNarrowed"),W));
            Test.TestEqual(TEXT("Narrowing restores original width"),Root->Width,Width);
            Test.TestTrue(TEXT("Rows wrap again when narrowed"),Log->GetChildAt(0)->GetDesiredSize().Y>=NarrowRowH);
            Root->UserResizeW=SavedW; Root->UserResizeH=SavedH;
            Root->UserDragX=SavedDrag.X; Root->UserDragY=SavedDrag.Y;
            Root->RecomputeLayoutOffset(); UACEUIElementManager::ApplyFloatyResizeLayout(Root);
            Gameplay->ClearChatLog(W); Gameplay->AppendChatLineToLog(W,LongChat,FLinearColor::White,FString());
            if (W) Gameplay->SetFloatyVisible(Name,false);
        }
        // The main chat's explicit DAT control expands rather than hiding it.
        const FString MainName=TEXT("RootGameplay_FloatyMainChat_Field");
        const auto Main=Manager->FindElementByName(MainName);
        const int32 SavedHeight=Main->UserResizeH,SavedY=Main->UserDragY;
        const FText Draft=Gameplay->ChatEntry->GetText();
        Gameplay->ChatEntry->SetText(FText::FromString(TEXT("unsent chat draft")));
        Gameplay->ClearChatLog(0);
        Gameplay->AppendChatLineToLog(0,LongChat,FLinearColor::White,TEXT("Expand sender fixture"));
        for(bool Lock:{false,true})for(float Scale:{1.f,1.5f})for(bool AtTop:{false,true})
        {
            Manager->SetUiLocked(Lock);
            CaptureScreen(TEXT("ChatExpandViewport"),Scale);
            Main->UserResizeH=200-Main->AuthoredHeight;
            UACEUIElementManager::ApplyFloatyResizeLayout(Main);
            Main->UserDragY+=(AtTop?40:Main->Parent.Pin()->Height-Main->Height)-Main->GetDrawY();
            Main->RecomputeLayoutOffset();Manager->BringFloatyToFront(Main);
            const int32 OldY=Main->GetDrawY(),OldH=Main->Height,OldW=Main->Width;
            const auto Button=Manager->FindElementUnder(MainName,TEXT("MaximizeButton"));
            Gameplay->GetChatLogWidget(0)->ScrollToStart();
            CaptureScreen(TEXT("ChatBeforeExpand"),Scale);
            const auto* ExpandState=Button->ResolvePaintState(false,false,false);
            Test.TestTrue(TEXT("Main chat uses retail expand artwork"),ExpandState && ExpandState->ImageFileId==0x06005E65);
            CheckControlHit(Button,TEXT("Expand icon wins Slate hit testing over chat text at both scales"),Scale);
            const FVector2D CoveredSender=Canvas->GetCachedGeometry().LocalToAbsolute(
                Canvas->LayoutToViewport(FVector2D(Button->GetScreenOrigin())+FVector2D(8,8)));
            Button->bVisible=false;
            Test.TestTrue(TEXT("Fixture has clickable sender text underneath the expand control"),Gameplay->TryHandleChatNameClick(CoveredSender));
            Test.TestEqual(TEXT("Uncovered chat sender still starts a tell"),Gameplay->ChatEntry->GetText().ToString(),FString(TEXT("@tell Expand sender fixture, ")));
            Button->bVisible=true;
            Gameplay->ChatEntry->SetText(FText::FromString(TEXT("unsent chat draft")));
            auto ClickExpand=[&]()
            {
                const FVector2D P=Canvas->LayoutToViewport(FVector2D(Button->GetScreenOrigin())+FVector2D(8,8));
                Test.TestTrue(TEXT("DAT hit testing resolves the expand/restore control"),Manager->HitTestCanvas(Button->GetScreenOrigin().X+8,Button->GetScreenOrigin().Y+8)==Button);
                const auto Geometry=Canvas->GetCachedGeometry();
                const FVector2D Absolute=Geometry.LocalToAbsolute(P);
                Canvas->NativeOnMouseButtonDown(Geometry,FPointerEvent(0,Absolute,Absolute,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
                Canvas->NativeOnMouseButtonUp(Geometry,FPointerEvent(0,Absolute,Absolute,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
            };
            ClickExpand();CaptureScreen(TEXT("ChatExpanded"),Scale);
            Test.TestTrue(TEXT("Expanding keeps main chat visible while UI is locked"),Main->bVisible);
            Test.TestEqual(TEXT("Retail expand adds half the logical screen height"),Main->Height,OldH+Main->Parent.Pin()->Height/2);
            Test.TestEqual(TEXT("Top chat grows down and bottom chat grows up"),Main->GetDrawY(),AtTop?OldY:OldY-Main->Parent.Pin()->Height/2);
            Test.TestEqual(TEXT("Expand preserves the width"),Main->Width,OldW);
            const auto* State=Button->ResolvePaintState(true,true,false);
            Test.TestTrue(TEXT("Restore artwork remains visible when pressed"),State && State->ImageFileId==0x06005E64);
            ClickExpand();CaptureScreen(TEXT("ChatRestored"),Scale);
            Test.TestEqual(TEXT("Restore returns to the previous height"),Main->Height,OldH);
            Test.TestEqual(TEXT("Restore returns to the previous position"),Main->GetDrawY(),OldY);
            Test.TestEqual(TEXT("Expand/restore preserves the unsent draft"),Gameplay->ChatEntry->GetText().ToString(),FString(TEXT("unsent chat draft")));
        }
        Gameplay->ClearChatLog(0);Gameplay->AppendChatLineToLog(0,LongChat,FLinearColor::White,FString());
        Main->UserResizeH=SavedHeight;Main->UserDragY=SavedY;Main->RecomputeLayoutOffset();
        UACEUIElementManager::ApplyFloatyResizeLayout(Main);
        Gameplay->ChatEntry->SetText(Draft);CaptureScreen(TEXT("ChatResizeRestored"));
        Manager->SetUiLocked(Locked);
    }
    }
};


namespace
{
FString ACERetailUIScreenTestsDatDirectory()
{
    FString Directory=TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Directory);
    return Directory;
}
}

// Keep the counted-contract rendering regression outside the large UI suite.
static FORCENOINLINE void CheckContractProgress(FAutomationTestBase& Test, UACEDatSubsystem* Dat,
    UACEUIElementManager* Manager, TArray<FACEContractEntry>& Contracts, const TArray<int32>& RowIds,
    const TArray<TObjectPtr<UTextBlock>>& StatusRows, TFunctionRef<void()> Refresh,
    TFunctionRef<void(const FString&,float)> CaptureScreen, TFunctionRef<int32()> ScrollNext)
{
    const auto SavedContracts=Contracts;
    TArray<int32> KillContracts;
    for(int32 Id=1;Id<1000;++Id)
        if(const auto* Info=Dat->GetContractInfo(Id);Info && Info->DescriptionProgress.Contains(TEXT("%d")))KillContracts.Add(Id);
    Test.TestFalse(TEXT("Real DAT contains counted contract tasks"),KillContracts.IsEmpty());
    KillContracts.Sort([&](int32 A,int32 B){return Dat->GetContractInfo(A)->DescriptionProgress.Len()>Dat->GetContractInfo(B)->DescriptionProgress.Len();});
    for(int32 Count:{1,25,999})
    {
        Contracts.Reset();
        for(int32 Index=0;Index<FMath::Min(5,KillContracts.Num());++Index)
        {FACEContractEntry Entry;Entry.ContractId=KillContracts[Index];Entry.Stage=Count+4;Contracts.Add(Entry);}
        Refresh();
        for(float Scale:{1.f,1.5f})
        {
            CaptureScreen(FString::Printf(TEXT("GameplayContractKills%dScale%d"),Count,int32(Scale*100)),Scale);
            for(int32 ContractRow=0;ContractRow<StatusRows.Num() && ContractRow<RowIds.Num();++ContractRow)
            {
                if(StatusRows[ContractRow]->GetVisibility()==ESlateVisibility::Collapsed)continue;
                const auto* Info=Dat->GetContractInfo(RowIds[ContractRow]);
                auto* Label=CastChecked<UACERetailTextBlock>(StatusRows[ContractRow]);
                const FString Expected=Info->DescriptionProgress.Replace(TEXT("%d"),*FString::FromInt(Count));
                Test.TestEqual(TEXT("Contract count uses the retail DAT progress description"),Label->GetText().ToString(),Expected);
                const auto Cell=Label->GetRetailElement();
                const int32 Width=Cell->Width-int32(Cell->TextMargins.Left+Cell->TextMargins.Right);
                const auto Lines=ACEDatText::Layout(*Label->GetBitmapFont(),Expected,Width,false);
                Test.TestTrue(TEXT("Complete wrapped progress, including the count, fits vertically"),Lines.Num()*int32(Label->GetBitmapFont()->MaxCharHeight)<=Cell->Height);
                for(const auto& Line:Lines)Test.TestTrue(TEXT("Progress fits between the column margins"),Line.Width<=Width);
                Test.TestTrue(TEXT("Long progress leaves the contract name column intact"),Cell->X>=152);
                const auto ContractList=Manager->FindElementByName(TEXT("ContractsBox"));
                Test.TestTrue(TEXT("Count stays inside the list and away from its scrollbar"),Cell->GetScreenOrigin().X+Cell->Width<=ContractList->GetScreenOrigin().X+ContractList->Width);
                Test.TestEqual(TEXT("Contract status uses the authored retail font"),Cell->FontId,uint32(0x40000001));
            }
        }
        if(Count==999)
        {
            int32 VisibleRows=0;
            for(const auto& Label:StatusRows)if(Label->GetVisibility()!=ESlateVisibility::Collapsed)++VisibleRows;
            Test.TestTrue(TEXT("Scrolling wrapped contracts never skips an unseen entry"),ScrollNext()<=VisibleRows);
            CaptureScreen(TEXT("GameplayContractKillsScrolled"),1.f);
        }
    }
    Contracts=SavedContracts;Refresh();CaptureScreen(TEXT("GameplayContractsRestored"),1.f);
}

struct FACEContractUIFixture
{
    static FORCENOINLINE void Run(FAutomationTestBase& Test, UACEDatSubsystem* Dat,
        UACEUIGameplayBinder* Gameplay, UACEUIElementManager* Manager, UACEUICanvasWidget* Canvas,
        UACEUIResourceResolver* Resources, TArray<FACEContractEntry>& Contracts,
        TFunctionRef<void(const FString&,float)> CaptureScreen)
    {
        Contracts.Reset();
        for (int32 Id=1;Id<1000 && Contracts.Num()<2;++Id)
        {
            if (!Dat->GetContractInfo(Id)) continue;
            FACEContractEntry Entry;Entry.ContractId=Id;Entry.Stage=2;
            Entry.TimeWhenDone=3600;Entry.ReceivedAt=FPlatformTime::Seconds()-60;
            Contracts.Add(Entry);
        }
        Gameplay->ShowPanelPage(TEXT("QuestManagementPanel_Field"));
        Gameplay->RefreshQuestOverlays();CaptureScreen(TEXT("GameplayContracts"),1.f);
        Test.TestFalse(TEXT("Personal notes do not overlap the contracts page"),bool(Manager->FindElementByName(TEXT("JournalPage"))->bVisible));
        Test.TestEqual(TEXT("Journal displays received contracts"),Contracts.Num(),2);
        if (Gameplay->QuestRows.Num()>1)
        {
            const auto NameElement=Gameplay->ContractRowElements[0]->Children[0];
            FACEDatFont ContractFont;Resources->ResolveFont(NameElement->FontId,ContractFont);
            Test.TestEqual(TEXT("Journal truncates only the contract name within its retail column"),Gameplay->QuestRows[0]->GetText().ToString(),
                ACEDatText::Ellipsize(ContractFont,Dat->GetContractInfo(Gameplay->QuestRowIds[0])->Name,
                    NameElement->Width-int32(NameElement->TextMargins.Left+NameElement->TextMargins.Right)));
            Test.TestEqual(TEXT("Journal text lets the list receive clicks"),Gameplay->QuestRows[0]->GetVisibility(),ESlateVisibility::HitTestInvisible);
            Test.TestEqual(TEXT("Journal shows the server stage in the status column"),Gameplay->QuestStatusRows[0]->GetText().ToString(),FString(TEXT("In Progress")));
            Test.TestTrue(TEXT("Journal countdown advances after the server snapshot"),Gameplay->QuestDetailLabels.ContainsByPredicate([](UTextBlock* Label)
                {return Label && Label->GetText().ToString().Contains(TEXT("00:59:"));}));
            const auto& RowGeometry=Gameplay->QuestStatusRows[1]->GetCachedGeometry();
            const FVector2D RowPoint=Canvas->GetCachedGeometry().AbsoluteToLocal(RowGeometry.LocalToAbsolute(RowGeometry.GetLocalSize()*.5));
            Test.TestTrue(TEXT("Clicking journal status selects its contract"),Gameplay->TryHandleOverlayClick(RowPoint));
            Test.TestEqual(TEXT("Journal detail follows the clicked second row"),Gameplay->SelectedContractId,Gameplay->QuestRowIds[1]);
        }
        if (Contracts.Num()==2 && Gameplay->QuestRowIds.Num()>=2)
        {
            const TArray<FACEContractEntry> SavedContracts=Contracts;
            const int32 FirstByName=Gameplay->QuestRowIds[0],SecondByName=Gameplay->QuestRowIds[1];
            const int32 Selection=Gameplay->SelectedContractId;
            Gameplay->HandleNamedClick(TEXT("ContractNameSortButton"));
            Test.TestTrue(TEXT("Clicking active name heading reverses order without changing selection"),
                Gameplay->QuestRowIds[0]==SecondByName && Gameplay->QuestRowIds[1]==FirstByName
                && Gameplay->SelectedContractId==Selection);
            Gameplay->HandleNamedClick(TEXT("ContractNameSortButton"));
            Test.TestEqual(TEXT("Clicking name heading again restores ascending order"),Gameplay->QuestRowIds[0],FirstByName);

            Contracts[0].Stage=Contracts[1].Stage=3;
            Contracts[0].TimeWhenDone=Contracts[1].TimeWhenDone=0.0;
            Contracts[0].ReceivedAt=Contracts[1].ReceivedAt=0.0;
            Contracts[0].TimeWhenRepeats=120.0; Contracts[1].TimeWhenRepeats=600.0;
            Gameplay->HandleNamedClick(TEXT("ContractStatusSortButton"));
            Test.TestTrue(TEXT("Status orders repeat timers numerically, not by text or contract ID"),
                Gameplay->QuestRowIds[0]==Contracts[0].ContractId && Gameplay->QuestRowIds[1]==Contracts[1].ContractId);
            Test.TestEqual(TEXT("Repeatable completed contract shows retail countdown"),Gameplay->QuestStatusRows[0]->GetText().ToString(),FString(TEXT("Done (2m 0s to Repeat)")));
            CaptureScreen(TEXT("GameplayContractRepeatTimers"),1.f);
            Gameplay->HandleNamedClick(TEXT("ContractStatusSortButton"));
            Test.TestEqual(TEXT("Second status click reverses numeric countdown order"),Gameplay->QuestRowIds[0],Contracts[1].ContractId);
            Gameplay->HandleNamedClick(TEXT("ContractNameSortButton"));
            Test.TestTrue(TEXT("Changing sort heading resets reverse order"),!Gameplay->bQuestReverseSort && Gameplay->QuestRowIds[0]==FirstByName);

            // Age only the local receipt time; no server event is required at expiry.
            Contracts.SetNum(1); Contracts[0].TimeWhenRepeats=60.; Contracts[0].ReceivedAt=FPlatformTime::Seconds()-61.;
            Gameplay->RefreshQuestOverlays();
            Test.TestEqual(TEXT("Elapsed repeat timer becomes Available locally"),Gameplay->QuestStatusRows[0]->GetText().ToString(),FString(TEXT("Available")));
            Test.TestEqual(TEXT("Presentation does not rewrite the server stage"),Contracts[0].Stage,3);
            Contracts[0].ReceivedAt=0.; Contracts[0].TimeWhenRepeats=2678405.;
            Gameplay->RefreshQuestOverlays();
            Test.TestEqual(TEXT("Long repeat timer uses retail month/day units"),Gameplay->QuestStatusRows[0]->GetText().ToString(),FString(TEXT("Done (1mo 1d 5s to Repeat)")));

            int32 Repeatable=0,OneTime=0;
            for (int32 Id=1;Id<1000 && (!Repeatable || !OneTime);++Id)
                if (const auto* Info=Dat->GetContractInfo(Id))
                {
                    if (!Info->QuestFlagRepeatTime.IsEmpty()) Repeatable=Id;
                    else OneTime=Id;
                }
            Test.TestTrue(TEXT("Retail DAT supplies repeatable and one-time contract flags"),Repeatable!=0 && OneTime!=0);
            for (int32 Id : {Repeatable,OneTime}) if (Id)
            {
                Contracts[0].ContractId=Id; Contracts[0].TimeWhenRepeats=0.;
                Gameplay->RefreshQuestOverlays();
                Test.TestEqual(TEXT("Untimed completion respects DAT repeatability"),Gameplay->QuestStatusRows[0]->GetText().ToString(),
                    FString(Id==Repeatable?TEXT("Available"):TEXT("Done")));
            }
            Contracts=SavedContracts; Gameplay->RefreshQuestOverlays();
            Contracts[0].Stage=2; Contracts[1].Stage=1;
            Gameplay->HandleNamedClick(TEXT("ContractStatusSortButton"));
            Test.TestEqual(TEXT("Status sort follows displayed status instead of raw stage descending"),Gameplay->QuestRowIds[0],Contracts[1].ContractId);
            Contracts=SavedContracts; Gameplay->HandleNamedClick(TEXT("ContractNameSortButton"));
        }
        CheckContractProgress(Test,Dat,Manager,Contracts,Gameplay->QuestRowIds,Gameplay->QuestStatusRows,
            [&](){Gameplay->RefreshQuestOverlays();},[&](const FString& Name,float Scale){CaptureScreen(Name,Scale);},
            [&](){Gameplay->ScrollQuestList(-1.f);return Gameplay->QuestScrollOffset;});
    }
};

struct FACERetailVendorUIFixture
{
    static FORCENOINLINE void CheckAll(FAutomationTestBase& Test,FACESession& Session,
        UACEClientSubsystem* Client,UACEUIGameplayBinder* Gameplay,UACEUICanvasWidget* Canvas,
        UACEUIElementManager* Manager,UACEUIResourceResolver* Resources,const FACEWorldObject& Player,
        const FACEWorldObject& Comp,int32 ComponentWcid,const TMap<int32,int32>& SavedDesired,
        TFunctionRef<void(const FString&,float)> Capture,TFunctionRef<void(const TCHAR*)> NativeClick,
        TFunctionRef<bool(uint32)> HasAction)
    {
        auto CaptureScreen=[&](const FString& Name,float Scale=1.f){Capture(Name,Scale);};
        Session.DesiredComponents[ComponentWcid]=100;
        const auto SavedStock=Session.VendorMerchandise;
        const int32 SavedVendor=Gameplay->OpenVendorGuid;
        const auto SavedCart=Gameplay->VendorBuyCart;
        const float SavedRate=Session.VendorSellRate;
        FACEWorldObject Stock=Comp;Stock.Guid=99121;Stock.ContainerId=99122;Stock.StackSize=1;Stock.Value=10;
        Stock.VendorQuantityAvailable=-1;Session.VendorMerchandise={Stock};Session.WorldObjects.Add(Stock.Guid,Stock);
        Session.VendorSellRate=1;Gameplay->OpenVendorGuid=99122;Gameplay->VendorBuyCart={{10,Stock.Guid}};
        {
            const auto SavedSelection=Gameplay->LastSelection;
            const auto SavedClientSelection=Session.SelectedObject;
            const int32 SavedSelectedVendor=Gameplay->VendorSelectedGuid;
            const int32 SavedSessionVendor=Session.OpenVendorGuid;
            Session.OpenVendorGuid=99122;
            FACEWorldObject Helmet=Stock;Helmet.Guid=99123;Helmet.ItemType=ACEItemType::Armor;Helmet.MaxStackSize=0;
            Session.VendorMerchandise.Add(Helmet);Session.WorldObjects.Add(Helmet.Guid,Helmet);
            FACESelectedObject Pick;Pick.bValid=true;Pick.Guid=Helmet.Guid;
            Session.SelectedObject=Pick;Gameplay->VendorSelectedGuid=Helmet.Guid;Gameplay->HandleSelectionChanged(Pick);
            Test.TestEqual(TEXT("Unlimited helmets have no stack quantity slider"),Gameplay->SelectedStackMax,1);
            Gameplay->SelectedStackAmount=100;Gameplay->VendorBuyCart.Reset();Gameplay->AddSelectedVendorItemToBuyCart();
            Test.TestEqual(TEXT("A helmet can only be added as one item"),Gameplay->VendorBuyCart[0].Key,1);
            Gameplay->AddSelectedVendorItemToBuyCart();
            Test.TestEqual(TEXT("Repeated add does not turn helmets into a stack"),Gameplay->VendorBuyCart[0].Key,1);
            Gameplay->SetVendorPage(0);
            Session.VendorMerchandise[0].MaxStackSize=250;Session.VendorMerchandise[0].VendorQuantityAvailable=17;
            Pick.Guid=Stock.Guid;Session.SelectedObject=Pick;Gameplay->VendorSelectedGuid=Stock.Guid;Gameplay->HandleSelectionChanged(Pick);
            Test.TestEqual(TEXT("Quantity follows current vendor stock, not the stale object cache"),Gameplay->SelectedStackMax,17);
            Test.TestEqual(TEXT("Retail consumable stock starts at one unit"),Gameplay->SelectedStackAmount,1);
            const FString SavedCurrency=Session.VendorCurrencyName;
            const int32 SavedCurrencyCount=Session.VendorCurrencyCount;
            const uint32 SavedCurrencyWeenie=Session.VendorCurrencyWeenie;
            Session.VendorCurrencyWeenie=76542;Session.VendorCurrencyName=TEXT("Stipends");Session.VendorCurrencyCount=10;
            FACEWorldObject Stipends;Stipends.Guid=99225;Stipends.WeenieClassId=76542;Stipends.ContainerId=Session.PlayerGuid;
            Stipends.StackSize=10;Stipends.MaxStackSize=100;Session.UpsertWorldObject(Stipends);
            Gameplay->HandleVendorOpened(99122);
            if(Test.TestNotNull(TEXT("Vendor currency caption exists"),Gameplay->VendorItemCostLabel.Get()))
            {
                Test.TestTrue(TEXT("Vendor displays its alternate currency"),Gameplay->VendorItemCostLabel->GetText().ToString().Contains(TEXT("Stipends. You have 10")));
                Session.VendorCurrencyCount=9;
                FACEBinaryWriter Balance;Balance.WriteUInt8(1);Balance.WriteUInt32(Stipends.Guid);Balance.WriteUInt32(9);Balance.WriteUInt32(9);
                FACEBinaryReader BalanceUpdate(Balance.GetData());Session.HandleSetStackSize(BalanceUpdate);Gameplay->HandleVendorOpened(99122);
                Test.TestTrue(TEXT("A balance-only vendor reply refreshes the caption"),Gameplay->VendorItemCostLabel->GetText().ToString().Contains(TEXT("Stipends. You have 9")));
            }
            Session.WorldObjects.Remove(Stipends.Guid);++Session.InventoryDataRevision;
            Session.VendorCurrencyWeenie=76543;Session.VendorCurrencyName=TEXT("Grisly Meats");
            Session.VendorCurrencyCount=9;
            FACEWorldObject Currency;Currency.Guid=99224;Currency.WeenieClassId=76543;
            Currency.ContainerId=Session.PlayerGuid;Currency.StackSize=9;Currency.MaxStackSize=100;
            Session.WorldObjects.Add(Currency.Guid,Currency);++Session.InventoryDataRevision;
            Gameplay->RefreshVendorOverlays();
            Test.TestTrue(TEXT("Trophy vendor displays owned currency"),Gameplay->VendorItemCostLabel->GetText().ToString().Contains(TEXT("You have 9")));
            FACEBinaryWriter Spent;Spent.WriteUInt8(1);Spent.WriteUInt32(Currency.Guid);Spent.WriteUInt32(5);Spent.WriteUInt32(5);
            FACEBinaryReader SpentReader(Spent.GetData());Session.HandleSetStackSize(SpentReader);
            Gameplay->RefreshVendorOverlays();
            Test.TestTrue(TEXT("Purchase stack update refreshes count without another trade"),Gameplay->VendorItemCostLabel->GetText().ToString().Contains(TEXT("You have 5")));
            Test.TestEqual(TEXT("Stale ApproachVendor count cannot restore spent currency"),Session.GetVendorCurrencyCount(),5);
            Session.WorldObjects.Remove(Currency.Guid);++Session.InventoryDataRevision;
            Test.TestEqual(TEXT("Final currency removal reports zero"),Session.GetVendorCurrencyCount(),0);
            Session.VendorCurrencyWeenie=SavedCurrencyWeenie;
            Session.VendorCurrencyName=SavedCurrency;Session.VendorCurrencyCount=SavedCurrencyCount;
            Gameplay->VendorBuyCart.Reset();Gameplay->AddSelectedVendorItemToBuyCart();
            Test.TestEqual(TEXT("Vendor Add transfers the default one unit"),Gameplay->VendorBuyCart[0].Key,1);
            Gameplay->SetVendorPage(0);
            Gameplay->SelectedStackAmount=5;Gameplay->HandleSelectionChanged(Pick);
            Test.TestEqual(TEXT("Selection refresh preserves an explicitly split quantity"),Gameplay->SelectedStackAmount,5);
            Gameplay->VendorBuyCart.Reset();Gameplay->AddSelectedVendorItemToBuyCart();
            Test.TestEqual(TEXT("Vendor Add respects the user's quantity adjustment"),Gameplay->VendorBuyCart[0].Key,5);
            Gameplay->SetVendorPage(0);
            Session.VendorMerchandise[0].VendorQuantityAvailable=-1;Gameplay->HandleVendorOpened(99122);
            Test.TestEqual(TEXT("Unlimited ammunition is limited to its real stack size"),Gameplay->SelectedStackMax,250);
            FACERetailVendorUIFixture::Check(Test,Session,Gameplay,Canvas,Stock,Pick,
                [&](const TCHAR* Name){CaptureScreen(Name);},[&](const TCHAR* Name){NativeClick(Name);},HasAction);
            Session.VendorMerchandise[0].VendorQuantityAvailable=0;Gameplay->HandleVendorOpened(99122);
            Test.TestEqual(TEXT("Sold-out stack hides quantity control"),Gameplay->SelectedStackMax,0);
            Session.CachedC2SPackets.Reset();Gameplay->BuySelectedVendorItem();
            Session.SendBuyItems(99122,{{100,Stock.Guid}});
            Test.TestFalse(TEXT("Stale cart cannot buy exhausted vendor stock"),HasAction(ACEGameAction::Buy));
            // Retail trade notes use fixed rates and a one-unit initial split,
            // even where the vendor's ordinary markup is much higher.
            FACEWorldObject Note=Stock;Note.Name=TEXT("Trade Note (250,000)");Note.PluralName=TEXT("Trade Notes (250,000)");
            Note.ItemType=ACEItemType::PromissoryNote;Note.Value=250000;Note.StackSize=1;Note.MaxStackSize=250;
            Session.VendorMerchandise={Note};Session.WorldObjects[Note.Guid]=Note;Session.VendorSellRate=1.55f;
            Session.VendorCurrencyName.Empty();Gameplay->LastSelection={};
            Gameplay->HandleVendorOpened(99122);Gameplay->HandleSelectionChanged(Pick);Gameplay->SetVendorPage(0);
            Test.TestEqual(TEXT("Trade notes start at one, not the 250-unit stock limit"),Gameplay->SelectedStackAmount,1);
            Test.TestEqual(TEXT("Toolbar describes stock independently of the chosen split"),Gameplay->SelectionText->GetText().ToString(),FString(TEXT("250 Trade Notes (250,000)")));
            Test.TestEqual(TEXT("Vendor uses the singular stock name without a quantity prefix"),Gameplay->VendorItemNameLabel->GetText().ToString(),Note.Name);
            Test.TestTrue(TEXT("Trade note purchase uses retail fixed 15-percent markup"),Gameplay->VendorItemCostLabel->GetText().ToString().Contains(TEXT("287,500p")));
            Gameplay->VendorBuyCart.Reset();Gameplay->AddSelectedVendorItemToBuyCart();
            Gameplay->SetVendorPage(1);
            Test.TestTrue(TEXT("Buy cart uses the same trade note price"),Gameplay->VendorBuyCostLabel->GetText().ToString().Contains(TEXT("287,500")));
            Gameplay->VendorSellCart={{1,Note.Guid}};Gameplay->SetVendorPage(2);
            Test.TestTrue(TEXT("Trade note sells for its full face value"),Gameplay->VendorSellCostLabel->GetText().ToString().Contains(TEXT("250,000p")));
            {
                const uint32 Before=Session.NextGameActionSequence;
                Gameplay->InvDragGuid=Note.Guid;Gameplay->bInvDragPending=true;Gameplay->bInvDragActive=true;
                Gameplay->bInvDragFromVendorSell=true;
                Gameplay->TryFinishInventoryDrag(FVector2D(-1000,-1000));
                Test.TestTrue(TEXT("Dragging a sell row out removes that offer"),Gameplay->VendorSellCart.IsEmpty());
                Test.TestEqual(TEXT("Removing a sell offer does not drop or move the actual item"),Session.NextGameActionSequence,Before);
                auto OtherNote=Note;OtherNote.Guid=99129;Session.WorldObjects.Add(OtherNote.Guid,OtherNote);
                Gameplay->VendorSellCart={{1,Note.Guid},{1,OtherNote.Guid}};
                Gameplay->InvDragGuid=Note.Guid;Gameplay->bInvDragPending=true;Gameplay->bInvDoubleClickPending=true;
                Gameplay->bInvDragFromVendorSell=true;
                Gameplay->TryFinishInventoryDrag(FVector2D(-1000,-1000));
                Test.TestEqual(TEXT("Double-click removes only the selected sell offer"),Gameplay->VendorSellCart.Num(),1);
                if(Gameplay->VendorSellCart.Num()==1)Test.TestEqual(TEXT("Other sell offers remain in place"),Gameplay->VendorSellCart[0].Value,OtherNote.Guid);
                Session.WorldObjects.Remove(OtherNote.Guid);
                Gameplay->VendorSellCart={{1,Note.Guid}};
            }
            FACEWorldObject Ammo=Stock;Ammo.ItemType=ACEItemType::MissileWeapon;Ammo.MaxStackSize=250;Ammo.VendorQuantityAvailable=17;
            Session.VendorMerchandise={Ammo};Session.WorldObjects[Ammo.Guid]=Ammo;Gameplay->LastSelection={};
            Gameplay->HandleVendorOpened(99122);Gameplay->HandleSelectionChanged(Pick);Gameplay->SetVendorPage(0);
            Test.TestEqual(TEXT("Retail ammunition selects the available stack instead"),Gameplay->SelectedStackAmount,17);
            Gameplay->SelectedStackAmount=5;Gameplay->VendorBuyCart.Reset();Gameplay->AddSelectedVendorItemToBuyCart();
            Test.TestEqual(TEXT("Ammo Add honors an explicit split"),Gameplay->VendorBuyCart[0].Key,5);
            Gameplay->VendorSellCart.Reset();Session.VendorSellRate=1;
            Session.VendorCurrencyName=SavedCurrency;Session.WorldObjects[Stock.Guid]=Stock;
            Session.VendorMerchandise={Stock};Session.WorldObjects.Remove(Helmet.Guid);
            Gameplay->VendorBuyCart={{10,Stock.Guid}};Gameplay->VendorSelectedGuid=SavedSelectedVendor;
            Session.SelectedObject=SavedClientSelection;Gameplay->HandleSelectionChanged(SavedSelection);Session.OpenVendorGuid=SavedSessionVendor;
        }
        {
            FACEWorldObject Pack;Pack.Guid=99130;Pack.ContainerId=Player.Guid;Pack.ItemType=ACEItemType::Container;Pack.ItemsCapacity=24;Pack.Attuned=1;
            FACEWorldObject Bread;Bread.Guid=99131;Bread.ContainerId=Pack.Guid;Bread.ItemType=ACEItemType::Food;Bread.StackSize=12;Bread.Value=120;
            FACEWorldObject Retained=Bread;Retained.Guid=99132;Retained.ObjectDescriptionFlags=ACEObjectDescFlag::Retained;
            FACEWorldObject Bound=Bread;Bound.Guid=99133;Bound.Attuned=1;
            for(const auto& Object:{Pack,Bread,Retained,Bound})Session.WorldObjects.Add(Object.Guid,Object);
            Gameplay->VendorSellCart={{2,Bread.Guid}};
            Session.CachedC2SPackets.Reset();Gameplay->AddInventoryGuidToVendorSellCart(Pack.Guid);
            Test.TestEqual(TEXT("Pack drop adds only sellable contents, never the pack"),Gameplay->VendorSellCart.Num(),1);
            Test.TestEqual(TEXT("Pack drop restores full stack quantity"),Gameplay->VendorSellCart[0].Key,12);
            Test.TestEqual(TEXT("Pack contents open the Sell tab"),Gameplay->ActiveVendorPage,2);
            Test.TestFalse(TEXT("Pack drop stages contents without selling them"),HasAction(ACEGameAction::Sell));
            Gameplay->AddInventoryGuidToVendorSellCart(Pack.Guid);
            Test.TestEqual(TEXT("Repeated pack drops do not duplicate sale entries"),Gameplay->VendorSellCart.Num(),1);
            Gameplay->ShowVendorPanel(99122);CaptureScreen(TEXT("GameplayBulkVendor"));
            TFunction<void()> VerifyVendorDropFeedback = [&]()
            {
                const FVector2D SavedCursor=FSlateApplication::Get().GetCursorPos();
                ON_SCOPE_EXIT { FSlateApplication::Get().SetCursorPos(SavedCursor); };
                for (float Scale : {1.f, 1.5f})
                {
                    CaptureScreen(TEXT("VendorDragLayout"),Scale);
                    auto Preview = [&](int32 Guid, bool Accept, int32 CellIndex)
                    {
                        auto* Cell=Gameplay->VendorItemSlotBgs[CellIndex].Get();
                        const auto& Geo=Cell->GetCachedGeometry();
                        const FVector2D Point=Canvas->GetCachedGeometry().AbsoluteToLocal(Geo.LocalToAbsolute(Geo.GetLocalSize()*.5));
                        FSlateApplication::Get().SetCursorPos(Geo.LocalToAbsolute(Geo.GetLocalSize()*.5));
                        Gameplay->InvDragGuid=Guid;Gameplay->bInvDragPending=true;Gameplay->bInvDragActive=true;
                        Gameplay->InvDragIconDid=0x06004CF7u;
                        Gameplay->UpdateInventoryDrag(Point);
                        auto* Marker=Gameplay->PaperDollDragTargetIcon.Get();
                        if (!Test.TestNotNull(TEXT("Vendor cell has drag feedback"),Marker)) return;
                        Test.TestTrue(TEXT("Vendor feedback is visible"),Marker->IsVisible());
                        Test.TestTrue(TEXT("Vendor drag uses the retail green circle or red blocked artwork"),
                            Marker->Background.GetResourceObject()==Resources->ResolveIconTexture(Accept?0x060011F9:0x060011F8));
                        auto* MarkerSlot=Cast<UCanvasPanelSlot>(Marker->Slot);
                        auto* CellSlot=Cast<UCanvasPanelSlot>(Cell->Slot);
                        Test.TestEqual(TEXT("Vendor marker snaps to hovered cell"),MarkerSlot->GetPosition(),CellSlot->GetPosition());
                        Test.TestEqual(TEXT("Vendor marker matches scaled cell size"),MarkerSlot->GetSize(),CellSlot->GetSize());
                    };
                    Preview(Player.Guid,true,0);
                    Preview(Pack.Guid,true,1); // empty cells are also drop targets
                    Preview(Bread.Guid,true,0);
                    CaptureScreen(TEXT("VendorAcceptDrop"),Scale);
                    Preview(Retained.Guid,false,1);
                    CaptureScreen(TEXT("VendorRejectDrop"),Scale);
                    Preview(Bound.Guid,false,0);
                    const int32 Count=Gameplay->VendorSellCart.Num();
                    Gameplay->AddInventoryGuidToVendorSellCart(Retained.Guid);
                    Test.TestEqual(TEXT("Blocked item cannot be staged on drop"),Gameplay->VendorSellCart.Num(),Count);
                    const uint32 Types=Session.VendorItemTypes;
                    Session.VendorItemTypes=ACEItemType::Armor; Preview(Bread.Guid,false,0);
                    Session.VendorItemTypes=Types;
                    Gameplay->UpdateInventoryDrag(FVector2D(500,200));
                    Test.TestFalse(TEXT("Leaving vendor cells hides drag feedback"),Gameplay->PaperDollDragTargetIcon->IsVisible());
                    Preview(Player.Guid,true,0);
                    Gameplay->CancelInventoryDrag();
                    Test.TestFalse(TEXT("Canceling drag clears vendor feedback"),Gameplay->PaperDollDragTargetIcon->IsVisible());
                }
                CaptureScreen(TEXT("GameplayBulkVendor"));
            };
            VerifyVendorDropFeedback();
            const auto SellList=Manager->FindElementUnder(TEXT("VendorSellPage"),TEXT("VendorSellList"));
            if(Test.TestTrue(TEXT("Vendor sell list is a real drop target"),SellList.IsValid()))
            {
                auto DropPack=[&](int32 Guid)
                {
                    Gameplay->InvDragGuid=Guid;Gameplay->InvDragSourcePack=Player.Guid;
                    Gameplay->bInvDragPending=true;Gameplay->bInvDragActive=true;
                    Gameplay->TryFinishInventoryDrag(FVector2D(SellList->GetScreenOrigin())+FVector2D(15,15));
                };
                Gameplay->VendorSellCart.Reset();DropPack(Pack.Guid);
                Test.TestTrue(TEXT("Native pack drop reaches the sell cart"),Gameplay->VendorSellCart.ContainsByPredicate([&](auto& P){return P.Value==Bread.Guid;}));
                Session.WorldObjects[Bread.Guid].ContainerId=Player.Guid;
                const auto SavedSelf=Session.WorldObjects[Player.Guid];
                Session.WorldObjects[Player.Guid].ItemsCapacity=0;
                Session.WorldObjects[Player.Guid].ItemType=ACEItemType::Creature;
                Gameplay->VendorSellCart.Reset();DropPack(Player.Guid);
                Test.TestTrue(TEXT("Main backpack bulk drop works without container metadata on the player"),Gameplay->VendorSellCart.ContainsByPredicate([&](auto& P){return P.Value==Bread.Guid;}));
                Test.TestFalse(TEXT("Main pack bulk drop never offers the player"),Gameplay->VendorSellCart.ContainsByPredicate([&](auto& P){return P.Value==Player.Guid;}));
                Session.WorldObjects[Player.Guid]=SavedSelf;
                Session.WorldObjects[Bread.Guid].ContainerId=Pack.Guid;
            }
            const uint32 SavedTypes=Session.VendorItemTypes;
            Session.VendorItemTypes=ACEItemType::Armor;Gameplay->VendorSellCart.Reset();
            Gameplay->AddInventoryGuidToVendorSellCart(Pack.Guid);
            Test.TestTrue(TEXT("Bulk sale skips item types the vendor does not buy"),Gameplay->VendorSellCart.IsEmpty());
            Session.VendorItemTypes=SavedTypes;
            Session.VendorMaxValue=9;Gameplay->AddInventoryGuidToVendorSellCart(Pack.Guid);
            Test.TestTrue(TEXT("Bulk sale respects vendor per-unit value limits"),Gameplay->VendorSellCart.IsEmpty());
            Session.VendorMaxValue=-1;
            Client->SelectObject(0);
            Gameplay->HandleEscape();
            Test.TestEqual(TEXT("Escape dismisses an open vendor"),Gameplay->OpenVendorGuid,0);
            for(const auto& Object:{Pack,Bread,Retained,Bound})Session.WorldObjects.Remove(Object.Guid);
            Gameplay->OpenVendorGuid=99122;Gameplay->VendorBuyCart={{10,Stock.Guid}};
        }
        Session.CachedC2SPackets.Reset();Gameplay->TryDispatchChatCommand(TEXT("/fillcomps"));
        Test.TestEqual(TEXT("Refill subtracts carried and already-cart quantities"),Gameplay->VendorBuyCart[0].Key,60);
        Test.TestFalse(TEXT("Refill never purchases without the Buy button"),HasAction(ACEGameAction::Buy));
        Gameplay->TryDispatchChatCommand(TEXT("@fillcomps"));
        Test.TestEqual(TEXT("Repeated refill does not overbuy"),Gameplay->VendorBuyCart[0].Key,60);
        Gameplay->VendorBuyCart.Reset();Gameplay->TryDispatchChatCommand(TEXT("/fillcomps 599"));
        Test.TestTrue(TEXT("Price cap preserves whole-component refill limit"),Gameplay->VendorBuyCart.IsEmpty());
        Session.VendorMerchandise[0].VendorQuantityAvailable=20;Gameplay->TryDispatchChatCommand(TEXT("/fillcomps"));
        Test.TestEqual(TEXT("Finite vendor supply caps cart quantity"),Gameplay->VendorBuyCart[0].Key,20);
        Gameplay->TryDispatchChatCommand(TEXT("/fillcomps clear"));
        Test.TestTrue(TEXT("Clear removes saved refill quantities"),Session.DesiredComponents.IsEmpty());
        Session.DesiredComponents=SavedDesired;Session.VendorMerchandise=SavedStock;Session.VendorSellRate=SavedRate;
        Gameplay->OpenVendorGuid=SavedVendor;Gameplay->VendorBuyCart=SavedCart;
        Session.WorldObjects.Remove(Comp.Guid);Session.WorldObjects.Remove(Stock.Guid);
    }

    static FORCENOINLINE void Check(FAutomationTestBase& Test,FACESession& Session,
        UACEUIGameplayBinder* Gameplay,UACEUICanvasWidget* Canvas,const FACEWorldObject& Stock,
        const FACESelectedObject& Pick,TFunctionRef<void(const TCHAR*)> CaptureScreen,
        TFunctionRef<void(const TCHAR*)> NativeClick,TFunctionRef<bool(uint32)> HasAction)
    {
        TGuardValue<TMap<int32,int32>> SavedVendorQualities(Session.PlayerVitals.StatQualityInts,Session.PlayerVitals.StatQualityInts);
        // Custom weenies use the same server stock and pyreal purchase path.
        // Exercise actual pointer routing, not only the binder methods.
        [&]()
        {
        Session.VendorMerchandise[0].WeenieClassId=1050067;
        Session.VendorCurrencyName.Empty();
        Gameplay->VendorBuyCart.Reset();Gameplay->SetVendorPage(0);Gameplay->HandleVendorOpened(99122);
        Gameplay->SetVendorItemTypeFilter(10); // retail Spell Components category
        CaptureScreen(TEXT("GameplayCustomPyrealVendor"));
        {
            const auto& Geo=Gameplay->VendorItemSlots[0]->GetCachedGeometry();
            const FVector2D Point=Geo.GetAbsolutePosition()+Geo.GetAbsoluteSize()*.5;
            Canvas->NativeOnMouseButtonDown(Canvas->GetCachedGeometry(),FPointerEvent(0,Point,Point,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
            Canvas->NativeOnMouseButtonUp(Canvas->GetCachedGeometry(),FPointerEvent(0,Point,Point,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
        }
        CheckVendorFundsUI(Test,Session.PlayerVitals.StatQualityInts,Gameplay->TransientInfoLines,
            Gameplay->VendorItemCostLabel,[&](){Session.CachedC2SPackets.Reset();NativeClick(TEXT("VendorItemBuy_Button"));},
            [&](){Gameplay->RefreshVendorOverlays();},HasAction);
        Gameplay->SelectedStackAmount=1;Session.CachedC2SPackets.Reset();
        for(int32 Count=1;Count<=10;++Count)
        {
            NativeClick(TEXT("VendorItemAdd_Button"));
            CaptureScreen(TEXT("GameplayRepeatedVendorAdd"));
            Test.TestEqual(TEXT("Add to List keeps the retail Items page open"),Gameplay->ActiveVendorPage,0);
            Test.TestEqual(TEXT("Add to List keeps the item selected"),Gameplay->VendorSelectedGuid,Stock.Guid);
            Test.TestTrue(TEXT("Repeated Add to List accumulates one unit per click"),
                Gameplay->VendorBuyCart.ContainsByPredicate([&](const auto& P){return P.Value==Stock.Guid&&P.Key==Count;}));
        }
        Test.TestFalse(TEXT("Staging a cart does not purchase stock"),HasAction(ACEGameAction::Buy));
        NativeClick(TEXT("VendorBuyTab"));
        Test.TestEqual(TEXT("Buying tab still explicitly opens the staged cart"),Gameplay->ActiveVendorPage,1);
        Gameplay->VendorBuyCart.Reset();Gameplay->SetVendorPage(0);
        }();
        // Shrine of Knowledge (43370020) advertises each gem with a zero
        // create-list quantity and charges MMDs. Replay the server packet:
        // retail BuySingleItem still buys one, rather than treating it as sold out.
        [&]()
        {
            const auto PreviousStock=Session.VendorMerchandise;
            const auto PreviousContents=Session.ContainerContents.FindRef(99122);
            TGuardValue<uint32> CurrencyWeenieGuard(Session.VendorCurrencyWeenie,20630u);
            TGuardValue<FString> CurrencyNameGuard(Session.VendorCurrencyName,TEXT("Trade Notes (250,000)"));
            TGuardValue<int32> CurrencyCountGuard(Session.VendorCurrencyCount,10);
            FACEWorldObject Notes;Notes.Guid=99226;Notes.WeenieClassId=20630;
            Notes.ContainerId=Session.PlayerGuid;Notes.StackSize=10;Notes.MaxStackSize=250;
            Session.UpsertWorldObject(Notes);
            ON_SCOPE_EXIT { Session.WorldObjects.Remove(Notes.Guid);++Session.InventoryDataRevision; };
            TGuardValue<float> BuyRateGuard(Session.VendorBuyRate,.9f);
            TGuardValue<uint32> ItemTypesGuard(Session.VendorItemTypes,4481568u);
            TGuardValue<int32> MinValueGuard(Session.VendorMinValue,0);
            TGuardValue<int32> MaxValueGuard(Session.VendorMaxValue,100000);
            auto ReceiveStock=[&](uint32 Supply,uint16 MaxStackSize)
            {
                FACEBinaryWriter Wire;
                Wire.WriteUInt32(99122);Wire.WriteUInt32(4481568);
                Wire.WriteUInt32(0);Wire.WriteUInt32(100000);Wire.WriteUInt32(1);
                Wire.WriteFloat(.9f);Wire.WriteFloat(1.f);Wire.WriteUInt32(20630);
                Wire.WriteUInt32(10);Wire.WriteString16L(TEXT("Trade Notes (250,000)"));
                Wire.WriteUInt32(1);Wire.WriteUInt32(0xFF000000u|(Supply&0xFFFFFFu));
                Wire.WriteUInt32(Stock.Guid);Wire.WriteUInt32(0x00003008u); // value, stack, max stack
                Wire.WriteString16L(TEXT("Gem of Raising Strength"));
                Wire.WriteUInt16(22948);Wire.WriteUInt16(0x10E8);
                Wire.WriteUInt32(ACEItemType::Gem);Wire.WriteUInt32(0);Wire.Align();
                Wire.WriteUInt32(1);Wire.WriteUInt16(1);Wire.WriteUInt16(MaxStackSize);Wire.Align();
                FACEBinaryReader Reader(Wire.GetData());Session.HandleApproachVendor(Reader);
                Test.TestEqual(TEXT("Custom vendor packet retains its catalog entry"),Session.VendorMerchandise.Num(),1);
                Gameplay->VendorBuyCart.Reset();Gameplay->VendorSelectedGuid=Stock.Guid;
                Gameplay->LastSelection={};Session.SelectedObject=Pick;
                Gameplay->HandleVendorOpened(99122);Gameplay->HandleSelectionChanged(Pick);Gameplay->SetVendorPage(0);
            };
            ReceiveStock(0,0);
            Test.TestEqual(TEXT("Zero-quantity custom gem permits one retail purchase"),Gameplay->SelectedStackMax,1);
            CaptureScreen(TEXT("GameplayCustomZeroQuantityVendor"));
            Session.CachedC2SPackets.Reset();NativeClick(TEXT("VendorItemBuy_Button"));
            Test.TestTrue(TEXT("Zero-quantity vendor Buy button sends a purchase"),HasAction(ACEGameAction::Buy));
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader Wire(Packet.Value.Payload);Wire.Skip(16);
                if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;
                Wire.ReadUInt32();if(Wire.ReadUInt32()!=ACEGameAction::Buy)continue;
                Test.TestEqual(TEXT("Gem purchase addresses the open shrine"),Wire.ReadUInt32(),99122u);
                Test.TestEqual(TEXT("Gem purchase contains one line"),Wire.ReadUInt32(),1u);
                Test.TestEqual(TEXT("Gem purchase requests one unit"),Wire.ReadInt32(),1);
                Test.TestEqual(TEXT("Gem purchase uses server stock GUID"),Wire.ReadUInt32(),uint32(Stock.Guid));
            }
            NativeClick(TEXT("VendorItemAdd_Button"));
            Test.TestTrue(TEXT("Zero-quantity vendor Add to List stages one gem"),
                Gameplay->VendorBuyCart.ContainsByPredicate([&](const auto& P){return P.Value==Stock.Guid&&P.Key==1;}));
            Session.CachedC2SPackets.Reset();Gameplay->HandleNamedClick(TEXT("VendorBuyBuyAll_Button"));
            Test.TestTrue(TEXT("Zero-quantity vendor cart sends a purchase"),HasAction(ACEGameAction::Buy));
            ReceiveStock(0,250);
            Test.TestEqual(TEXT("Zero supply permits one, not an unlimited stack"),Gameplay->SelectedStackMax,1);
            ReceiveStock(17,250);
            Test.TestEqual(TEXT("Positive wire supply preserves its finite limit"),Gameplay->SelectedStackMax,17);
            ReceiveStock(0xFFFFFFu,250);
            Test.TestEqual(TEXT("Unlimited wire supply preserves its stack limit"),Gameplay->SelectedStackMax,250);
            Session.VendorMerchandise=PreviousStock;Session.WorldObjects[Stock.Guid]=Stock;
            Session.ContainerContents.FindOrAdd(99122)=PreviousContents;
            Gameplay->VendorBuyCart.Reset();
        }();
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailScreenTest,"ACE.RetailParity.UIScreens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACERetailScreenTest::RunTest(const FString& Parameters)
{
    // Offscreen passes represent separate frames; the HUD now coalesces same-frame callbacks.
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter);
    TGuardValue<FString> ScreenSettingsPath(GGameUserSettingsIni,FPaths::ProjectSavedDir()/TEXT("Automation/ScreenPreferencesFixture.ini"));
    FConfigFile ScreenPreferences;ScreenPreferences.NoSave=true;GConfig->SetFile(GGameUserSettingsIni,&ScreenPreferences);
    TestEqual(TEXT("Gameplay uses physical pixels at 1440p"),GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(FIntPoint(2560,1440)),1.f);
    TestEqual(TEXT("Retail base font is distinct from sans"),ACEUIFontStyles::FontIdForStyleName(TEXT("basefont16")),0x40000000u);
    TestEqual(TEXT("Retail sans font retains its own atlas"),ACEUIFontStyles::FontIdForStyleName(TEXT("sansfont16")),0x40000009u);
    TestEqual(TEXT("Named medium field font resolves without numeric suffix"),ACEUIFontStyles::FontIdForStyleName(TEXT("fieldvaluemedium")),0x40000000u);
    auto* GI=NewObject<UGameInstance>();
    GI->Init();
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    if (!Dat->LoadDatDirectory(ACERetailUIScreenTestsDatDirectory())) return false;
    auto* Resources=NewObject<UACEUIResourceResolver>(); Resources->Initialize(Dat);
    FACEDatFont BaseFont; Resources->ResolveFont(0x40000000u,BaseFont);
    bool bGlyphTint=false; Resources->ResolveFontAtlas(BaseFont.ForegroundSurfaceDataID,bGlyphTint);
    TestTrue(TEXT("Retail glyph atlases accept buff and debuff colors"),bGlyphTint);
    for (int32 Heritage : {1,12,13})
        TestTrue(TEXT("Retail paperdoll animation resolves from enum group 7"),(Resources->ResolvePaperDollAnimation(Heritage)>>24)==3);
    TestEqual(TEXT("Human paperdoll uses the independent DAT mapping"),Resources->ResolvePaperDollAnimation(1),0x030003C0u);
    TSet<uint32> BodyMasks;
    for (int32 Y=0; Y<256; ++Y) for (int32 X=0; X<256; ++X)
        if (const uint32 Mask = Resources->ResolvePaperDollSelectionMask(FIntPoint(X,Y))) BodyMasks.Add(Mask);
    TestEqual(TEXT("Authored paperdoll click map contains all nine body regions"), BodyMasks.Num(), 9);
    TestEqual(TEXT("Click outside the paperdoll map selects nothing"), Resources->ResolvePaperDollSelectionMask(FIntPoint(-1,0)), 0u);
    TestEqual(TEXT("Creature names resolve through retail enum mapper"), Resources->ResolveEnumString(0x10000005, 30), FString(TEXT("Skeleton")));
    TestEqual(TEXT("Creature enum preserves authored separators"), Resources->ResolveEnumString(0x10000005, 38), FString(TEXT("Fire_Elemental")));
    TestTrue(TEXT("Unrecognized creature enum stays absent"), Resources->ResolveEnumString(0x10000005, 9999).IsEmpty());
    TestEqual(TEXT("Weapon item background from retail DidMapper"),Resources->ResolveItemBackgroundId(1),0x060011CBu);
    TestEqual(TEXT("Armor background differs from weapon background"),Resources->ResolveItemBackgroundId(2),0x060011CFu);
    TestEqual(TEXT("Multi-type item uses lowest set type bit"),Resources->ResolveItemBackgroundId(3),0x060011CBu);
    TestEqual(TEXT("Unknown item background uses enum 33"),Resources->ResolveItemBackgroundId(0),0x060011D4u);
    TestNotNull(TEXT("Item background composite decodes"),Resources->ResolveItemBackground(1,0));
    auto* Manager=NewObject<UACEUIElementManager>(); Manager->Initialize();
    auto* Layout=NewObject<UACEUILayoutResolver>(); Layout->Initialize(Dat,Manager);
    if (!Layout->LoadLayout(0x21000004)) return false;
    TestEqual(TEXT("Character selection remains unscaled on larger screens"),Manager->GetCanvasScale(FVector2D(2560,1440)),FVector2D(1,1));
    const auto Enter=Manager->FindElementByName(TEXT("EnterGameButton"));
    if (!TestTrue(TEXT("Character-select Enter element exists"),Enter.IsValid())) return false;
    TestEqual(TEXT("Enter pressed state retains retail smaller font"),Enter->States[3].FontId,0x40000011u);
    auto* Canvas=NewObject<UACEUICanvasWidget>(); Canvas->Initialize();
    Canvas->InitializeCanvas(Manager); Canvas->SetResourceResolver(Resources);
    auto Slate=Canvas->TakeWidget();
    auto* Binder=NewObject<UACEUICharSelectBinder>();
    FACECharacterInfo First;First.CharacterId=1;First.Name=TEXT("Adventurer");
    FACECharacterInfo Second;Second.CharacterId=2;Second.Name=TEXT("Recreated");
    Binder->Initialize(nullptr,Manager,Canvas,nullptr,{First,Second},TEXT("Retail test"));
    Canvas->SetCharSelectBinder(Binder);
    const auto* OriginalLayer=Canvas->GetElementLayer();
    Canvas->RebuildWidget();
    TestTrue(TEXT("Viewport-to-VR reconstruction preserves the label layer"),OriginalLayer==Canvas->GetElementLayer());
    for (const auto& Name : Binder->CharNameImages)
        TestTrue(TEXT("Character names stay attached after Slate reconstruction"),Name && Name->GetParent()==Canvas->GetElementLayer());
    if (FApp::CanEverRender())
    {
        FWidgetRenderer Renderer(true,true);
        for (const FIntPoint Size : {FIntPoint(800,600),FIntPoint(1280,960),FIntPoint(1600,900)})
        {
            auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(Size),TF_Bilinear,true);
            for (int32 Pass=0; Pass<4; ++Pass)
            {
                Renderer.DrawWidget(Target,Slate,FVector2D(Size),0.f); FlushRenderingCommands();
                ++GFrameCounter; Canvas->NativeTick(Canvas->GetCachedGeometry(),0.f);
            }
            TArray<FColor> Pixels;
            Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            if(Size.X>800) TestTrue(TEXT("Unscaled character screen has an opaque black surround"),
                !Pixels.IsEmpty() && Pixels[0].R==0 && Pixels[0].G==0 && Pixels[0].B==0 && Pixels[0].A==255);
            TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(
                TEXT("Automation/RetailParity/CharSelect_%dx%d.png"),Size.X,Size.Y)));
            bool bFoundLabel=false;
            Canvas->WidgetTree->ForEachWidget([&](UWidget* Widget)
            {
                auto* Label=Cast<UACERetailTextBlock>(Widget);
                if (!Label || Label->GetText().ToString()!=TEXT("ENTER")) return;
                bFoundLabel=true;
                const auto* Slot=Cast<UCanvasPanelSlot>(Label->Slot);
                const FVector2D Expected=FVector2D(Enter->GetScreenOrigin())*Canvas->GetLastScale2D();
                TestTrue(TEXT("Enter text uses exactly the button origin at each resolution"),Slot && Slot->GetPosition().Equals(Expected,.01));
                TestTrue(TEXT("Enter text uses exactly the button extent"),Slot && Slot->GetSize().Equals(
                    FVector2D(Enter->Width,Enter->Height)*Canvas->GetLastScale2D(),.01));
            });
            TestTrue(TEXT("Character-select uses DAT text widget"),bFoundLabel);
        }
    }
    const auto List=Manager->FindElementByName(TEXT("CharacterListBox"));
    if(List.IsValid())
    {
        const FVector2D Origin(List->GetScreenOrigin());
        Binder->TryHandleOverlayClick(Origin+FVector2D(20,40));
        TestEqual(TEXT("Second readable character row selects its own character"),Binder->SelectedCharacterId,2);
    }
    if (!Binder->CharNameCacheTex.IsEmpty() && Binder->CharNameCacheTex[0])
    {
        const auto* Texture=Binder->CharNameCacheTex[0].Get();
        auto& Mip=Texture->GetPlatformData()->Mips[0];
        const auto* Pixels=static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
        int32 Soft=0,Dimmed=0;
        for (int32 I=0; I<Mip.SizeX*Mip.SizeY; ++I)
            if(Pixels[I].A>=32 && Pixels[I].A<224) { ++Soft; if(Pixels[I].R<220) ++Dimmed; }
        Mip.BulkData.Unlock();
        TestTrue(TEXT("DAT character glyphs retain antialiased coverage"),Soft>5);
        TestEqual(TEXT("Character glyph RGB is not darkened by alpha twice"),Dimmed,0);
    }
    if (List.IsValid())
    {
        for (int32 I=2; I<20; ++I)
        {
            FACECharacterInfo Character; Character.CharacterId=I+1;
            Character.Name=FString::Printf(TEXT("Character %d"),I+1);
            Binder->Characters.Add(Character);
        }
        Binder->TickRefresh();
        const float RowHeight=Binder->GetCharacterRowHeight();
        TestTrue(TEXT("Twenty-character roster fits the list"),20*RowHeight<=List->Height);
        Binder->TryHandleOverlayClick(FVector2D(List->GetScreenOrigin())+FVector2D(20,19.5f*RowHeight));
        TestEqual(TEXT("Last character remains selectable on a full roster"),Binder->SelectedCharacterId,20);
    }
    Binder->Shutdown(); Canvas->SetCharSelectBinder(nullptr);
    Layout->LoadLayout(0x21000005);
    auto* Client=NewObject<UACEClientSubsystem>(GI);
    Client->Session=MakeShared<FACESession>(); Client->Session->State=EACESessionState::InWorld;
    Client->Session->PlayerGuid=1234;
    FACEWorldObject Player; Player.Guid=1234; Player.Name=TEXT("Retail test"); Player.ItemsCapacity=102;
    Player.ItemType=ACEItemType::Creature;
    Client->Session->WorldObjects.Add(Player.Guid,Player);
    FACEWorldObject Item; Item.Guid=2345; Item.Name=TEXT("Test inventory wand"); Item.ContainerId=1234;
    Item.ItemType=ACEItemType::Caster; Item.IconId=0x060010F9; Item.PlacementPosition=0;
    Client->Session->WorldObjects.Add(Item.Guid,Item);
    Client->Session->ShortcutObjects.SetNumZeroed(18); Client->Session->ShortcutObjects[2]=Item.Guid;
    auto* Gameplay=NewObject<UACEUIGameplayBinder>(); Gameplay->Initialize(Client,Manager,Canvas,nullptr);
    Canvas->SetGameplayBinder(Gameplay);
    Gameplay->TickRefresh();
    const auto Tab=Manager->FindElementByName(TEXT("Spellcast_Tab2"));
    if (TestTrue(TEXT("Casting tab exists"),Tab.IsValid()))
    {
        TestTrue(TEXT("Casting text tab is an activatable hit target"),Tab->IsInteractiveHitTarget());
        // Enter through the normal stance path. Forcing only the spell panel
        // visible leaves the mutually exclusive melee panel over it in retail.
        Gameplay->SyncCombatModeFromServer(static_cast<int32>(ACECombatMode::Magic));
        TestFalse(TEXT("Magic stance hides the overlapping melee panel"),
            bool(Manager->FindElementByName(TEXT("CombatPanel"))->bVisible));
        Manager->ApplyEdgeAnchoredLayout(1600,900);
        const auto Origin=Tab->GetScreenOrigin();
        const FVector2D Pos(Origin.X+Tab->Width/2,Origin.Y+Tab->Height/2);
        const auto Hit=Manager->HitTestCanvas(Pos.X,Pos.Y);
        AddInfo(FString::Printf(TEXT("Casting tab at %s hit=%s scale=%s"),*Pos.ToString(),
            Hit ? *Hit->ElementName : TEXT("none"),*Manager->GetCanvasScale(FVector2D(1600,900)).ToString()));
        TSharedPtr<FACEUIElement> Activated;
        const auto ClickHandle=Manager->OnElementActivated.AddLambda([&](TSharedPtr<FACEUIElement> E){Activated=E;});
        Manager->NotifyMouseDown(Pos,FVector2D(1600,900),EKeys::LeftMouseButton);
        Manager->NotifyMouseUp(Pos,FVector2D(1600,900),EKeys::LeftMouseButton);
        TestTrue(TEXT("Mouse press and release activate the correct casting tab"),Activated==Tab);
        Manager->OnElementActivated.Remove(ClickHandle);
    }
    Gameplay->SyncCombatModeFromServer(2);
    Gameplay->TickRefresh(); Manager->ApplyEdgeAnchoredLayout(1600,900);
    auto Click=[&](const TCHAR* Name)
    {
        const auto Element=Manager->FindElementByName(Name);
        if (!TestTrue(Name,Element.IsValid())) return;
        const auto Origin=Element->GetScreenOrigin();
        const FVector2D Pos(Origin.X+Element->Width/2,Origin.Y+Element->Height/2);
        Manager->NotifyMouseDown(Pos,FVector2D(1600,900),EKeys::LeftMouseButton);
        Manager->NotifyMouseUp(Pos,FVector2D(1600,900),EKeys::LeftMouseButton);
        Gameplay->TickRefresh();
    };
    const bool RepeatBefore=Gameplay->bCombatAutoRepeat;
    Click(TEXT("AutoRepeatAttack"));
    TestTrue(TEXT("Combat repeat checkbox click changes its value"),Gameplay->bCombatAutoRepeat!=RepeatBefore);
    const auto Repeat=Manager->FindElementByName(TEXT("AutoRepeatAttack"));
    TestTrue(TEXT("DAT composite checkbox carries boolean button behavior"),Repeat && Repeat->bBooleanButton);
    if (Repeat)
    {
        Repeat->ResolvePaintState(false,false,false);
        TestEqual(TEXT("Checkbox paints its authored on/off state"),Repeat->PaintState,Gameplay->bCombatAutoRepeat ? 6u : 1u);
    }
    Click(TEXT("LowAttack"));
    TestEqual(TEXT("Combat height click selects low attack"),Gameplay->CombatAttackHeight,ACEAttackHeight::Low);
    const auto Slider=Manager->FindElementByName(TEXT("PowerSlider"));
    if (Slider)
    {
        const auto Origin=Slider->GetScreenOrigin();
        const FVector2D Position(Origin.X+Slider->Width*0.75,Origin.Y+Slider->Height/2);
        TestTrue(TEXT("Power track accepts drag"),Gameplay->TryBeginCombatPowerDrag(Position));
        Gameplay->TryFinishCombatPowerDrag(Position); Gameplay->TickRefresh();
        TestTrue(TEXT("Power drag updates requested attack power"),Gameplay->RequestedAttackPower>0.70f && Gameplay->RequestedAttackPower<0.8f);
        const float Requested=Gameplay->RequestedAttackPower;
        Click(TEXT("HighAttack"));
        TestEqual(TEXT("Height button preserves the selected slider power"),Gameplay->RequestedAttackPower,Requested);
        TestTrue(TEXT("Height activation queues one charged attack"),Gameplay->bCombatAttackRequestPending);
    }
    const bool RepeatSaved=Gameplay->bCombatAutoRepeat;
    Gameplay->ApplyCombatMode(static_cast<int32>(ACECombatMode::NonCombat));
    Gameplay->ApplyCombatMode(static_cast<int32>(ACECombatMode::Melee));
    TestEqual(TEXT("Combat mode changes preserve the repeat preference"),Gameplay->bCombatAutoRepeat,RepeatSaved);
    TestFalse(TEXT("Entering melee does not attack just because repeat is enabled"),Gameplay->bCombatAttackRequestPending);
    FIntPoint ScreenSize(1600,900);
    auto CaptureScreen=[&](const FString& Name,float DPIScale=1.f,FVector2D DesktopOffset=FVector2D::ZeroVector) -> TArray<FColor>
    {
        if (!FApp::CanEverRender()) return {};
        FWidgetRenderer Renderer(true,true);
        const TSharedRef<SWidget> Scaled=SNew(SDPIScaler).DPIScale(DPIScale)[Slate];
        auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(ScreenSize),TF_Bilinear,true);
        const auto Window=SNew(SVirtualWindow).Size(FVector2D(ScreenSize));
        Window->SetContent(Scaled);
        FHittestGrid Grid;
        const FGeometry Geometry=FGeometry::MakeRoot(FVector2D(ScreenSize),FSlateLayoutTransform());
        const FPaintArgs PaintArgs(nullptr,Grid,DesktopOffset,FApp::GetCurrentTime(),0.f);
        for (int32 Pass=0; Pass<4; ++Pass)
        {
            ++GFrameCounter; Canvas->NativeTick(Canvas->GetCachedGeometry(),0.f);
            Renderer.DrawWindow(PaintArgs,Target,Window,Geometry,Geometry.GetLayoutBoundingRect(),0.f); FlushRenderingCommands();
        }
        TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(ScreenSize.X,ScreenSize.Y,Pixels,PNG);
        FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/RetailParity")/(Name+TEXT(".png"))));
        return Pixels;
    };
    CaptureScreen(TEXT("GameplayCombat"));
    {
        const auto SavedPose=Client->Session->GetPlayerPosition();
        const auto SavedSelection=Client->Session->SelectedObject;
        FACEPosition Pose;Pose.CellId=0xDA55001D;Pose.Location=FVector(100,100,20);
        Client->Session->SetLocalPosition(Pose);
        FACEWorldObject RadarNPC;RadarNPC.Guid=24680;RadarNPC.ItemType=ACEItemType::Creature;
        RadarNPC.bHasPosition=true;RadarNPC.Position=Pose;RadarNPC.Position.Location.Y+=20;
        Client->Session->WorldObjects.Add(RadarNPC.Guid,RadarNPC);
        // A behind-camera object and an adjacent indoor cell stay on radar;
        // neither requires a spawned/rendered actor.
        auto Behind=RadarNPC;Behind.Guid=24681;Behind.Position.Location.Y-=40;
        auto Adjacent=RadarNPC;Adjacent.Guid=24682;Adjacent.Position.CellId=0xDA550101;
        auto Distant=RadarNPC;Distant.Guid=24683;Distant.Position.Location.Y+=100;
        Client->Session->WorldObjects.Add(Behind.Guid,Behind);
        Client->Session->WorldObjects.Add(Adjacent.Guid,Adjacent);
        Client->Session->WorldObjects.Add(Distant.Guid,Distant);
        Gameplay->RefreshRadarOverlays();CaptureScreen(TEXT("GameplayRetailRadar"));
        TestTrue(TEXT("Radar includes objects behind the player"),Gameplay->RadarBlipGuids.Contains(Behind.Guid));
        TestTrue(TEXT("Radar includes nearby objects in another cell without a rendered actor"),Gameplay->RadarBlipGuids.Contains(Adjacent.Guid));
        TestFalse(TEXT("Radar excludes out-of-range objects"),Gameplay->RadarBlipGuids.Contains(Distant.Guid));
        const int32 Index=Gameplay->RadarBlipGuids.IndexOfByKey(RadarNPC.Guid);
        if(TestTrue(TEXT("Nearby creature appears on the minimap"),Index!=INDEX_NONE))
        {
            auto* Blip=Gameplay->RadarBlips[Index].Get();
            const auto& Geo=Blip->GetCachedGeometry();
            // Just outside the small sprite, but inside retail's six-pixel tolerance.
            const FVector2D Absolute=Geo.LocalToAbsolute(Geo.GetLocalSize()*.5+FVector2D(5,0));
            TestTrue(TEXT("Radar click accepts the retail pick tolerance"),Gameplay->TryHandleOverlayClick(
                Canvas->GetElementLayer()->GetCachedGeometry().AbsoluteToLocal(Absolute),false));
            TestEqual(TEXT("Radar click selects the nearby creature"),Client->GetSelectedObject().Guid,RadarNPC.Guid);
            Gameplay->RefreshRadarOverlays();CaptureScreen(TEXT("GameplayRetailRadarSelected"));
            TestTrue(TEXT("Selected radar marker uses the retail square mask"),
                Blip->Background.GetResourceObject()==Resources->ResolveRadarBlip(4,true));
        }
        {
            TGuardValue<FACEFellowshipInfo> SavedFellow(Client->Session->Fellowship,Client->Session->Fellowship);
            auto& Fellow=Client->Session->Fellowship;Fellow.bValid=true;Fellow.Members.Reset();
            FACEFellowshipMember Member;Member.Guid=RadarNPC.Guid;Fellow.Members.Add(Member);
            RadarNPC.bIsPlayer=true;RadarNPC.ObjectDescriptionFlags=ACEObjectDescFlag::PlayerKiller;
            Client->Session->WorldObjects[RadarNPC.Guid]=RadarNPC;
            for (int32 Leader : {RadarNPC.Guid,0})
            {
                Fellow.LeaderGuid=Leader;Gameplay->RefreshRadarOverlays();
                TestEqual(TEXT("Fellowship leader and member markers override PK red with retail green"),
                    Gameplay->ResolveRadarColor(RadarNPC,&Fellow),ACERadarColor::BrightGreen);
                if (Index!=INDEX_NONE)TestTrue(TEXT("Selected fellowship radar blip is bright green"),
                    Gameplay->RadarBlips[Index]->GetBrushColor()==FLinearColor::Green);
                TestTrue(TEXT("Selection arrows use the same bright green fellowship color"),
                    Gameplay->ColorFromSelectionMarker(Gameplay->ResolveRadarColor(RadarNPC,&Fellow))==FLinearColor::Green);
            }
            Fellow.Members.Reset();
            TestEqual(TEXT("Leaving fellowship restores PK selection color"),Gameplay->ResolveRadarColor(RadarNPC,&Fellow),ACERadarColor::PlayerKiller);
        }
        for (int32 Guid : {RadarNPC.Guid,Behind.Guid,Adjacent.Guid,Distant.Guid})Client->Session->WorldObjects.Remove(Guid);
        Client->Session->SetLocalPosition(SavedPose);Client->Session->SelectedObject=SavedSelection;
        Gameplay->HandleSelectionChanged(SavedSelection);Gameplay->RefreshRadarOverlays();
    }
    const FString LongChat = TEXT("A long incoming tell must wrap all of its words into several visible lines instead of hiding the rest of the message beneath the next row. This final sentence must also remain readable.");
    for (int32 W : {0, 1})
    {
        if (W) Gameplay->SetFloatyVisible(TEXT("RootGameplay_FloatyChat1_Field"), true);
        Gameplay->AppendChatLineToLog(W, LongChat, FLinearColor::White, TEXT("Sender"));
    }
    CaptureScreen(TEXT("GameplayWrappedChat"));
    for (int32 W : {0, 1})
    {
        auto* Log = Gameplay->GetChatLogWidget(W);
        auto* Row = CastChecked<UACERetailTextBlock>(Log->GetChildAt(Log->GetChildrenCount()-1));
        TestTrue(TEXT("Incoming chat reserves height for every wrapped line"),
            Row->GetCachedGeometry().GetLocalSize().Y > BaseFont.MaxCharHeight * 2);
        TestTrue(TEXT("Chat layout and allocated row agree after wrapping"),
            FMath::IsNearlyEqual(Row->GetDesiredSize().Y,Row->GetCachedGeometry().GetLocalSize().Y,1.f));
    }
    Gameplay->SetFloatyVisible(TEXT("RootGameplay_FloatyChat1_Field"), false);
    FACEChatUIFixture::Check(*this,Gameplay,Canvas,Manager,ScreenSize,LongChat,CaptureScreen);
    TFunction<void()> CheckVitals=[&]()
    {
        TGuardValue<FACEPlayerVitals> SavedVitals(Client->Session->PlayerVitals, Client->Session->PlayerVitals);
        TGuardValue<uint32> SavedOptions(Client->Session->CharacterOptions1, Client->Session->CharacterOptions1);
        TGuardValue<bool> SavedNumbers(Gameplay->bShowVitalNumbers, Gameplay->bShowVitalNumbers);
        auto& V = Client->Session->PlayerVitals;
        V.bValid=true; V.Health=50; V.MaxHealth=100; V.Stamina=60; V.MaxStamina=100; V.Mana=70; V.MaxMana=100;
        Manager->CancelPointerCapture();
        const bool SavedLock=Manager->IsUiLocked();Manager->SetUiLocked(false);
        for(bool Side:{false,true})
        {
            if(Side)Client->Session->CharacterOptions1|=0x00200000u;
            else Client->Session->CharacterOptions1&=~0x00200000u;
            Gameplay->RefreshVitalsOverlays();
            const FString RootName=Side?TEXT("RootGameplay_FloatySideVitals_Field"):TEXT("RootGameplay_FloatyVitals_Field");
            auto Root=Manager->FindElementByName(RootName);
            // Retail classic_floatyvitals (2100006C) / classic_floatysidevitals (21000075).
            TestEqual(TEXT("Vital minimum width follows the retail DAT"),Root->MinWidth,Side?360:160);
            TestEqual(TEXT("Vital maximum width follows the retail DAT"),Root->MaxWidth,Side?3000:610);
            TestEqual(TEXT("Vital minimum height follows the retail DAT"),Root->MinHeight,Side?26:58);
            TestEqual(TEXT("Vital maximum height follows the retail DAT"),Root->MaxHeight,Side?26:58);
            const int32 SavedW=Root->UserResizeW,SavedH=Root->UserResizeH;
            const FIntPoint SavedDrag(Root->UserDragX,Root->UserDragY);
            Root->UserDragX+=200-Root->GetScreenOrigin().X;Root->UserDragY+=100-Root->GetScreenOrigin().Y;
            Root->RecomputeLayoutOffset();Manager->BringFloatyToFront(Root);
            const FString Prefix=Side?TEXT("SideVitals"):TEXT("Vitals");
            for(float UIScale:{1.f,1.25f})
            {
                CaptureScreen(TEXT("VitalsResizeViewport"),UIScale);
                // DPI re-anchoring can move the centered frame. Keep this drag
                // inside the viewport so its left edge has room to expand.
                Root->UserDragX+=200-Root->GetScreenOrigin().X;
                Root->UserDragY+=100-Root->GetScreenOrigin().Y;
                Root->RecomputeLayoutOffset();
                CaptureScreen(TEXT("VitalsResizeSetup"),UIScale);
                auto Drag=[&](const TCHAR* Edge,int32 Amount)
                {
                    const auto Grip=Manager->FindElementUnder(RootName,Prefix+Edge);
                    const FVector2D Start=Canvas->LayoutToViewport(FVector2D(Grip->GetScreenOrigin())+FVector2D(2,2));
                    const FVector2D Size=Canvas->GetCachedGeometry().GetLocalSize();
                    TestTrue(TEXT("Vital side grips display the horizontal resize cursor"),Manager->GetWindowCursor(Start,Size)==EMouseCursor::ResizeLeftRight);
                    const FVector2D End=Start+FVector2D(Amount*Canvas->GetLastScale2D().X,25);
                    const FGeometry Geo=Canvas->GetCachedGeometry();
                    const FVector2D From=Geo.LocalToAbsolute(Start),To=Geo.LocalToAbsolute(End);
                    Canvas->NativeOnMouseButtonDown(Geo,FPointerEvent(0,From,From,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
                    const auto Captured=Manager->GetCaptureElement();
                    TestTrue(*FString::Printf(TEXT("%s at %.2fx captures its own resize grip (%s)"),*Prefix,UIScale,Captured?*Captured->ElementName:TEXT("none")),Captured==Grip);
                    Canvas->NativeOnMouseMove(Geo,FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
                    Canvas->NativeOnMouseButtonUp(Geo,FPointerEvent(0,To,To,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
                };
                const int32 StartWidth=Root->Width,Height=Root->Height;
                const int32 StartRight=Root->GetScreenOrigin().X+Root->Width;
                Drag(TEXT("LeftBorder"),-80);
                TestEqual(TEXT("Left vital grip widens the frame"),Root->Width,StartWidth+80);
                TestEqual(TEXT("Left vital grip keeps the right edge anchored"),Root->GetScreenOrigin().X+Root->Width,StartRight);
                Drag(TEXT("RightBorder"),80);
                TestEqual(TEXT("Right vital grip widens the frame"),Root->Width,StartWidth+160);
                TestEqual(TEXT("Vital bars keep their retail height"),Root->Height,Height);
                CaptureScreen(FString::Printf(TEXT("VitalsResized_%s_%d"),*Prefix,int32(UIScale*100)),UIScale);
                const auto Health=Manager->FindElementUnder(RootName,TEXT("HealthMeter"));
                const auto Stamina=Manager->FindElementUnder(RootName,TEXT("StaminaMeter"));
                const auto Mana=Manager->FindElementUnder(RootName,TEXT("ManaMeter"));
                for(const auto& Meter:{Health,Stamina,Mana})
                {
                    TestEqual(TEXT("Resizing preserves the 16px meter height"),Meter->Height,16);
                    TestTrue(TEXT("Vital meters stay inside the resized frame"),Meter->GetDrawX()>=5 && Meter->GetDrawX()+Meter->Width<=Root->Width-5);
                }
                if(Side)
                {
                    TestEqual(TEXT("Horizontal vitals meet without gaps"),Health->GetDrawX()+Health->Width,Stamina->GetDrawX());
                    TestEqual(TEXT("Horizontal stamina and mana meet without gaps"),Stamina->GetDrawX()+Stamina->Width,Mana->GetDrawX());
                }
                else TestEqual(TEXT("Stacked meters fill the resized width"),Health->Width,Root->Width-10);
                Drag(TEXT("RightBorder"),-160);
                const int32 BeforeBoundsW=Root->UserResizeW;
                const auto Grip=Manager->FindElementUnder(RootName,Prefix+TEXT("RightBorder"));
                const FGeometry Geo=Canvas->GetCachedGeometry();
                const FVector2D From=Geo.LocalToAbsolute(Canvas->LayoutToViewport(FVector2D(Grip->GetScreenOrigin())+FVector2D(2,2)));
                Canvas->NativeOnMouseButtonDown(Geo,FPointerEvent(0,From,From,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
                for(int32 Delta:{4000,-4000})
                {
                    const FVector2D To=From+FVector2D(Delta*Canvas->GetLastScale2D().X,0);
                    Canvas->NativeOnMouseMove(Geo,FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
                    TestEqual(TEXT("Captured resize reaches retail bounds even outside the viewport"),Root->Width,Delta>0?Root->MaxWidth:Root->MinWidth);
                    TestEqual(TEXT("Bounded resize cannot change the fixed vital height"),Root->Height,Height);
                    TestTrue(TEXT("Resize capture survives leaving the viewport"),Manager->GetCaptureElement()==Grip);
                }
                const FVector2D Outside=From-FVector2D(4000*Canvas->GetLastScale2D().X,0);
                Canvas->NativeOnMouseButtonUp(Geo,FPointerEvent(0,Outside,Outside,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
                TestFalse(TEXT("Releasing outside clears the vital resize capture"),Manager->GetCaptureElement().IsValid());
                Root->UserResizeW=BeforeBoundsW;UACEUIElementManager::ApplyFloatyResizeLayout(Root);
            }
            Root->UserResizeW=SavedW;Root->UserResizeH=SavedH;
            Root->UserDragX=SavedDrag.X;Root->UserDragY=SavedDrag.Y;
            Root->RecomputeLayoutOffset();UACEUIElementManager::ApplyFloatyResizeLayout(Root);
        }
        Manager->SetUiLocked(SavedLock);CaptureScreen(TEXT("VitalsResizeRestored"));
        for (int32 Percent : {0,50,100}) for (bool Side : {false,true}) for (bool Numbers : {false,true})
        {
            V.Health=V.Stamina=V.Mana=Percent;
            if (Side) Client->Session->CharacterOptions1 |= 0x00200000u;
            else Client->Session->CharacterOptions1 &= ~0x00200000u;
            Gameplay->bShowVitalNumbers=Numbers; Gameplay->RefreshVitalsOverlays();
            CaptureScreen(FString::Printf(TEXT("Vitals_%s_%s_%d"),Side?TEXT("Side"):TEXT("Stacked"),Numbers?TEXT("Numbers"):TEXT("Textures"),Percent));
            const TCHAR* Root=Side?TEXT("RootGameplay_FloatySideVitals_Field"):TEXT("RootGameplay_FloatyVitals_Field");
            for (const TCHAR* Name : {TEXT("HealthMeter"),TEXT("StaminaMeter"),TEXT("ManaMeter")})
            {
                const auto Meter=Manager->FindElementUnder(Root,Name);
                TestEqual(TEXT("Vital meter selects the authored detail state"),Meter->PaintState,Numbers?0x10000006u:0x10000007u);
                int32 Details=0;
                TArray<TSharedPtr<FACEUIElement>> Nodes{Meter};
                while (!Nodes.IsEmpty())
                {
                    const auto Node=Nodes.Pop(EAllowShrinking::No); Nodes.Append(Node->Children);
                    if(const auto* Paint=Canvas->PaintStates.Find(Node->InstanceId))
                    {
                        const FIntPoint Origin=Meter->GetScreenOrigin();const auto Scale=Canvas->GetLastScale2D();
                        TestTrue(TEXT("Vital artwork remains inside its meter vertically without stretching"),
                            Paint->Y>=FMath::RoundToInt(Origin.Y*Scale.Y)
                            && Paint->Y+Paint->H<=FMath::RoundToInt((Origin.Y+Meter->Height)*Scale.Y));
                        if(const auto Border=Canvas->ImageWidgets.FindRef(Node->InstanceId);Border && Paint->Texture.IsValid())
                        {
                            const auto Texture=Paint->Texture.Get();
                            const FVector2D NativeSize(Texture->GetSizeX()*Scale.X,Texture->GetSizeY()*Scale.Y);
                            TestTrue(TEXT("Vital artwork keeps native DAT pixels for opaque and alpha images"),
                                FVector2D(Border->Background.ImageSize).Equals(NativeSize,.01));
                        }
                        if(Percent==0)
                        {
                            auto Layer=Node;while(Layer && Layer->Parent.Pin()!=Meter)Layer=Layer->Parent.Pin();
                            TestFalse(TEXT("A zero vital paints no residual fill pixel"),Layer&&Layer->ElementName!=TEXT("meter_background")&&Layer->Type!=ACEUI::ElementType::Text);
                        }
                    }
                    if (Node->ElementName!=TEXT("detail")) continue;
                    ++Details;
                    const auto* State=Node->States.Find(Node->PaintState);
                    TestTrue(TEXT("Both background and fill detail select textured media only without numbers"),
                        State && (Numbers ? State->ImageFileId==0 : State->ImageFileId!=0 && Resources->ResolveTexture(State->ImageFileId,State->AlphaFileId)));
                }
                TestEqual(TEXT("Each vital retains both background and fill texture layers"),Details,2);
            }
            TestEqual(TEXT("Vital numeric overlay follows display preference"),
                Gameplay->HealthLabel->GetVisibility()==ESlateVisibility::Collapsed || Gameplay->HealthLabel->GetText().IsEmpty(),!Numbers);
        }
        // Save a readable, directly rendered close-up at 3x UI scale. No source
        // texture resize is involved: this also checks the actual scaled result.
        Client->Session->CharacterOptions1&=~0x00200000u;Gameplay->bShowVitalNumbers=false;
        for(float Scale:{1.f,3.f})
        {
            const auto Pixels=CaptureScreen(FString::Printf(TEXT("Vitals_Scale%d"),int32(Scale)),Scale);
            const auto Root=Manager->FindElementByName(TEXT("RootGameplay_FloatyVitals_Field"));
            const FIntPoint O=Root->GetScreenOrigin();const int32 W=Root->Width*Scale,H=Root->Height*Scale;
            if(Pixels.Num()==ScreenSize.X*ScreenSize.Y)
            {
                TArray<FColor> Crop;Crop.Reserve(W*H);
                for(int32 Y=0;Y<H;++Y)for(int32 X=0;X<W;++X)Crop.Add(Pixels[(int32(O.Y*Scale)+Y)*ScreenSize.X+int32(O.X*Scale)+X]);
                TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(W,H,Crop,PNG);
                FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("Automation/RetailParity/Vitals_Closeup%d.png"),int32(Scale))));
            }
        }
    };
    CheckVitals();
    Gameplay->RefreshVitalsOverlays();
    CaptureScreen(TEXT("Vitals_ScaleRestored"));
    Gameplay->SetFloatyVisible(TEXT("OptionsPanel_Field"),false);
    FSlateApplication::Get().ClearKeyboardFocus(EFocusCause::Cleared);
    const FKeyEvent Escape(EKeys::Escape,FModifierKeysState(),0,false,0,0);
    Canvas->NativeOnPreviewKeyDown(Canvas->GetCachedGeometry(),Escape);
    TestTrue(TEXT("Escape opens the options menu"),Manager->FindElementByName(TEXT("OptionsPanel_Field"))->bVisible);
    Canvas->NativeOnPreviewKeyDown(Canvas->GetCachedGeometry(),Escape);
    TestFalse(TEXT("A second Escape closes the options menu"),Manager->FindElementByName(TEXT("OptionsPanel_Field"))->bVisible);
    auto NativeClick=[&](const TCHAR* Name)
    {
        const auto Element=Manager->FindElementByName(Name);
        if (!TestTrue(Name,Element.IsValid())) return;
        const FVector2D Local=Canvas->LayoutToViewport(FVector2D(Element->GetScreenOrigin())+FVector2D(Element->Width,Element->Height)*.5);
        const auto& Geometry=Canvas->GetCachedGeometry();
        const FVector2D Absolute=Geometry.LocalToAbsolute(Local);
        const auto Hit=Manager->HitTestCanvas(Element->GetScreenOrigin().X+Element->Width/2,Element->GetScreenOrigin().Y+Element->Height/2);
        AddInfo(FString::Printf(TEXT("Native click %s hit=%s activatable=%d type=%08X"),Name,Hit?*Hit->ElementName:TEXT("none"),Element->bActivatable,Element->Type));
        FPointerEvent Down(0,Absolute,Absolute,TSet<FKey>{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState());
        FPointerEvent Up(0,Absolute,Absolute,TSet<FKey>{},EKeys::LeftMouseButton,0,FModifierKeysState());
        Canvas->NativeOnMouseButtonDown(Geometry,Down);
        Canvas->NativeOnMouseButtonUp(Geometry,Up);
        Gameplay->TickRefresh();
    };
    NativeClick(TEXT("ChatTarget"));
    TestTrue(TEXT("Actual canvas Say press/release opens the chat destinations"),Gameplay->bChatTargetPopupOpen);
    Gameplay->CloseChatTargetPopup();
    {
    TGuardValue<FACEPlayerVitals> SavedScaleVitals(Client->Session->PlayerVitals,Client->Session->PlayerVitals);
    auto& V=Client->Session->PlayerVitals;
    V.bValid=true;V.Health=50;V.MaxHealth=100;V.Stamina=60;V.MaxStamina=100;V.Mana=70;V.MaxMana=100;
    for(float Scale:{1.25f,2.f,3.f})
    {
        const FIntPoint OriginalSize=ScreenSize;ScreenSize=FIntPoint(3840,2160);
        CaptureScreen(FString::Printf(TEXT("DesktopUI_%dpercent"),FMath::RoundToInt(Scale*100)),Scale);
        TestTrue(TEXT("Scaled viewport keeps the entire layout in bounds"),Canvas->GetCachedGeometry().GetLocalSize().Equals(FVector2D(ScreenSize)/Scale,1.f));
        NativeClick(TEXT("ChatTarget"));
        TestTrue(TEXT("Physical mouse coordinates still activate scaled UI controls"),Gameplay->bChatTargetPopupOpen);
        Gameplay->CloseChatTargetPopup();ScreenSize=OriginalSize;
    }
    }
    CaptureScreen(TEXT("DesktopUI_Restored"));
    Gameplay->ShowVendorPanel(9876);
    CaptureScreen(TEXT("GameplayVendorBefore"));
    NativeClick(TEXT("VendorBuyTab"));
    TestEqual(TEXT("Actual canvas vendor tab switches to buy cart"),Gameplay->ActiveVendorPage,1);
    NativeClick(TEXT("VendorItemsTab"));
    NativeClick(TEXT("Menu_Vendor_SelectionWidget"));
    TestTrue(TEXT("Actual canvas vendor filter opens"),Gameplay->bVendorFilterDropdownOpen);
    CaptureScreen(TEXT("GameplayVendorGreenDropdown"));
    CheckVendorRowArtwork(*this,Gameplay->VendorFilterDropdownRows,Gameplay->VendorVisibleFilterIndices,
        Gameplay->VendorItemTypeFilter,Canvas->GetResourceResolver());
    [&]()
    {
        TGuardValue<TArray<FACEWorldObject>> SavedStock(Client->Session->VendorMerchandise,{});
        // Enough real item categories and stock to cover an enabled scrollbar
        // with the dropdown, using the authored layout rather than moving controls.
        for (const auto& Category : ACERetailVendorCategories::Categories)
            for (int32 I=0;I<32;++I)
            {
                FACEWorldObject Stock;
                Stock.Guid=980000+Client->Session->VendorMerchandise.Num();
                Stock.Name=Category.Name;Stock.ContainerId=9876;
                Stock.ItemType=Category.Mask & (~Category.Mask+1u);
                Client->Session->VendorMerchandise.Add(Stock);
            }
        Gameplay->SetVendorItemTypeFilter(0);
        Gameplay->ToggleVendorFilterDropdown();
        CaptureScreen(TEXT("GameplayVendorDropdownHitPriority"));
        const auto Bar=Manager->FindElementByName(TEXT("Vendor_ItemListScroll"));
        if (!TestTrue(TEXT("Vendor regression has an enabled item scrollbar"),Bar && Bar->bVisible)) return;
        const auto& Geo=Canvas->GetCachedGeometry();
        const FVector2D BarOrigin(Bar->GetScreenOrigin());
        const auto& DropGeo=Gameplay->VendorFilterDropdownBg->GetCachedGeometry();
        const FVector2D DropMin=Canvas->ViewportToLayout(Geo.AbsoluteToLocal(DropGeo.GetAbsolutePosition()));
        const FVector2D DropMax=Canvas->ViewportToLayout(Geo.AbsoluteToLocal(DropGeo.LocalToAbsolute(DropGeo.GetLocalSize())));
        const FVector2D Min(FMath::Max(BarOrigin.X+17.,DropMin.X),FMath::Max(BarOrigin.Y,DropMin.Y));
        const FVector2D Max(FMath::Min(BarOrigin.X+Bar->Width-17.,DropMax.X),FMath::Min(BarOrigin.Y+Bar->Height,DropMax.Y));
        if (!TestTrue(TEXT("Vendor dropdown overlaps the scrollbar track"),Max.X>Min.X && Max.Y>Min.Y)) return;
        const FVector2D Point=Geo.LocalToAbsolute(Canvas->LayoutToViewport((Min+Max)*.5));
        const int32 Row=FMath::FloorToInt(DropGeo.AbsoluteToLocal(Point).Y/DropGeo.GetLocalSize().Y*Gameplay->VendorVisibleFilterIndices.Num());
        const int32 ExpectedFilter=Gameplay->VendorVisibleFilterIndices[Row];
        const int32 Before=Gameplay->VendorItemsScrollOffset;
        auto Press=[&](FVector2D P)
        {
            TestTrue(TEXT("Vendor popup press is handled"),Canvas->NativeOnMouseButtonDown(Geo,
                FPointerEvent(0,P,P,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState())).IsEventHandled());
            TestFalse(TEXT("Vendor popup never begins a scrollbar drag"),Gameplay->IsScrollbarDragActive());
            Canvas->NativeOnMouseButtonUp(Geo,FPointerEvent(0,P,P,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
        };
        Press(Point);
        TestEqual(TEXT("Covered scrollbar click selects the visible vendor category"),Gameplay->VendorItemTypeFilter,ExpectedFilter);
        TestFalse(TEXT("Selecting a category closes the vendor dropdown"),Gameplay->bVendorFilterDropdownOpen);
        TestEqual(TEXT("Category click leaves the scrollbar offset unchanged"),Gameplay->VendorItemsScrollOffset,Before);
        Gameplay->ToggleVendorFilterDropdown();
        CaptureScreen(TEXT("GameplayVendorDropdownDismiss"));
        // The far right arrow is outside the dropdown but still on the vendor.
        const FVector2D Arrow=Geo.LocalToAbsolute(Canvas->LayoutToViewport(BarOrigin+FVector2D(Bar->Width-8,Bar->Height*.5)));
        TestFalse(TEXT("Dismiss click is outside the category popup"),Gameplay->VendorFilterDropdownBg->GetCachedGeometry().IsUnderLocation(Arrow));
        Press(Arrow);
        TestFalse(TEXT("Outside click dismisses the category popup"),Gameplay->bVendorFilterDropdownOpen);
        TestEqual(TEXT("Dismiss click does not activate the scrollbar arrow behind it"),Gameplay->VendorItemsScrollOffset,Before);
        Press(Arrow);
        TestEqual(TEXT("Scrollbar arrow works again after popup dismissal"),Gameplay->VendorItemsScrollOffset,Before+1);
        Gameplay->CloseVendorFilterDropdown();
    }();
    Gameplay->HideVendorPanel();
    Gameplay->ShowExternalContainer(123);
    Gameplay->TickRefresh();
    const auto Loot=Manager->FindElementByName(TEXT("ExternalContainer"));
    TestEqual(TEXT("Loot content keeps resolved retail width"),Loot->Width,600);
    const auto Area=Manager->FindElementUnder(TEXT("ExternalContainer"),TEXT("ItemsArea"));
    const auto Edge=Manager->FindElementUnder(TEXT("ExternalContainer"),TEXT("RightEdge"));
    TestTrue(TEXT("Loot bevel follows the item area width"),Area && Edge && Edge->X+Edge->Width==Area->Width);
    CaptureScreen(TEXT("GameplayLoot"));
    Gameplay->HideExternalContainer(false);
    Gameplay->LastAppraisal.Name=TEXT("Inspection layout regression");
    Gameplay->LastAppraisal.bSuccess=true;
    Gameplay->LastAppraisal.ObjectGuid=456;
    Gameplay->LastAppraisal.Value=150; Gameplay->LastAppraisal.bHasValue=true;
    Gameplay->LastAppraisal.Burden=200; Gameplay->LastAppraisal.bHasBurden=true;
    Gameplay->LastAppraisal.Summary=TEXT("Spells (1)\n  Spell 1\nDamage 20  Speed 30  Offense 1.00\n");
    Gameplay->LastAppraisal.bHasWeaponProfile=true; Gameplay->LastAppraisal.ItemType=ACEItemType::MeleeWeapon;
    Gameplay->LastAppraisal.Damage=20; Gameplay->LastAppraisal.DamageVariance=.2f;
    Gameplay->LastAppraisal.DamageType=1; Gameplay->LastAppraisal.WeaponTime=30;
    Gameplay->LastAppraisal.SpellIds={1};
    for (int32 I=0; I<40; ++I) Gameplay->LastAppraisal.Summary+=TEXT("Authored bitmap text wraps and scrolls within the body.\n");
    Gameplay->LastAppraisal.Inscription=TEXT("Inscription stays in the paper strip.");
    Gameplay->LastAppraisal.StringProperties.Add(8,TEXT("Test Scribe"));
    Gameplay->ShowExamination(true); Gameplay->RefreshExaminationOverlay();
    const auto ItemText=Manager->FindElementUnder(TEXT("ItemExamineUI"),TEXT("ItemDisplayText"));
    TestEqual(TEXT("Inspection body keeps DAT y=0, without guessed icon band"),ItemText->Y,0);
    TestEqual(TEXT("Inspection body keeps DAT x=4"),ItemText->X,4);
    TestFalse(TEXT("Inspection does not graft obsolete item icon"),Manager->FindElementUnder(TEXT("ItemExamineUI"),TEXT("ItemIcon")).IsValid());
    CaptureScreen(TEXT("GameplayInspection"));
    const auto Signature=Manager->FindElementUnder(TEXT("ItemExamineUI"),TEXT("ItemInscriptionSignatureText"));
    TestEqual(TEXT("Inscription displays its author"),Gameplay->ExamInscriptionSignature->GetText().ToString(),FString(TEXT("--Test Scribe")));
    TestEqual(TEXT("Inscription author uses retail's right alignment"),CastChecked<UACERetailTextBlock>(Gameplay->ExamInscriptionSignature)->GetTextJustification(),ETextJustify::Right);
    TestTrue(TEXT("Inscription signature remains inside the resized examination panel"),Signature&&Signature->Y>=0&&Signature->Y+Signature->Height<=Manager->FindElementByName(TEXT("ItemExamineUI"))->Height);
    TestTrue(TEXT("Long appraisal is scrollable instead of overflowing"),Gameplay->ExamScroll && Gameplay->ExamScroll->GetScrollOffsetOfEnd()>0.f);
    const auto InscriptionBar=Manager->FindElementUnder(TEXT("ItemExamineUI"),TEXT("ItemInscriptionScrollbar"));
    TestTrue(TEXT("Short inscriptions hide their unused arrow controls"),InscriptionBar && !InscriptionBar->bVisible);
    TestTrue(TEXT("Retail layout without value controls includes value and burden in body"),
        Gameplay->ExamBody->GetText().ToString().StartsWith(TEXT("Value: 150\nBurden: 200\n")));
    TestTrue(TEXT("Resolving spell names preserves formatted weapon info"),
        Gameplay->ExamBody->GetText().ToString().Contains(TEXT("Damage: 16 - 20, Slashing")));
    TestFalse(TEXT("Inspection removes the obsolete diagnostic weapon summary"),Gameplay->ExamBody->GetText().ToString().Contains(TEXT("Offense 1.00")));
    auto* ExistingBodySlot = Gameplay->ExamBody->Slot.Get();
    Gameplay->ExamScroll->SetScrollOffset(80.f);
    Gameplay->RefreshExaminationOverlay();
    TestTrue(TEXT("Refreshing appraisal retains the existing scroll child and slot"), Gameplay->ExamBody->Slot == ExistingBodySlot);
    TestEqual(TEXT("Refreshing same appraisal preserves scroll position"), Gameplay->ExamScroll->GetScrollOffset(), 80.f);
    Gameplay->ExamScroll->SetScrollOffset(Gameplay->ExamScroll->GetScrollOffsetOfEnd());
    const auto ScrolledPixels = CaptureScreen(TEXT("GameplayInspectionScrolled"));
    if (ScrolledPixels.Num() == 1600*900)
    {
        const auto Origin = ItemText->GetScreenOrigin();
        int32 WhitePixels = 0;
        for (int32 Y=Origin.Y+ItemText->Height-100; Y<Origin.Y+ItemText->Height-20; ++Y)
            for (int32 X=Origin.X+8; X<Origin.X+ItemText->Width-24; ++X)
                if (X>=0 && X<1600 && Y>=0 && Y<900)
                {
                    const FColor P=ScrolledPixels[Y*1600+X];
                    WhitePixels += P.R>200 && P.G>200 && P.B>200;
                }
        TestTrue(TEXT("Bottom of a scrolled appraisal still paints visible glyphs"), WhitePixels>100);
    }
    const FString ShortInscription=Gameplay->LastAppraisal.Inscription;
    for(int I=0;I<20;++I)Gameplay->LastAppraisal.Inscription+=TEXT("\nA long inscription remains readable on its paper strip.");
    CaptureScreen(TEXT("GameplayLongInscription"));
    TestTrue(TEXT("Long inscription activates the retail scrollbar"),InscriptionBar->bVisible && Gameplay->ExamInscriptionScroll->GetScrollOffsetOfEnd()>0);
    Gameplay->OnElementActivated(Manager->FindElementUnder(TEXT("ItemInscriptionScrollbar"),TEXT("ScrollBar_Down")));
    TestTrue(TEXT("Inscription down arrow scrolls its own text"),Gameplay->ExamInscriptionScroll->GetScrollOffset()>0);
    Gameplay->LastAppraisal.Inscription=ShortInscription;
    Gameplay->LastAppraisal.ObjectGuid=457; Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("New appraisal starts at top as retail is_new requires"), Gameplay->ExamScroll->GetScrollOffset(), 0.f);
    const auto ItemAppraisal = Gameplay->LastAppraisal;
    Gameplay->LastAppraisal = FACEAppraisalInfo();
    Gameplay->LastAppraisal.ObjectGuid=459; Gameplay->LastAppraisal.Name=TEXT("Bandana of Mana Conversion");
    Gameplay->LastAppraisal.bSuccess=true; Gameplay->LastAppraisal.bHasValue=true; Gameplay->LastAppraisal.Value=3880;
    Gameplay->LastAppraisal.bHasBurden=true; Gameplay->LastAppraisal.Burden=23;
    Gameplay->LastAppraisal.IntProperties={{28,120},{105,7},{171,2},{106,100},{107,300},{108,400},{109,85},{158,7},{160,50}};
    Gameplay->LastAppraisal.FloatProperties={{144,.31},{29,1.15}};
    Gameplay->LastAppraisal.ArmorResistances={1.6f,1.2f,1.f,.4f,.8f,.5f,0.f,2.f};
    Gameplay->LastAppraisal.ArmorEnchantments=0x00030007; // armor/slash raised, pierce lowered
    Gameplay->LastAppraisal.Summary=TEXT("Spells (1)\n  Spell 1\n"); Gameplay->LastAppraisal.SpellIds={1};
    Gameplay->RefreshExaminationOverlay();
    const FString ItemDetails=Gameplay->ExamBody->GetText().ToString();
    TestTrue(TEXT("Item appraisal includes effective armor and mana"),ItemDetails.Contains(TEXT("Slashing: Excellent (192)")) && ItemDetails.Contains(TEXT("Mana: 300 / 400.")));
    if(auto* Colored=Cast<UACERetailTextBlock>(Gameplay->ExamBody))
    {
        TestEqual(TEXT("Buffed armor uses green retail stat text"),Colored->GetGlyphColor(ItemDetails.Find(TEXT("Slashing:")),FLinearColor::White),FLinearColor::Green);
        TestEqual(TEXT("Debuffed armor uses red retail stat text"),Colored->GetGlyphColor(ItemDetails.Find(TEXT("Piercing:")),FLinearColor::White),FLinearColor(1.f,.2f,.15f));
        TestEqual(TEXT("Unenchanted armor stays neutral"),Colored->GetGlyphColor(ItemDetails.Find(TEXT("Bludgeoning:")),FLinearColor::White),FLinearColor::White);
    }
    TestTrue(TEXT("Mana conversion is a zero-based bonus; defense is a multiplier"),ItemDetails.Contains(TEXT("Mana Conversion: +31%.")) && ItemDetails.Contains(TEXT("Melee Defense: +15.0%.")));
    TestTrue(TEXT("Item appraisal displays wield requirements and workmanship"),ItemDetails.Contains(TEXT("Wield requires Level 50")) && ItemDetails.Contains(TEXT("Workmanship: Flawless (7)")));
    CaptureScreen(TEXT("GameplayDetailedItemInspection"));
    Gameplay->LastAppraisal=FACEAppraisalInfo();
    auto& Melee=Gameplay->LastAppraisal;
    Melee.ObjectGuid=462; Melee.Name=TEXT("Cleave Sword"); Melee.bSuccess=true; Melee.bHasWeaponProfile=true;
    Melee.ItemType=ACEItemType::MeleeWeapon; Melee.Damage=40; Melee.DamageVariance=.3f; Melee.DamageType=3;
    Melee.WeaponSkill=44; Melee.WeaponTime=30; Melee.WeaponOffense=1.15f;
    Melee.WeaponEnchantments=0x00040004;
    Melee.IntProperties={{353,2},{292,3},{158,2},{159,44},{160,350}}; Melee.FloatProperties={{29,1.2}}; Melee.SpellIds={1};
    Gameplay->RefreshExaminationOverlay();
    const FString MeleeText=Gameplay->ExamBody->GetText().ToString();
    if(auto* Colored=Cast<UACERetailTextBlock>(Gameplay->ExamBody))
        TestEqual(TEXT("Faster enchanted weapons use the server's beneficial color bit"),Colored->GetGlyphColor(MeleeText.Find(TEXT("Speed:")),FLinearColor::White),FLinearColor::Green);
    TestTrue(TEXT("Melee inspection shows skill, type, range and speed"),MeleeText.Contains(TEXT("Heavy Weapons (Sword)")) && MeleeText.Contains(TEXT("Damage: 28 - 40, Slashing/Piercing")) && MeleeText.Contains(TEXT("Speed: Fast (30)")));
    TestTrue(TEXT("Melee inspection shows attack, defense and exact cleave count"),MeleeText.Contains(TEXT("Attack Skill: +15%")) && MeleeText.Contains(TEXT("Melee Defense: +20.0%")) && MeleeText.Contains(TEXT("Cleave: 3 enemies")));
    TestTrue(TEXT("Damage is visible before long spell descriptions"),MeleeText.Find(TEXT("Damage:")) < MeleeText.Find(TEXT("Spells:")));
    CaptureScreen(TEXT("GameplayMeleeInspection"));
    Melee.bSuccess=false; Melee.bHasWeaponProfile=false; Melee.SpellIds.Reset(); Gameplay->RefreshExaminationOverlay();
    TestTrue(TEXT("Failed weapon identification displays unknown damage"),Gameplay->ExamBody->GetText().ToString().Contains(TEXT("Damage: Unknown")));
    Gameplay->LastAppraisal=FACEAppraisalInfo();
    auto& CasterInfo=Gameplay->LastAppraisal;
    CasterInfo.ObjectGuid=460; CasterInfo.Name=TEXT("Ivory Slashing Baton"); CasterInfo.bSuccess=true;
    CasterInfo.bHasValue=CasterInfo.bHasBurden=true; CasterInfo.Value=20613; CasterInfo.Burden=50;
    CasterInfo.IntProperties={{105,7},{171,10},{106,370},{107,4084},{108,4084},{109,389},{158,2},{159,34},{160,375},{179,8},{45,1}};
    CasterInfo.FloatProperties={{29,1.15},{144,.07},{152,1.25},{5,-1.0/15.0}};
    CasterInfo.StringProperties={{39,TEXT("Tinker Holdem")},{40,TEXT("Tinker Holdem")}};
    CasterInfo.SpellIds={1,2,3,4,5,6}; CasterInfo.BoolProperties.Add(22,true);
    Gameplay->RefreshExaminationOverlay();
    const FString CasterDetails=Gameplay->ExamBody->GetText().ToString();
    TestTrue(TEXT("Caster describes imbue and elemental bonuses"),CasterDetails.Contains(TEXT("Slash Rending")) && CasterDetails.Contains(TEXT("vs. Monsters: +25.0%")) && CasterDetails.Contains(TEXT("vs. Players: +12.5%")));
    TestTrue(TEXT("Caster mana cost comes from regeneration rate"),CasterDetails.Contains(TEXT("Mana Cost: 1 point per 15 seconds.")));
    FString Description; TestTrue(TEXT("Spell DAT supplies description"),Dat->TryGetSpellDescription(1,Description));
    TestTrue(TEXT("Inspect includes the actual DAT spell description"),!Description.IsEmpty() && CasterDetails.Contains(Description));
    CaptureScreen(TEXT("GameplayCasterInspection"));
    TestTrue(TEXT("Real caster details overflow into scrollable content"),Gameplay->ExamScroll->GetScrollOffsetOfEnd()>0.f);

    TestFalse(TEXT("Spell-bearing equipment does not display stone stored-mana fields"),CasterDetails.Contains(TEXT("Stored Mana:")));
    {
        // Slayer is an appraisal property, including on casters that have no
        // weapon profile. Cover the complete retail enum instead of a name list.
        FACEAppraisalInfo Slayer;Slayer.bSuccess=true;Slayer.ItemType=ACEItemType::Caster;
        Slayer.IntProperties={{179,8}};
        const auto CreatureNames=Resources->ResolveEnumStrings(0x10000005);
        TestTrue(TEXT("Retail creature enum has the complete slayer family"),CreatureNames.Num()>100);
        for(const auto& Entry:CreatureNames)
        {
            Slayer.IntProperties.Add(166,int32(Entry.Key));
            FString Expected=Entry.Value;Expected.ReplaceInline(TEXT("_"),TEXT(" "));
            Expected=Entry.Key==31?TEXT("Bael'Zharon's Hate"):Expected+TEXT(" slayer");
            for(bool Chrome:{false,true})
                TestTrue(*FString::Printf(TEXT("Slayer type %u renders on desktop/VR without a weapon profile"),Entry.Key),
                    ACEAppraisalFormatting::ItemExaminationText(Slayer,Dat,Chrome,Chrome).Contains(TEXT("Properties: ")+Expected+TEXT(", Slash Rending")));
        }
        for(const auto& Entry:{TPair<int32,const TCHAR*>(3,TEXT("Drudge slayer")),{38,TEXT("Fire Elemental slayer")},{31,TEXT("Bael'Zharon's Hate")}})
            TestEqual(TEXT("Retail slayer spelling and human-slayer title"),ACEAppraisalFormatting::SlayerProperty(Dat,Entry.Key),FString(Entry.Value));
        Slayer.IntProperties[166]=3;Slayer.bSuccess=false;
        TestTrue(TEXT("Partial appraisal retains a server-supplied slayer"),ACEAppraisalFormatting::ItemDetails(Slayer,Dat).Contains(TEXT("Properties: Drudge slayer, Slash Rending")));
        Slayer.IntProperties[166]=MAX_int32;
        TestTrue(TEXT("Unknown creature enum never invents a slayer name"),ACEAppraisalFormatting::ItemDetails(Slayer,Dat).Contains(TEXT("Properties: Slash Rending")));
        Slayer.IntProperties.Remove(166);Slayer.FloatProperties.Add(138,2.);
        TestTrue(TEXT("A multiplier without a creature type does not invent a slayer"),ACEAppraisalFormatting::ItemDetails(Slayer,Dat).Contains(TEXT("Properties: Slash Rending")));

        CasterInfo.Name=TEXT("Emerald Slashing Sceptre");CasterInfo.ItemType=ACEItemType::Caster;
        CasterInfo.Value=37474;CasterInfo.IntProperties.Add(105,6);CasterInfo.IntProperties.Add(166,3);CasterInfo.IntProperties.Add(370,3);
        CasterInfo.FloatProperties.Add(152,12.95);CasterInfo.FloatProperties.Add(136,.1);CasterInfo.FloatProperties.Add(147,.1);
        CasterInfo.ResistanceEnchantments=0x20002000;
        Gameplay->RefreshExaminationOverlay();
        const FString SceptreText=Gameplay->ExamBody->GetText().ToString();
        TestTrue(TEXT("Real item panel shows Drudge slayer ahead of existing imbues"),SceptreText.Contains(TEXT("Properties: Drudge slayer, Slash Rending, Crushing Blow, Biting Strike")));
        TestTrue(TEXT("Real item panel retains a blank before Ratings, without subtracting a point"),SceptreText.Contains(TEXT("Workmanship: Nearly flawless (6)\n\nRatings: Dam 3.")));
        if(auto* Colored=Cast<UACERetailTextBlock>(Gameplay->ExamBody))
            for(const TCHAR* Prefix:{TEXT("Damage bonus for "),TEXT(" vs. Monsters:"),TEXT(" vs. Players:")})
                TestEqual(TEXT("Real item panel colors caster bonus heading and both damage values"),
                    Colored->GetGlyphColor(SceptreText.Find(Prefix),FLinearColor::White),FLinearColor::Green);
        // Show all reported fields in one capture instead of leaving damage
        // bonuses below the default-height inspection window's scroll area.
        const auto Window=Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"));
        const FIntPoint Drag(Window->UserDragX,Window->UserDragY),Origin=Window->GetScreenOrigin();
        const int32 Resize=Window->UserResizeH;
        Window->UserDragX+=20-Origin.X;Window->UserDragY+=20-Origin.Y;
        Window->UserResizeH=840-Window->AuthoredHeight;
        Gameplay->RefreshExaminationOverlay();
        CaptureScreen(TEXT("GameplaySlayerSceptreInspection"));
        Window->UserDragX=Drag.X;Window->UserDragY=Drag.Y;Window->UserResizeH=Resize;
        Gameplay->RefreshExaminationOverlay();
    }
    Gameplay->LastAppraisal=FACEAppraisalInfo();
    auto& Charge=Gameplay->LastAppraisal;
    Charge.ObjectGuid=9060; Charge.Name=TEXT("Titan Mana Charge"); Charge.bSuccess=true;
    Charge.Summary=TEXT("Use on a magic item to give the stone's stored Mana to that item.");
    Charge.IntProperties.Add(107,5000); Charge.FloatProperties={{87,1.0},{137,1.0}};
    Gameplay->RefreshExaminationOverlay();
    FString ChargeText=Gameplay->ExamBody->GetText().ToString();
    TestTrue(TEXT("Titan Mana Charge displays all retail mana fields after its usage"),
        ChargeText.Contains(TEXT("Stored Mana: 5000\nEfficiency: 100%\nChance of Destruction: 100%")));
    CaptureScreen(TEXT("GameplayTitanManaChargeInspection"));
    Charge.IntProperties[107]=0;Charge.FloatProperties[87]=.75;Charge.FloatProperties[137]=.25;
    Gameplay->RefreshExaminationOverlay();ChargeText=Gameplay->ExamBody->GetText().ToString();
    TestTrue(TEXT("Empty reusable stones retain zero mana and fractional percentages"),
        ChargeText.Contains(TEXT("Stored Mana: 0\nEfficiency: 75%\nChance of Destruction: 25%")));

    Gameplay->LastAppraisal = FACEAppraisalInfo();
    Gameplay->LastAppraisal.ObjectGuid=458; Gameplay->LastAppraisal.Name=TEXT("Creature appraisal");
    Gameplay->LastAppraisal.bIsCreature=true; Gameplay->LastAppraisal.bSuccess=true;
    Gameplay->LastAppraisal.CreatureType=38;
    Gameplay->LastAppraisal.Level=25; Gameplay->LastAppraisal.Strength=200;
    Gameplay->LastAppraisal.Coordination=140; Gameplay->LastAppraisal.Quickness=75;
    Gameplay->LastAppraisal.Health=150; Gameplay->LastAppraisal.MaxHealth=200;
    Gameplay->LastAppraisal.AttributeHighlights=1; Gameplay->LastAppraisal.AttributeColors=1;
    Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Creature uses nine authored attribute rows"), Gameplay->ExamAttributeRows.Num(), 9);
    TestEqual(TEXT("Creature type heading uses retail display name"), Gameplay->ExamCreatureHeadings[2]->GetText().ToString(), FString(TEXT("Fire Elemental")));
    TestEqual(TEXT("Level heading uses authored label"), Gameplay->ExamCreatureHeadings[1]->GetText().ToString(), FString(TEXT("Level")));
    TestEqual(TEXT("Creature panel separates coordination and quickness in retail order"), Gameplay->ExamAttributeValues[2]->GetText().ToString(), FString(TEXT("140")));
    TestEqual(TEXT("Creature health includes retail percent format"), Gameplay->ExamAttributeValues[6]->GetText().ToString(), FString(TEXT("150/200 (75 %)")));
    TestEqual(TEXT("Creature row uses the retail glyph font"), Gameplay->ExamAttributeRows[0]->Children[0]->FontId, uint32(0x40000001));
    TestEqual(TEXT("Creature pane suppresses summary text slab"), Gameplay->ExamBody->GetVisibility(), ESlateVisibility::Collapsed);
    CaptureScreen(TEXT("GameplayCreatureInspection"));
    const auto CreatureWindow=Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"));
    const int32 PreviousCreatureResize=CreatureWindow->UserResizeH;
    CreatureWindow->UserResizeH=400-CreatureWindow->AuthoredHeight;
    Gameplay->LastAppraisal.ObjectGuid=461;
    Gameplay->LastAppraisal.StringProperties = {{5,TEXT("War Mage")},{21,TEXT("Thwargnaught")},{35,TEXT("Patron Name")},{47,TEXT("The Allegiance")},{43,TEXT("29 March 2019")}};
    Gameplay->LastAppraisal.IntProperties = {{113,1},{188,1},{30,6},{35,91},{125,1481800},{43,6},{134,1},{307,9},{262,150},{350,3},{351,7},{281,1},{287,450}};
    Gameplay->LastAppraisal.ArmorLevels = {730,675,692,661,664,674,713,704,706};
    Gameplay->LastAppraisal.DamageRating = 9; Gameplay->LastAppraisal.CritDamageRating = 8;
    Gameplay->RefreshExaminationOverlay();
    auto DetailValue = [&](const TCHAR* Label) -> FString
    {
        for (int32 I=0;I<Gameplay->ExamMiscLabels.Num();++I)
            if (Gameplay->ExamMiscLabels[I]->GetVisibility()!=ESlateVisibility::Collapsed && Gameplay->ExamMiscLabels[I]->GetText().ToString()==Label)
                return Gameplay->ExamMiscValues[I]->GetText().ToString();
        return FString();
    };
    TestEqual(TEXT("Player inspection uses numeric gender and heritage"),Gameplay->ExamCreatureHeadings[3]->GetText().ToString(),FString(TEXT("Male Aluvian")));
    TestEqual(TEXT("Inspected character name includes its allegiance rank"),Gameplay->ExamTitle->GetText().ToString(),FString(TEXT("Ealdor Creature appraisal")));
    Gameplay->LastAppraisal.IntProperties.Add(113,2);Gameplay->LastAppraisal.IntProperties.Add(30,7);Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Inspection rank title respects gender"),Gameplay->ExamTitle->GetText().ToString(),FString(TEXT("Duchess Creature appraisal")));
    Gameplay->LastAppraisal.IntProperties.Add(30,11);Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Unrecognized rank leaves the subject name intact"),Gameplay->ExamTitle->GetText().ToString(),Gameplay->LastAppraisal.Name);
    Gameplay->LastAppraisal.IntProperties.Add(113,1);Gameplay->LastAppraisal.IntProperties.Add(30,6);Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Player profession has its own authored line"),Gameplay->ExamCreatureHeadings[4]->GetText().ToString(),FString(TEXT("War Mage")));
    TestEqual(TEXT("Creature heading is hidden for a character"),Gameplay->ExamCreatureHeadings[2]->GetVisibility(),ESlateVisibility::Collapsed);
    TestEqual(TEXT("Monarch name uses appraisal property 21"),DetailValue(TEXT("Monarch:")),FString(TEXT("Thwargnaught")));
    TestEqual(TEXT("Patron name uses appraisal property 35"),DetailValue(TEXT("Patron:")),FString(TEXT("Patron Name")));
    TestEqual(TEXT("Player examination includes body armor"),DetailValue(TEXT("Head/Chest/Groin")),FString(TEXT("AL: 730/675/692")));
    TestEqual(TEXT("Player examination includes deaths"),DetailValue(TEXT("Deaths:")),FString(TEXT("6")));
    TestEqual(TEXT("Society rating uses retail rank ranges"),DetailValue(TEXT("Society:")),FString(TEXT("Celestial Hand ~ Knight")));
    Gameplay->LastAppraisal.IntProperties.Add(134,ACEPlayerKillerStatus::PK);Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("PK status uses the actual PK flag"),Gameplay->ExamCreatureHeadings[5]->GetText().ToString(),FString(TEXT("Player Killer")));
    Gameplay->LastAppraisal.IntProperties.Add(134,ACEPlayerKillerStatus::PKLite);Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("PK Lite status uses the actual PK Lite flag"),Gameplay->ExamCreatureHeadings[5]->GetText().ToString(),FString(TEXT("Player Killer Lite")));
    {
        FACEWorldObject Subject;Subject.Guid=461;Subject.bIsPlayer=true;
        Subject.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
        Client->Session->WorldObjects.Add(Subject.Guid,Subject);
        Gameplay->LastAppraisal.IntProperties.Add(134,1); // earlier NPK appraisal remains open
        FACEBinaryWriter PKL;PKL.WriteUInt8(1);PKL.WriteUInt32(461);PKL.WriteUInt32(134);PKL.WriteInt32(0x40);
        FACEBinaryReader PKLR(PKL.GetData());Client->Session->HandlePublicUpdatePropertyInt(PKLR);
        Gameplay->RefreshExaminationOverlay();
        TestEqual(TEXT("Open inspection follows PKL broadcast without another Identify"),Gameplay->ExamCreatureHeadings[5]->GetText().ToString(),FString(TEXT("Player Killer Lite")));
        TestEqual(TEXT("PKL selection and radar use retail pink"),Gameplay->ResolveRadarColor(Client->Session->WorldObjects[461]),ACERadarColor::PKLite);
        Client->Session->WorldObjects.Remove(461);
        Gameplay->LastAppraisal.IntProperties.Add(134,ACEPlayerKillerStatus::PKLite);
    }
    CaptureScreen(TEXT("GameplayPlayerInspection"));
    const auto ExtraInfo=Manager->FindElementUnder(TEXT("BasicCreatureExamineUI"),TEXT("BasicCreatureExam_ExtraInfo"));
    const auto ExtraSlot=CastChecked<UCanvasPanelSlot>(Gameplay->ExamCreatureDetailsScroll->Slot);
    TestTrue(TEXT("Character details use the authored list bounds"), ExtraSlot->GetPosition().Equals(FVector2D(ExtraInfo->GetScreenOrigin())*Canvas->GetLastScale2D(),.01));
    TestTrue(TEXT("Complete character information can scroll"),Gameplay->ExamCreatureDetailsScroll->GetScrollOffsetOfEnd()>0.f);
    Gameplay->ExamCreatureDetailsScroll->SetScrollOffset(80.f);Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Repeated appraisal preserves the details scroll position"),Gameplay->ExamCreatureDetailsScroll->GetScrollOffset(),80.f);
    CaptureScreen(TEXT("GameplayPlayerInspectionScrolled"));
    // Exercise an explicit expansion, independent of the user's saved editor layout.
    CreatureWindow->UserResizeH=600-CreatureWindow->AuthoredHeight;Gameplay->RefreshExaminationOverlay();
    CaptureScreen(TEXT("GameplayPlayerInspectionExpanded"));
    TestTrue(TEXT("Extra information expands with the inspection frame"),ExtraInfo->Height>87);
    CreatureWindow->UserResizeH=PreviousCreatureResize;Gameplay->RefreshExaminationOverlay();
    Gameplay->LastAppraisal.IntProperties.Add(261,1);Gameplay->LastAppraisal.StringProperties.Remove(5);
    Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Title-only character selects character examination"),Gameplay->ExamCreatureHeadings[4]->GetText().ToString(),FString(TEXT("Adventurer")));
    Gameplay->LastAppraisal.IntProperties.Add(261,999999);Gameplay->LastAppraisal.StringProperties.Add(5,TEXT("War Mage"));
    Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Unknown display title falls back to the profession"),Gameplay->ExamCreatureHeadings[4]->GetText().ToString(),FString(TEXT("War Mage")));
    Gameplay->LastAppraisal.IntProperties.Add(261,1);Gameplay->LastAppraisal.StringProperties.Remove(5);
    Gameplay->LastAppraisal.IntProperties.Add(43,0);Gameplay->LastAppraisal.ArmorLevels[0]=9999+123;
    Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Unenchantable armor strips the wire sentinel"),DetailValue(TEXT("Head/Chest/Groin")),FString(TEXT("AL: *123/675/692")));
    TestEqual(TEXT("Retail zero-death wording"),DetailValue(TEXT("Deaths:")),FString(TEXT("Has never died")));
    // An NPC's gender/heritage alone do not make it a character appraisal.
    Gameplay->LastAppraisal.ObjectGuid=462;Gameplay->LastAppraisal.IntProperties.Remove(261);
    Gameplay->LastAppraisal.StringProperties.Add(3,TEXT("Male"));Gameplay->LastAppraisal.StringProperties.Add(4,TEXT("Aluvian"));
    Gameplay->LastAppraisal.CreatureType=31;Gameplay->RefreshExaminationOverlay();
    TestFalse(TEXT("NPC switches off the character header"),bool(Manager->FindElementUnder(TEXT("BasicCreatureExamineUI"),TEXT("CharacterExam_Attributes"))->bVisible));
    TestEqual(TEXT("Creature name omits character rank prefixes"),Gameplay->ExamTitle->GetText().ToString(),Gameplay->LastAppraisal.Name);
    TestEqual(TEXT("Character details do not leak into creature examination"),DetailValue(TEXT("Deaths:")),FString());
    TestEqual(TEXT("Switching subjects resets creature details scroll"),Gameplay->ExamCreatureDetailsScroll->GetScrollOffset(),0.f);
    TestEqual(TEXT("Creature resistance details remain available"),DetailValue(TEXT("DoT/Life:")),FString(TEXT("%Resist: 3/7")));
    CaptureScreen(TEXT("GameplayNPCInspection"));
    Gameplay->LastAppraisal.bSuccess=false; Gameplay->LastAppraisal.Strength=0;
    Gameplay->RefreshExaminationOverlay();
    TestEqual(TEXT("Failed appraisal retains only health percent"), Gameplay->ExamAttributeValues[6]->GetText().ToString(), FString(TEXT("75 %")));
    TestEqual(TEXT("Unknown attribute is not displayed as zero"), Gameplay->ExamAttributeValues[0]->GetText().ToString(), FString(TEXT("???")));
    Gameplay->LastAppraisal = ItemAppraisal;
    Gameplay->ShowExamination(false); Gameplay->RefreshExaminationOverlay();
    Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field")); Gameplay->RefreshInventoryOverlays();
    CaptureScreen(TEXT("GameplayInventory"));
    {
        const auto Page=Manager->FindElementByName(TEXT("InventoryPanel_Field"));
        const auto Background=Manager->FindElementUnder(TEXT("InventoryPanel_Field"),TEXT("InvBackgroundImage"));
        const auto Black=Manager->FindElementUnder(TEXT("InventoryPanel_Field"),TEXT("Blackness"));
        const int32 OldHeight=Page->Height;
        for(int32 Height:{362,600,900})
        {
            Page->Height=Height;Gameplay->ReflowInventoryPanelGeometry();
            TestEqual(TEXT("Inventory gradient retains retail authored height instead of repeating"),Background->Height,362);
            TestEqual(TEXT("Inventory black extension starts at retail gradient bottom"),Black->Y,361);
            TestEqual(TEXT("Inventory black extension covers the entire resized panel"),Black->Y+Black->Height,Height);
        }
        Page->Height=OldHeight;Gameplay->ReflowInventoryPanelGeometry();
    }
    TestTrue(TEXT("Inventory draws bound shortcut number overlay"),Gameplay->InventorySlotOverlays.Num()>0
        && Gameplay->InventorySlotOverlays[0]->GetVisibility()!=ESlateVisibility::Collapsed);
    auto* ShortcutArt=Cast<UTexture2D>(Gameplay->InventorySlotOverlays[0]->Background.GetResourceObject());
    TestTrue(TEXT("Inventory shortcut uses third DAT numeral"),ShortcutArt==Resources->ResolveIconTexture(0x060010A0));
    // A raised retail floaty moves both its DAT chrome and cached generated contents.
    // An opaque front window must also prevent dragging an inventory slot through it.
    const auto InventoryWindow=Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field"));
    const auto ExaminationWindow=Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"));
    auto* ItemBorder=Gameplay->InventorySlots[0].Get();
    const auto ItemGeometry=ItemBorder->GetCachedGeometry();
    const FVector2D ItemAbsolute=ItemGeometry.LocalToAbsolute(ItemGeometry.GetLocalSize()*.5);
    const FVector2D ItemLocal=Canvas->GetCachedGeometry().AbsoluteToLocal(ItemAbsolute);
    const FIntPoint PreviousExaminationDrag(ExaminationWindow->UserDragX,ExaminationWindow->UserDragY);
    const auto ExaminationOrigin=ExaminationWindow->GetScreenOrigin();
    ExaminationWindow->UserDragX+=FMath::RoundToInt(ItemLocal.X)-ExaminationOrigin.X-40;
    ExaminationWindow->UserDragY+=FMath::RoundToInt(ItemLocal.Y)-ExaminationOrigin.Y-80;
    Gameplay->ShowExamination(true); Manager->BringFloatyToFront(ExaminationWindow);
    CaptureScreen(TEXT("GameplayInspectionOverInventory"));
    TestFalse(TEXT("Inventory slot behind appraisal is not exposed"),Canvas->IsWidgetExposedAt(ItemBorder,ItemAbsolute));
    TestFalse(TEXT("Covered inventory grid cannot start a drag through its coordinate fallback"),Gameplay->TryBeginInventoryDrag(ItemLocal));
    TestTrue(TEXT("Front examination owns the covered point"),Manager->FindWindowAtCanvas(ItemLocal.X,ItemLocal.Y)==ExaminationWindow);
    Manager->BringFloatyToFront(InventoryWindow);
    CaptureScreen(TEXT("GameplayInventoryOverInspection"));
    TestTrue(TEXT("Raising inventory restores item hit testing without changing contents"),Canvas->IsWidgetExposedAt(ItemBorder,ItemAbsolute));
    TestTrue(TEXT("Raising inventory also raises cached item art over the appraisal body"),
        Cast<UCanvasPanelSlot>(ItemBorder->Slot)->GetZOrder()>Cast<UCanvasPanelSlot>(Gameplay->ExamScroll->Slot)->GetZOrder());
    TestTrue(TEXT("Visible inventory item starts a drag"),Gameplay->TryBeginInventoryDrag(ItemLocal));
    Gameplay->UpdateInventoryDrag(ItemLocal+FVector2D(24,0));
    Gameplay->TryFinishInventoryDrag(ItemLocal);
    TestEqual(TEXT("Drag back to its source slot cannot arm double-click use"),Gameplay->LastInvClickGuid,0);
    ExaminationWindow->UserDragX=PreviousExaminationDrag.X; ExaminationWindow->UserDragY=PreviousExaminationDrag.Y;
    Gameplay->ShowExamination(false);
    Gameplay->bInvDragPending=true; Gameplay->bInvDragActive=true; Gameplay->InvDragGuid=Item.Guid;
    Gameplay->InvDragSourcePack=Player.Guid; Gameplay->LastInvClickGuid=Item.Guid;
    const auto Title=Manager->FindElementByName(TEXT("InvTitleText"));
    if (Title) Gameplay->TryFinishInventoryDrag(FVector2D(Title->GetScreenOrigin())+FVector2D(8,8));
    TestEqual(TEXT("Cancelled item drag cannot arm double-click use"),Gameplay->LastInvClickGuid,0);
    if (FApp::CanEverRender())
    {
        // Exercise Slate's actual input path, not only our model hit-test helpers.
        // Visible native chat text used to win through HitTestInvisible DAT art.
        const auto ChatWindow=Manager->FindElementByName(TEXT("RootGameplay_FloatyMainChat_Field"));
        const auto CombatWindow=Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"));
        const auto VendorWindow=Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field"));
        const auto PreviousMode=Gameplay->CombatMode;
        Gameplay->ApplyCombatMode(static_cast<int32>(ACECombatMode::Magic));
        Gameplay->ShowVendorPanel(9876);
        FWidgetRenderer Renderer(true,true); FHittestGrid Grid;
        const auto Window=SNew(SVirtualWindow).Size(FVector2D(ScreenSize)); Window->SetContent(Slate);
        auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(ScreenSize),TF_Bilinear,true);
        auto Draw=[&]()
        {
            for(int32 Pass=0;Pass<3;++Pass)
            {
                ++GFrameCounter; Canvas->NativeTick(Canvas->GetCachedGeometry(),0.f);
                Renderer.DrawWindow(Target,Grid,Window,1.f,FVector2D(ScreenSize),0.f);
                FlushRenderingCommands();
            }
        };
        Manager->BringFloatyToFront(ChatWindow); Draw();
        for (UWidget* Chat : {static_cast<UWidget*>(Gameplay->ChatEntry.Get()),static_cast<UWidget*>(Gameplay->ChatLog.Get())})
        {
            const auto Geometry=Chat->GetCachedGeometry();
            FVector2D Point=Geometry.LocalToAbsolute(Chat==Gameplay->ChatEntry
                ? Geometry.GetLocalSize()*.5 : FVector2D(40,Geometry.GetLocalSize().Y-8));
            auto HitsChat=[&]()
            {
                for(const auto& Hit:Grid.GetBubblePath(Point,0,false))
                    if(Hit.Widget==Chat->TakeWidget()) return true;
                return false;
            };
            // Empty space in the log intentionally bubbles to DAT chrome. Find
            // an actual visible text row rather than assuming the last line fills it.
            if(Chat==Gameplay->ChatLog && !HitsChat())
                for(float Y=2;Y<Geometry.GetLocalSize().Y;++Y)
                {
                    Point=Geometry.LocalToAbsolute(FVector2D(40,Y));
                    if(HitsChat())break;
                }
            TestTrue(TEXT("Uncovered chat retains native editing/selection hit path"),HitsChat());
            for(const auto& Front:{CombatWindow,VendorWindow})
            {
                const FIntPoint Drag(Front->UserDragX,Front->UserDragY);
                const FIntPoint Origin=Front->GetScreenOrigin();
                const FVector2D Local=Canvas->ViewportToLayout(Canvas->GetCachedGeometry().AbsoluteToLocal(Point));
                Front->UserDragX+=FMath::RoundToInt(Local.X)-Origin.X-Front->Width/2;
                Front->UserDragY+=FMath::RoundToInt(Local.Y)-Origin.Y-Front->Height/2;
                Manager->BringFloatyToFront(Front); Draw();
                TestFalse(TEXT("Front spell/vendor window blocks native chat focus through its art"),HitsChat());
                TestTrue(TEXT("Front window owns the overlapping mouse point"),Manager->FindWindowAtCanvas(Local.X,Local.Y)==Front);
                Manager->BringFloatyToFront(ChatWindow); Draw();
                TestTrue(TEXT("Raising chat restores native input without rebuilding its text"),HitsChat());
                Front->UserDragX=Drag.X; Front->UserDragY=Drag.Y; Draw();
            }
        }
        Gameplay->HideVendorPanel(); Gameplay->ApplyCombatMode(PreviousMode);
    }
    Gameplay->bChatTargetPopupOpen=true; Gameplay->RefreshChatTargetPopup();
    CaptureScreen(TEXT("GameplayChatMenu"));
    TestTrue(TEXT("Say menu background is below its channel options"),
        Cast<UCanvasPanelSlot>(Gameplay->ChatTargetPopupBg->Slot)->GetZOrder()
        <Cast<UCanvasPanelSlot>(Gameplay->ChatTargetPopupRows[0]->Slot)->GetZOrder());
    TestEqual(TEXT("Chat menu uses all fourteen retail entries"),Gameplay->ChatTargetPopupRows.Num(),14);
    TestTrue(TEXT("Chat menu text uses the authored DAT font"),CastChecked<UACERetailTextBlock>(Gameplay->ChatTargetPopupRows[0])->GetBitmapFont()!=nullptr);
    {
        const auto Saved=Client->Session->SelectedObject;
        const int32 SavedChannel=Gameplay->ChatSendChannel;
        FACECharacterCreation ChatStrings;
        TestTrue(TEXT("Retail chat strings load from the language DAT"),ChatStrings.LoadStrings(Dat->GetDatDirectory(),0x23000001));
        const TCHAR* MenuKeys[]={TEXT("SquelchSelectedNoSelection"),TEXT("TellToMonarch"),TEXT("TellToSelectedNoSelection"),
            TEXT("TellToPatron"),TEXT("TellToAll"),TEXT("TellToVassals"),TEXT("TellToFellows"),TEXT("TellToAllegiance"),
            TEXT("TellToGeneral"),TEXT("TellToTrade"),TEXT("TellToLFG"),TEXT("TellToRoleplay"),TEXT("TellToSociety"),TEXT("TellToOlthoi")};
        Client->Session->SelectedObject={}; Gameplay->RefreshChatTargetPopup();
        for(int32 I=0;I<UE_ARRAY_COUNT(MenuKeys);++I)
            TestEqual(FString::Printf(TEXT("Chat menu row %d matches retail DAT wording"),I),
                Gameplay->ChatTargetPopupRows[I]->GetText().ToString(),ChatStrings.Text(FString(TEXT("ID_Chat_"))+MenuKeys[I]));
        const TCHAR* CaptionKeys[]={TEXT(""),TEXT("Fellows"),TEXT("Allegiance"),TEXT("Vassals"),TEXT("Patron"),TEXT("Monarch"),
            nullptr,TEXT("General"),TEXT("Trade"),TEXT("LFG"),TEXT("Roleplay"),TEXT("Society"),TEXT("Selected"),TEXT("Olthoi")};
        for(int32 I=0;I<UE_ARRAY_COUNT(CaptionKeys);++I)
        {
            if(!CaptionKeys[I])continue; // Co-vassals is a command-only extension.
            Gameplay->ChatSendChannel=I; Gameplay->RefreshChatChromeOverlays();
            TestEqual(FString::Printf(TEXT("Chat mode %d displays the retail compact caption"),I),
                Gameplay->ChatTargetLabel->GetText().ToString(),ChatStrings.Text(FString(TEXT("ID_Chat_ChatTargetMenu"))+CaptionKeys[I]));
        }
        Gameplay->ChatSendChannel=SavedChannel; Gameplay->RefreshChatChromeOverlays();
        FACEWorldObject NPC;NPC.Guid=0x123455;NPC.Name=TEXT("Eiichi");NPC.ItemType=ACEItemType::Creature;
        Client->Session->WorldObjects.Add(NPC.Guid,NPC);
        FACESelectedObject Selected;Selected.Guid=NPC.Guid;Selected.bValid=true;
        Client->Session->SelectedObject=Selected;Gameplay->RefreshChatTargetPopup();
        TestEqual(TEXT("Retail Tell selection includes NPC names"),Gameplay->ChatTargetPopupRows[2]->GetText().ToString(),FString(TEXT("Tell to Eiichi")));
        TestEqual(TEXT("Selected squelch caption matches the retail string template"),Gameplay->ChatTargetPopupRows[0]->GetText().ToString(),
            ChatStrings.Text(TEXT("ID_Chat_SquelchSelected"))+NPC.Name);
        TestTrue(TEXT("NPC Tell option is enabled"),Gameplay->ChatTargetPopupElements[2]->PaintState!=13);
        const auto SavedSquelches=Client->Session->Squelches;
        auto& Squelch=Client->Session->Squelches.AddDefaulted_GetRef();Squelch.Guid=NPC.Guid;Squelch.Name=NPC.Name;Squelch.Mask=-1;
        Gameplay->RefreshChatTargetPopup();
        TestEqual(TEXT("Ignored selection checks the squelch row like retail"),Gameplay->ChatTargetPopupElements[0]->PaintState,0x10000001u);
        TestEqual(TEXT("Ignoring keeps the retail squelch caption"),Gameplay->ChatTargetPopupRows[0]->GetText().ToString(),
            ChatStrings.Text(TEXT("ID_Chat_SquelchSelected"))+NPC.Name);
        Client->Session->Squelches=SavedSquelches;
        NPC.ItemType=ACEItemType::Container;Client->Session->WorldObjects[NPC.Guid]=NPC;Gameplay->RefreshChatTargetPopup();
        TestEqual(TEXT("Objects cannot receive selected tells"),Gameplay->ChatTargetPopupElements[2]->PaintState,13u);
        Client->Session->WorldObjects.Remove(NPC.Guid);Client->Session->SelectedObject=Saved;Gameplay->RefreshChatTargetPopup();
    }
    const auto Row=Gameplay->ChatTargetPopupRows[8];
    const FVector2D RowCenter=Row->GetCachedGeometry().LocalToAbsolute(Row->GetCachedGeometry().GetLocalSize()*.5);
    TestTrue(TEXT("Say menu accepts channel click"),Gameplay->TryHandleModalPopupClick(RowCenter));
    TestEqual(TEXT("Say menu selects General"),Gameplay->ChatSendChannel,7);
    for (int32 I=1; I<=4; ++I)
    {
        const FString Name=FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"),I);
        Gameplay->SetFloatyVisible(Name,true);
        if (const auto Root=Manager->FindElementByName(Name)) { Root->UserDragX=I*260; Root->UserDragY=100; }
    }
    Gameplay->PlaceFloatyChatOverlays(); CaptureScreen(TEXT("GameplayFloatyChats"));
    {
        TGuardValue<FString> SettingsPath(GGameUserSettingsIni,FPaths::ProjectSavedDir()/TEXT("Automation/ChatRoutingFixture.ini"));
        FConfigFile FixtureConfig; FixtureConfig.NoSave=false; FixtureConfig.bCanSaveAllSections=true;
        GConfig->SetFile(GGameUserSettingsIni,&FixtureConfig);
        Gameplay->LoadFloatyChatSettings();
        TestEqual(TEXT("First auxiliary window defaults to speech and tells"),Gameplay->FloatyChatFilters[0],4124ull);
        TestEqual(TEXT("Fourth auxiliary window defaults to public channels"),Gameplay->FloatyChatFilters[3],2013265920ull);
        Gameplay->ShowPanelPage(TEXT("OptionsPanel_Field"));
        Gameplay->SyncOptionsPanelTab(TEXT("ChatPage"));
        TestTrue(TEXT("Chat options include all five routing sections"),Gameplay->GetOptionsRowCount()>70);
        TestEqual(TEXT("Both chat opacity sliders are present"),Gameplay->ChatOpacitySliders.Num(),2);
        for (USlider* OpacityControl : Gameplay->ChatOpacitySliders)
            TestTrue(TEXT("Chat opacity is visible at the top of its page"),OpacityControl && OpacityControl->GetVisibility()!=ESlateVisibility::Collapsed);

        TestEqual(TEXT("Both retail opacity controls are present"),Gameplay->ChatOpacitySliders.Num(),2);
        Gameplay->ChangeInactiveChatOpacity(.7f);
        Gameplay->ChangeActiveChatOpacity(.3f);
        Gameplay->LoadFloatyChatSettings();
        TestEqual(TEXT("Active opacity survives reload"),Gameplay->ChatActiveOpacity,.3f);
        TestEqual(TEXT("Retail opacity link keeps inactive below active"),Gameplay->ChatInactiveOpacity,.3f);
        Gameplay->RefreshChatOpacity();
        for (int32 W=1;W<=4;++W)
            TestEqual(TEXT("Auxiliary window background applies saved opacity"),Manager->FindElementByName(
                FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"),W))->ImageOpacity,.3f);
        constexpr int32 MainGeneral=1000+7, WindowTwoGeneral=1000+2*UE_ARRAY_COUNT(ACERetailChat::FilterGroups)+7;
        Gameplay->ToggleCharacterOption(MainGeneral);
        Gameplay->ToggleCharacterOption(WindowTwoGeneral);
        Gameplay->LoadFloatyChatSettings();
        TestTrue(TEXT("Main window routing survives reload"),(Gameplay->MainChatTypeFilter&(1ull<<27))==0);
        TestTrue(TEXT("Auxiliary routing survives reload"),(Gameplay->FloatyChatFilters[1]&(1ull<<27))!=0);
        Gameplay->OptionsScrollOffset=1000; Gameplay->RefreshOptionsOverlays();
        CaptureScreen(TEXT("GameplayChatWindowFourOptions"));
        TestTrue(TEXT("Routing list can reach the fourth auxiliary window"),Gameplay->OptionRowOptions.Contains(1000+4*UE_ARRAY_COUNT(ACERetailChat::FilterGroups)+7));
        Gameplay->HandleOptionsNamedClick(TEXT("ResetButton"));
        TestEqual(TEXT("Reset restores inactive opacity"),Gameplay->ChatInactiveOpacity,.5f);
        TestEqual(TEXT("Reset restores active opacity"),Gameplay->ChatActiveOpacity,1.f);
        TestTrue(TEXT("Reset restores the main routing snapshot"),(Gameplay->MainChatTypeFilter&(1ull<<27))!=0);
        TestTrue(TEXT("Reset restores the auxiliary routing snapshot"),(Gameplay->FloatyChatFilters[1]&(1ull<<27))==0);
    }
    FSlateApplication::Get().ClearKeyboardFocus();
    const FKeyEvent ChatEnterKey(EKeys::Enter,FModifierKeysState(),0,false,0,0);
    TestTrue(TEXT("Enter reaches chat before a focused option button can consume it"),
        Canvas->NativeOnPreviewKeyDown(Canvas->GetCachedGeometry(),ChatEnterKey).IsEventHandled());
    const bool MouseBefore=Client->IsCharacterOptionSet(0x31);
    Gameplay->ShowPanelPage(TEXT("OptionsPanel_Field"));
    Gameplay->HandleOptionsNamedClick(TEXT("GameplayOptions_MouseTurning_Button"));
    TestTrue(TEXT("Mouse turning option updates live client preference"),Client->IsCharacterOptionSet(0x31)!=MouseBefore);
    TestTrue(TEXT("Opening options highlights its toolbar icon"),Manager->FindElementByName(TEXT("PanelButton_OptionsButton"))->bHighlighted);
    Gameplay->SyncOptionsPanelTab(TEXT("CharacterSettingsPage"));
    Gameplay->RefreshOptionsOverlays();
    CaptureScreen(TEXT("GameplayCharacterOptions"));
    {
        const auto OriginalSize=ScreenSize;ScreenSize=FIntPoint(2048,1536);
        // Desktop offsets change which chat window is under the real cursor.
        // Freeze its intentional hover opacity while comparing static rendering.
        TGuardValue<float> ChatOpacity(Gameplay->ChatActiveOpacity,Gameplay->ChatInactiveOpacity);
        const auto AtOrigin=CaptureScreen(TEXT("GameplayOptions2048x1536Scale125"),1.25f);
        for (const FVector2D Offset : {FVector2D(240,160),FVector2D(-1920,320)})
        {
            const auto Moved=CaptureScreen(FString::Printf(TEXT("GameplayOptionsWindowOffset_%.0f"),Offset.X),1.25f,Offset);
            int32 Differences=0;
            if (Moved.Num()==AtOrigin.Num()) for (int32 I=0;I<Moved.Num();++I) Differences+=Moved[I]!=AtOrigin[I];
            TestTrue(TEXT("Moving a scaled window to either monitor preserves every static label"),Moved.Num()==AtOrigin.Num() && Differences==0);
        }
        ScreenSize=OriginalSize;
    }
    TestTrue(TEXT("Character settings page has live option rows"),
        Gameplay->OptionRowOptions.ContainsByPredicate([](int32 Option){return Option!=INDEX_NONE;}));
    {
        TGuardValue<FString> SettingsPath(GGameUserSettingsIni,FPaths::ProjectSavedDir()/TEXT("Automation/CameraUIFixture.ini"));
        FConfigFile FixtureConfig; FixtureConfig.NoSave=false; FixtureConfig.bCanSaveAllSections=true;
        GConfig->SetFile(GGameUserSettingsIni,&FixtureConfig);
        Gameplay->SyncOptionsPanelTab(TEXT("ConfigPage")); Gameplay->RefreshOptionsOverlays();
        CaptureScreen(TEXT("GameplayVideoOptions"));
        auto* Video=Cast<UACEVideoSettingsWidget>(Gameplay->VideoSettings);
        if (TestNotNull(TEXT("Config contains engine and camera controls"),Video))
        {
            const float OldScroll=Video->GetScrollOffset();
            auto* Shadow=Video->WidgetTree->FindWidget(TEXT("ShadowQuality"));
            auto* Lighting=Video->WidgetTree->FindWidget(TEXT("DynamicLightingQuality"));
            if(TestNotNull(TEXT("Video settings include shadow control"),Shadow) && TestNotNull(TEXT("Video settings include dynamic lighting control"),Lighting))
            {
                CastChecked<UScrollBox>(Video->WidgetTree->RootWidget)->ScrollWidgetIntoView(Shadow,false,EDescendantScrollDestination::TopOrLeft,20.f);
                CaptureScreen(TEXT("GameplayLightingOptions"));
                const float LightingBottom=Video->GetCachedGeometry().AbsoluteToLocal(Lighting->GetCachedGeometry().LocalToAbsolute(Lighting->GetCachedGeometry().GetLocalSize())).Y;
                TestTrue(TEXT("Lighting controls fit the scrolled configuration page"),LightingBottom>0 && LightingBottom<=Video->GetCachedGeometry().GetLocalSize().Y);
                Video->SetScrollOffset(OldScroll);
            }
            Video->ResetVideo(); Gameplay->RefreshOptionsOverlays();
            const auto PendingApply=Manager->FindElementUnder(TEXT("ConfigPage"),TEXT("ApplyButton"));
            const auto PendingReset=Manager->FindElementUnder(TEXT("ConfigPage"),TEXT("ResetButton"));
            TestFalse(TEXT("Unchanged configuration has no pending changes"),Video->HasPendingChanges());
            CheckCameraPreview(*this,Gameplay,Video,[&](const TCHAR* Page){Gameplay->SyncOptionsPanelTab(Page);});
            Gameplay->RefreshOptionsOverlays();
            TestTrue(TEXT("Unchanged PendingApply and PendingReset use retail disabled state"),PendingApply && PendingReset && !PendingApply->bActivatable && !PendingReset->bActivatable);
            auto* Toggle=Cast<UCheckBox>(Video->WidgetTree->FindWidget(TEXT("InvertMouseX")));
            if(Toggle)
            {
                Toggle->SetIsChecked(!Toggle->IsChecked());Gameplay->RefreshOptionsOverlays();
                TestTrue(TEXT("Changing an option enables Apply and Reset"),Video->HasPendingChanges() && PendingApply && PendingReset && PendingApply->bActivatable && PendingReset->bActivatable);
                Toggle->SetIsChecked(!Toggle->IsChecked());Gameplay->RefreshOptionsOverlays();
                TestTrue(TEXT("Restoring the original value disables both buttons"),!Video->HasPendingChanges() && PendingApply && PendingReset && !PendingApply->bActivatable && !PendingReset->bActivatable);
            }
            int32 Collapses=0;
            const auto VisibilityHandle=Video->OnNativeVisibilityChanged.AddLambda([&](ESlateVisibility V)
            { if (V==ESlateVisibility::Collapsed || V==ESlateVisibility::Hidden) ++Collapses; });
            for(int32 Refresh=0;Refresh<5;++Refresh) Gameplay->RefreshOptionsOverlays();
            Video->OnNativeVisibilityChanged.Remove(VisibilityHandle);
            TestEqual(TEXT("Config refresh never hides the active dropdown owner"),Collapses,0);
            TestTrue(TEXT("Full configuration extends past the viewport"),Video->GetScrollEnd()>100.f);
            Gameplay->ScrollOptionsList(-7.5f);
            TestTrue(TEXT("The shared config scrollbar scrolls actual controls"),Video->GetScrollOffset()>0.f);
            CaptureScreen(TEXT("GameplayDisplaySliders"));
            Video->SetScrollOffset(0.f);

            auto* Speed=Cast<USlider>(Video->WidgetTree->FindWidget(TEXT("MouseTurnSpeed")));
            auto* InvertX=Cast<UCheckBox>(Video->WidgetTree->FindWidget(TEXT("InvertMouseX")));
            auto* InvertY=Cast<UCheckBox>(Video->WidgetTree->FindWidget(TEXT("InvertMouseY")));
            auto* UIScale=Cast<UComboBoxString>(Video->WidgetTree->FindWidget(TEXT("DesktopUIScale")));
            auto* CursorScale=Cast<USlider>(Video->WidgetTree->FindWidget(TEXT("CursorScale")));
            if(TestNotNull(TEXT("Options has a cursor size slider"),CursorScale))
            {
                Video->ChangeCursorScale(1.74f);
                TestEqual(TEXT("Cursor uses readable quarter steps"),CursorScale->GetValue(),1.75f);
                FConfigFile CursorConfig;CursorConfig.Read(GGameUserSettingsIni);float SavedCursor=0;
                CursorConfig.GetFloat(TEXT("ACE.Presentation"),TEXT("CursorScale"),SavedCursor);
                TestEqual(TEXT("Cursor preview is saved immediately"),SavedCursor,1.75f);
                Video->ChangeCursorScale(.1f);TestEqual(TEXT("Cursor cannot become too small to find"),CursorScale->GetValue(),.5f);
                Video->ChangeCursorScale(1.f);
            }
            if (TestNotNull(TEXT("Horizontal mouse inversion control"),InvertX)
                && TestNotNull(TEXT("Vertical mouse inversion control"),InvertY)
                && TestNotNull(TEXT("Desktop UI scale control"),UIScale))
            {
                TestEqual(TEXT("UI scale offers nine clear quarter steps"),UIScale->GetOptionCount(),9);
                InvertX->SetIsChecked(true);InvertY->SetIsChecked(false);UIScale->SetSelectedIndex(4);
                Video->ApplyInterfaceOptions();
                TestTrue(TEXT("Apply saves independent mouse axes"),ACECameraSettings::GetInvertMouseX() && !ACECameraSettings::GetInvertMouseY());
                FConfigFile Saved;Saved.Read(GGameUserSettingsIni);float SavedScale=0;
                Saved.GetFloat(TEXT("ACE.Presentation"),TEXT("DesktopUIScale"),SavedScale);
                TestEqual(TEXT("UI scale persists to disk"),SavedScale,2.f);
                TestEqual(TEXT("4K supports crisp double-sized UI"),ACERuntimeOptions::DesktopUIScale(FIntPoint(3840,2160),false),2.f);
                TestEqual(TEXT("1080p scale is limited to keep the UI accessible"),ACERuntimeOptions::DesktopUIScale(FIntPoint(1920,1080),false),1.75f);
                TestEqual(TEXT("Small windows keep the native layout accessible"),ACERuntimeOptions::DesktopUIScale(FIntPoint(800,600),false),1.f);
                TestEqual(TEXT("VR surfaces retain their own scale"),ACERuntimeOptions::DesktopUIScale(FIntPoint(3840,2160),true),1.f);
                {
                    TGuardValue<float> RestoreScale(GetMutableDefault<UUserInterfaceSettings>()->ApplicationScale,1.f);
                    ACERuntimeOptions::ApplyDesktopUIScale(FIntPoint(3840,2160),false);
                    TestEqual(TEXT("The actual viewport DPI doubles all child widgets"),GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(FIntPoint(3840,2160)),2.f);
                    ACERuntimeOptions::ApplyDesktopUIScale(FIntPoint(3840,2160),true);
                    TestEqual(TEXT("Entering VR removes desktop DPI scaling"),GetDefault<UUserInterfaceSettings>()->ApplicationScale,1.f);
                }
                auto* Reopened=NewObject<UACEVideoSettingsWidget>();Reopened->Initialize();auto ReopenedSlate=Reopened->TakeWidget();
                auto* ReloadedScale=Cast<UComboBoxString>(Reopened->WidgetTree->FindWidget(TEXT("DesktopUIScale")));
                TestTrue(TEXT("Reopening configuration preserves scale"),ReloadedScale && ReloadedScale->GetSelectedOption()==TEXT("200%"));
                // A player-sized settings window can be taller than the logical
                // viewport after DPI changes, despite the 800x600 global limit.
                const auto Floaty=Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field"));
                const auto Apply=Manager->FindElementUnder(TEXT("ConfigPage"),TEXT("ApplyButton"));
                if(TestTrue(TEXT("Settings frame and Apply button exist"),Floaty && Apply))
                {
                    const FIntPoint OriginalScreen=ScreenSize;
                    const int32 OriginalHeight=Floaty->GetLayoutHeight();
                    const int32 OriginalDragX=Floaty->UserDragX,OriginalDragY=Floaty->UserDragY;
                    // Viewport clamping also moves the other visible windows.
                    // Restore those positions before subsequent shortcut tests;
                    // otherwise the resized inventory obscures the shortcut bar.
                    TArray<TPair<TSharedPtr<FACEUIElement>,FIntPoint>> WindowPositions;
                    if(const auto Root=Floaty->Parent.Pin())for(const auto& Window:Root->Children)
                        if(Window)WindowPositions.Emplace(Window,FIntPoint(Window->UserDragX,Window->UserDragY));
                    // Isolate the interface setting from ApplyVideo's OS display
                    // changes; retain the actual canvas hit testing and activation.
                    Manager->OnElementActivated.Remove(Gameplay->ActivatedHandle);
                    UACEUIElementManager::ApplyFloatyResizeLayout(Floaty);
                    for(const FIntPoint Size:{FIntPoint(1920,1080),FIntPoint(2560,1440),FIntPoint(3840,2160),FIntPoint(800,600),FIntPoint(640,480),FIntPoint(800,450),FIntPoint(480,360)})
                    {
                        ScreenSize=Size;
                        CaptureScreen(FString::Printf(TEXT("OptionsWindowResize_%d"),Size.X));
                        // The existing resize handle permits 320 extra pixels.
                        Floaty->UserResizeH=320;
                        UACEUIElementManager::ApplyFloatyResizeLayout(Floaty);
                        CaptureScreen(FString::Printf(TEXT("OptionsBeforeScale_%d"),Size.X));
                        ACERuntimeOptions::Set(TEXT("DesktopUIScale"),3.f);
                        const float Scale=ACERuntimeOptions::DesktopUIScale(Size,false);
                        CaptureScreen(FString::Printf(TEXT("OptionsAfterScale_%d"),Size.X),Scale);
                        const FVector2D LogicalSize=FVector2D(Size)/Scale;
                        const FIntPoint Origin=Apply->GetScreenOrigin();
                        AddInfo(FString::Printf(TEXT("Scaled options %dx%d at %.2f: frame=%s/%d Apply=%s/%d"),
                            Size.X,Size.Y,Scale,*Floaty->GetScreenOrigin().ToString(),Floaty->Height,*Origin.ToString(),Apply->Height));
                        TestTrue(TEXT("Scaled settings frame fits the visible viewport"),
                            // Fractional float DPI (e.g. .8) can put the double
                            // logical extent a few millionths below 600 pixels.
                            Floaty->GetScreenOrigin().Y>=0 && Floaty->GetScreenOrigin().Y+Floaty->Height<=LogicalSize.Y+.01);
                        TestTrue(TEXT("Apply remains entirely onscreen after enlarging the interface"),
                            Origin.X>=0 && Origin.Y>=0 && Origin.X+Apply->Width<=LogicalSize.X && Origin.Y+Apply->Height<=LogicalSize.Y);
                        const FVector2D ScaleOrigin=Canvas->GetCachedGeometry().AbsoluteToLocal(UIScale->GetCachedGeometry().GetAbsolutePosition());
                        const FVector2D ScaleEnd=Canvas->GetCachedGeometry().AbsoluteToLocal(
                            UIScale->GetCachedGeometry().LocalToAbsolute(UIScale->GetCachedGeometry().GetLocalSize()));
                        TestTrue(TEXT("Scale selector remains visible for reducing the UI size"),
                            ScaleOrigin.X>=0 && ScaleOrigin.Y>=0 && ScaleEnd.X<=LogicalSize.X && ScaleEnd.Y<=LogicalSize.Y);
                        const FVector2D Center(Origin.X+Apply->Width/2,Origin.Y+Apply->Height/2);
                        TestTrue(TEXT("Apply remains the pointer hit target after scaling"),Manager->HitTestCanvas(Center.X,Center.Y)==Apply);
                        UIScale->SetSelectedIndex(0);
                        bool Clicked=false;
                        const auto ClickHandle=Manager->OnElementActivated.AddLambda([&](TSharedPtr<FACEUIElement> E){Clicked|=E==Apply;});
                        const FGeometry& Geometry=Canvas->GetCachedGeometry();
                        const FVector2D Absolute=Geometry.LocalToAbsolute(Center);
                        const FPointerEvent Down(0,Absolute,Absolute,TSet<FKey>{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0.f,FModifierKeysState());
                        const FPointerEvent Up(0,Absolute,Absolute,TSet<FKey>{},EKeys::LeftMouseButton,0.f,FModifierKeysState());
                        // Exercise the scaled canvas input route without applying
                        // OS resolution changes in this automated render fixture.
                        const auto Handler=Manager->OnElementActivated.AddLambda([&](TSharedPtr<FACEUIElement> E)
                        { if(E==Apply)Video->ApplyInterfaceOptions(); });
                        Canvas->NativeOnMouseButtonDown(Geometry,Down);
                        Canvas->NativeOnMouseButtonUp(Geometry,Up);
                        Manager->OnElementActivated.Remove(Handler);
                        Manager->OnElementActivated.Remove(ClickHandle);
                        TestTrue(TEXT("Physical click on scaled Apply reaches the control"),Clicked);
                        TestEqual(TEXT("Player can return to 100% using the visible Apply button"),ACERuntimeOptions::Get(TEXT("DesktopUIScale")),1.f);
                    }
                    const float OldConstrain=ACERuntimeOptions::Get(TEXT("ConstrainUIWindows"));
                    ACERuntimeOptions::Set(TEXT("ConstrainUIWindows"),0);
                    Floaty->UserDragX=10000;Floaty->UserDragY=10000;Floaty->RecomputeLayoutOffset();
                    Gameplay->ShowPanelPage(TEXT("OptionsPanel_Field"));
                    Gameplay->SyncOptionsPanelTab(TEXT("GameplayOptionsPage"));
                    CaptureScreen(TEXT("SmallResolutionRecovery"),ACERuntimeOptions::DesktopUIScale(ScreenSize,false));
                    const auto Recovery=Manager->FindElementByName(TEXT("GameplayOptions_ResetGraphics_Button"));
                    if(TestTrue(TEXT("Options expose Reset graphics without scrolling"),Recovery.IsValid()))
                    {
                        const auto At=Recovery->GetScreenOrigin();
                        TestTrue(TEXT("Reset graphics stays in the recovery viewport"),At.X>=0 && At.Y>=0 && At.X+Recovery->Width<=800 && At.Y+Recovery->Height<=600);
                        TestTrue(TEXT("Reset graphics is clickable with offscreen windows allowed"),Manager->HitTestCanvas(At.X+Recovery->Width/2,At.Y+Recovery->Height/2)==Recovery);
                    }
                    ACERuntimeOptions::Set(TEXT("ConstrainUIWindows"),OldConstrain);
                    Gameplay->SyncOptionsPanelTab(TEXT("ConfigPage"));
                    ScreenSize=OriginalScreen;
                    // Refresh cached Slate geometry before restoring sizes. A
                    // tick with the old 800x600 geometry would clamp them again.
                    CaptureScreen(TEXT("OptionsViewportRestored"));
                    Floaty->UserResizeH=OriginalHeight-Floaty->AuthoredHeight;
                    Floaty->UserDragX=OriginalDragX;Floaty->UserDragY=OriginalDragY;
                    Floaty->RecomputeLayoutOffset();UACEUIElementManager::ApplyFloatyResizeLayout(Floaty);
                    for(const auto& SavedPosition:WindowPositions) {
                        SavedPosition.Key->UserDragX=SavedPosition.Value.X;
                        SavedPosition.Key->UserDragY=SavedPosition.Value.Y;
                        SavedPosition.Key->RecomputeLayoutOffset();
                    }
                    CaptureScreen(TEXT("OptionsScaleRestored"));
                    Gameplay->ActivatedHandle=Manager->OnElementActivated.AddUObject(Gameplay,&UACEUIGameplayBinder::OnElementActivated);
                }
                Video->DefaultsVideo();Video->ApplyInterfaceOptions();
                TestFalse(TEXT("Defaults restore normal mouse X"),ACECameraSettings::GetInvertMouseX());
                TestFalse(TEXT("Defaults restore normal mouse Y"),ACECameraSettings::GetInvertMouseY());
                TestEqual(TEXT("Defaults restore native UI scale"),ACERuntimeOptions::Get(TEXT("DesktopUIScale")),1.f);
                auto* FPS=Cast<UCheckBox>(Video->WidgetTree->FindWidget(TEXT("ShowFrameRate")));
                if(TestNotNull(TEXT("FPS overlay is available in configuration"),FPS))
                {
                    FPS->SetIsChecked(true);Video->ApplyInterfaceOptions();
                    TestEqual(TEXT("FPS toggle saves"),ACERuntimeOptions::Get(TEXT("ShowFrameRate")),1.f);
                    auto* Counter=NewObject<UACEFrameRateWidget>();Counter->Initialize();auto CounterSlate=Counter->TakeWidget();
                    Counter->Sample(0.);for(int32 I=1;I<=45;++I)Counter->Sample(I/90.);
                    TestEqual(TEXT("FPS measures actual frames over elapsed time"),Counter->GetCounterText(),FString(TEXT("90 FPS  |  11.1 ms")));
                    Counter->Sample(1.5);
                    TestEqual(TEXT("A stall is included rather than using clamped simulation delta"),Counter->GetCounterText(),FString(TEXT("1 FPS  |  1000.0 ms")));
                    FPS->SetIsChecked(false);Video->ApplyInterfaceOptions();
                }
            }
            if (TestNotNull(TEXT("Config exposes mouse turn speed"),Speed))
            {
                TestEqual(TEXT("Config displays default speed"),Speed->GetValue(),1.f);
                Speed->SetValue(2.f); Speed->OnValueChanged.Broadcast(2.f);
                TestEqual(TEXT("Selecting a speed applies immediately"),ACECameraSettings::GetMouseTurnSpeed(),2.f);
                FConfigFile Saved; Saved.Read(GGameUserSettingsIni);
                GConfig->SetFile(GGameUserSettingsIni,&Saved);
                auto* Reopened=NewObject<UACEVideoSettingsWidget>(); Reopened->Initialize();
                auto ReopenedSlate=Reopened->TakeWidget();
                auto* ReloadedSpeed=Cast<USlider>(Reopened->WidgetTree->FindWidget(TEXT("MouseTurnSpeed")));
                TestTrue(TEXT("Recreated Config UI reads the persisted speed"),ReloadedSpeed && ReloadedSpeed->GetValue()==2.f);
                TestTrue(TEXT("Slider uses the retail thumb texture"),Speed->GetWidgetStyle().NormalThumbImage.GetResourceObject()==Resources->ResolveTexture(0x06001286));
                Gameplay->HandleOptionsNamedClick(TEXT("DefaultButton"));
                TestEqual(TEXT("Default speed restores the saved preference"),ACECameraSettings::GetMouseTurnSpeed(),1.f);
                TestEqual(TEXT("Default speed updates the visible selector"),Speed->GetValue(),1.f);
            }
        }
    }
    // Footer captions are flattened overlays, so exercise returning to the first
    // page after every list page, as well as a repeated frame refresh.
    for (const TCHAR* PageName : {TEXT("CharacterSettingsPage"),TEXT("GameplayOptionsPage"),
        TEXT("ChatPage"),TEXT("GameplayOptionsPage"),TEXT("ConfigPage"),TEXT("GameplayOptionsPage")})
    {
        Gameplay->SyncOptionsPanelTab(PageName);
        Gameplay->RefreshOptionsOverlays();Gameplay->RefreshOptionsOverlays();
        const bool GameplayPage=FString(PageName)==TEXT("GameplayOptionsPage");
        int32 VisibleLabels=0;
        for (int32 Index=0;Index<Gameplay->OptionsButtonLabels.Num();++Index)
        {
            auto* Label=Cast<UACERetailTextBlock>(Gameplay->OptionsButtonLabels[Index]);
            if (!Label) continue;
            const bool ShouldShow=GameplayPage?Index<9:Index>=9;
            TestEqual(TEXT("Options captions belong only to the active tab"),Label->GetVisibility()!=ESlateVisibility::Collapsed,ShouldShow);
            if (!ShouldShow) continue;
            ++VisibleLabels;
            const auto Button=Label->GetRetailElement();
            const auto* Slot=Cast<UCanvasPanelSlot>(Label->Slot);
            if (TestTrue(TEXT("Options caption is attached to its authored button"),Button && Slot))
            {
                const FVector2D Scale=Canvas->GetLastScale2D();
                TestTrue(TEXT("Caption retains the full button rectangle for retail centering"),
                    Slot->GetPosition().Equals(FVector2D(Button->GetScreenOrigin())*Scale,.01)
                    && Slot->GetSize().Equals(FVector2D(Button->Width,Button->Height)*Scale,.01));
                TestTrue(TEXT("Caption uses the authored font with enough room for all glyphs"),
                    Label->GetBitmapFont() && Label->GetBitmapFont()->MaxCharHeight*Scale.Y<=Slot->GetSize().Y);

            }
        }
        TestEqual(TEXT("Only the active page's button captions are visible"),VisibleLabels,GameplayPage?9:3);
        if (GameplayPage)
        {
            const auto PluginButton=Manager->FindElementUnder(PageName,TEXT("GameplayOptions_Plugins_Button"));
            TestTrue(TEXT("Plugin manager button fits inside options"),PluginButton && PluginButton->Parent.IsValid()
                && PluginButton->Y+PluginButton->Height<=PluginButton->Parent.Pin()->Height);
            CaptureScreen(TEXT("GameplayOptionsReturn"));
        }
        else if (FString(PageName)==TEXT("ChatPage")) CaptureScreen(TEXT("GameplayChatOptions"));
    }
    {
        // Isolate persistence from the player's real key mappings.
        TGuardValue<FString> SettingsPath(GGameUserSettingsIni,FPaths::ProjectSavedDir()/TEXT("Automation/InputFixture.ini"));
        FConfigFile FixtureConfig; FixtureConfig.NoSave=false; FixtureConfig.bCanSaveAllSections=true;
        GConfig->SetFile(GGameUserSettingsIni,&FixtureConfig);
        ACEInputBindings::Reload(); ACEInputBindings::BeginEdit(); ACEInputBindings::Defaults(); ACEInputBindings::Commit();
        Gameplay->ToggleKeyboardMappingUI(); Gameplay->RefreshKeyboardOverlays();
        CaptureScreen(TEXT("GameplayKeyboardMovement"));
        Gameplay->HandleKeyboardNamedClick(TEXT("KeyboardLoadKeymapButton"));
        TestTrue(TEXT("Retail keymap importer opens inside the VR-compatible panel"),Gameplay->bKeymapImportOpen && Gameplay->KeymapImportPath);
        CaptureScreen(TEXT("GameplayKeyboardImport"));
        {
            const auto Frame=Manager->FindElementByName(TEXT("KeyboardFrame"));
            TestEqual(TEXT("Modal keeps the keyboard frame's origin after window reflow"),Gameplay->KeymapDialog->GetScreenOrigin(),Frame->GetScreenOrigin());
            TestEqual(TEXT("Modal retains the keyboard frame's width instead of stretching again"),Gameplay->KeymapDialog->Width,Frame->Width);
            int32 DialogChromeMin=MAX_int32;
            TArray<TSharedPtr<FACEUIElement>> Nodes{Gameplay->KeymapDialog};
            while(!Nodes.IsEmpty())
            {
                auto Node=Nodes.Pop(EAllowShrinking::No);Nodes.Append(Node->Children);
                if(const auto* Paint=Canvas->PaintStates.Find(Node->InstanceId))DialogChromeMin=FMath::Min(DialogChromeMin,Paint->Z);
            }
            for(auto* Labels:{&Gameplay->KeyboardLabels,&Gameplay->KeyboardRowLabels,&Gameplay->KeyboardKeyLabels})
                for(UTextBlock* Label:*Labels)if(Label&&Label->IsVisible())
                    TestTrue(TEXT("File-dialog chrome covers all underlying keyboard labels"),CastChecked<UCanvasPanelSlot>(Label->Slot)->GetZOrder()<DialogChromeMin);
            TestFalse(TEXT("Underlying mapping buttons cannot capture input through the file dialog"),Gameplay->KeyboardRows[0]->GetIsEnabled());
            const auto Page=Gameplay->ActiveKeyboardPage;Gameplay->HandleKeyboardNamedClick(TEXT("CameraTab"));
            TestEqual(TEXT("Modal file dialog blocks underlying tabs"),Gameplay->ActiveKeyboardPage,Page);
        }
        NativeClick(TEXT("KeymapFileCancel"));
        TestFalse(TEXT("Actual dialog Cancel closes the modal"),Gameplay->bKeymapImportOpen);
        const FString CustomPath=FPaths::ProjectSavedDir()/TEXT("Automation/CustomFixture.keymap");
        FFileHelper::SaveStringToFile(RetailCustomKeymap,*CustomPath);
        Gameplay->HandleKeymapImport(CustomPath);
        CaptureScreen(TEXT("GameplayKeyboardImportReport"));
        TestTrue(TEXT("Custom import exposes a scrollable report"),Gameplay->bKeymapReport && Gameplay->KeymapReportScroll->IsVisible());
        TestTrue(TEXT("Report distinguishes unchanged native contexts"),Gameplay->KeymapReportText->GetText().ToString().Contains(TEXT("Separate UI/system maps")));
        const auto ReportGeometry=Gameplay->KeymapReportScroll->GetCachedGeometry();
        const auto ReviewGeometry=Gameplay->KeymapDialogLabels[1]->GetCachedGeometry();
        TestTrue(TEXT("Report scroll area ends above the dialog buttons"),ReportGeometry.LocalToAbsolute(ReportGeometry.GetLocalSize()).Y<=ReviewGeometry.GetAbsolutePosition().Y);
        NativeClick(TEXT("KeymapFileOK"));
        TestFalse(TEXT("Review returns to the mapping draft"),Gameplay->bKeymapImportOpen);
        ACEInputBindings::Revert();Gameplay->RefreshKeyboardOverlays();
        TestEqual(TEXT("Key editor exposes retail mappings plus a controller mapping"),Gameplay->KeyboardRows.Num(),ACEInputBindings::Actions().Num()*4);
        TestTrue(TEXT("Gamepad column has a visible heading"),Gameplay->KeyboardLabels.ContainsByPredicate([](const UTextBlock* T){return T && T->IsVisible() && T->GetText().ToString()==TEXT("Gamepad");}));
        TestEqual(TEXT("Mapping rows use the retail DAT template"),Gameplay->KeyboardEntryElements[0]->ElementId,0x1000002fu);
        TestTrue(TEXT("Load File retains the retail caption"),Gameplay->KeyboardLabels.ContainsByPredicate([](const UTextBlock* T){return T&&T->GetText().ToString()==TEXT("Load File...");}));
        for(int32 K=0;K<4;++K)
        {
            const auto Button=Gameplay->KeyboardEntryElements[0]->Children[K];
            TestEqual(TEXT("Four mapping columns fit the retail row"),Button->X,270+K*75);
            TestEqual(TEXT("Mapping button includes left, middle and right chrome"),Button->Children.Num(),3);
            TestTrue(TEXT("Mapping button has a usable input target"),Gameplay->KeyboardRows[K]->IsVisible());
        }
        Gameplay->KeyboardScrollOffset=999;Gameplay->RefreshKeyboardOverlays();
        TestEqual(TEXT("Native scrollbar clamps to the last mapping row"),Gameplay->KeyboardScrollOffset,Gameplay->KeyboardMaxOffset);
        Gameplay->KeyboardScrollOffset=0;Gameplay->RefreshKeyboardOverlays();
        {
            const auto MappingList=Manager->FindElementUnder(TEXT("MovementPage"),TEXT("KeyboardMappingListBox"));
            const auto Point=Canvas->LayoutToViewport(FVector2D(MappingList->GetScreenOrigin())+FVector2D(20,80));
            TestTrue(TEXT("Mouse wheel recognizes the keyboard window over underlying panels"),Gameplay->ScrollKeyboard(-1,Point));
            TestEqual(TEXT("Wheel advances one retail mapping row"),Gameplay->KeyboardScrollOffset,1);
            Gameplay->KeyboardScrollOffset=0;Gameplay->RefreshKeyboardOverlays();
        }
        {
            auto* Key=CastChecked<UACERetailKeySelector>(Gameplay->KeyboardRows[0]);
            CheckKeyboardCapture(*this,Key);
            const auto Selector=Key->GetCachedWidget();
            const FKeyEvent Activate(EKeys::Enter,FModifierKeysState(),0,false,0,0);
            Selector->OnKeyDown(Key->GetCachedGeometry(),Activate);Selector->OnKeyUp(Key->GetCachedGeometry(),Activate);
            Gameplay->HandleKeyboardNamedClick(TEXT("CombatTab"));
            TestFalse(TEXT("Changing page cancels hidden key capture"),Key->GetIsSelectingKey());
            TestFalse(TEXT("Hidden capture cannot consume gameplay keys"),Key->FilterCaptureKey(Activate,true));
            Gameplay->HandleKeyboardNamedClick(TEXT("MovementTab"));

            ACEInputBindings::Revert();Gameplay->RefreshKeyboardOverlays();
        }
        Gameplay->HandleKeyboardNamedClick(TEXT("KeyboardSaveKeymapAsButton"));
        TestTrue(TEXT("Save As opens the retail file dialog"),Gameplay->bKeymapSave&&Gameplay->bKeymapImportOpen&&Gameplay->KeymapDialog->ElementId==0x1fu);
        CaptureScreen(TEXT("GameplayKeyboardSave"));
        Gameplay->HandleKeyboardNamedClick(TEXT("KeymapFileCancel"));
        ACEInputBindings::Set(EKeys::W,0,FInputChord(EKeys::F10));
        Gameplay->HandleKeyboardNamedClick(TEXT("KeyboardCancelButton"));
        TestEqual(TEXT("Cancel does not change the active movement key"),ACEInputBindings::Get(EKeys::W,0).Key,EKeys::W);
        Gameplay->ToggleKeyboardMappingUI();
        Gameplay->HandleKeyboardNamedClick(TEXT("CharacterSettingsTab"));
        CaptureScreen(TEXT("GameplayKeyboardCharacter"));
        Gameplay->bKeymapSave=false;Gameplay->HandleKeymapImport(CustomPath);
        Gameplay->HandleKeyboardNamedClick(TEXT("KeymapFileOK"));
        TestEqual(TEXT("Review displays the loaded filename"),ACEInputBindings::GetKeymapFileName(),FString(TEXT("CustomFixture.keymap")));
        ACEInputBindings::Set(EKeys::W,0,FInputChord(EKeys::F10));
        Gameplay->HandleKeyboardNamedClick(TEXT("KeyboardOKButton"));
        FConfigFile SavedBindings; SavedBindings.Read(GGameUserSettingsIni);
        FString SavedForward; SavedBindings.GetString(TEXT("ACE.InputBindings"),TEXT("W.0"),SavedForward);
        TestTrue(TEXT("Key mapping is written to disk"),SavedForward.StartsWith(TEXT("F10|")));
        GConfig->SetFile(GGameUserSettingsIni,&SavedBindings);
        ACEInputBindings::Reload();
        TestEqual(TEXT("Saved movement key survives a settings reload"),ACEInputBindings::Get(EKeys::W,0).Key,EKeys::F10);
        TestFalse(TEXT("Closing key editor releases game input"),ACEInputBindings::IsEditing());
        Gameplay->ToggleKeyboardMappingUI();Gameplay->RefreshKeyboardOverlays();
        TestTrue(TEXT("Reopened key editor displays the persisted custom filename"),Gameplay->KeyboardLabels.ContainsByPredicate(
            [](const auto& Label){return Label && Label->GetText().ToString()==TEXT("CustomFixture.keymap");}));
        Gameplay->HandleKeyboardNamedClick(TEXT("KeyboardCancelButton"));
    }
    ACEInputBindings::Reload();
    CheckButtonDragArtwork(*this,Manager);
    if (auto Toolbar=Manager->FindElementByName(TEXT("PanelButton_SkillManagementButton")))
    {
        const auto* Down=Toolbar->ResolvePaintState(true,true,false);
        TestTrue(TEXT("Pressed toolbar icon retains visible artwork"),Down && Down->ImageFileId!=0);
        Toolbar->ResolvePaintState(false,false,false);
    }

    // Item use is exercised through the live binder and real packet construction.
    // Loopback remains isolated from the user's account/server.
    auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FSocket* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("Inventory wire fixture"),false);
    const auto Address=Sockets->CreateInternetAddr(); bool ValidAddress=false;
    Address->SetIp(TEXT("127.0.0.1"),ValidAddress); Address->SetPort(0);
    if (Receiver && Receiver->Bind(*Address))
    {
        Receiver->GetAddress(*Address);
        auto& Session=*Client->Session;
        Session.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("Inventory fixture sender"),false);
        Session.ServerC2SAddr=Address; Session.IssacClient=MakeUnique<FACEIsaac>(123u);
        TFunction<void()> CheckCombatAndXP=[&]
        {
            const auto SavedVitals=Session.PlayerVitals;
            FACEWorldObject Target;Target.Guid=0x70001230;Target.Name=TEXT("Combat target");
            Target.ItemType=ACEItemType::Creature;Target.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
            Session.WorldObjects.Add(Target.Guid,Target);Client->SelectObject(Target.Guid);
            Gameplay->HandleSelectionChanged(Client->GetSelectedObject());
            Gameplay->ApplyCombatMode(int32(ACECombatMode::Melee));
            Gameplay->bCombatAutoRepeat=false;Gameplay->RequestedAttackPower=.5f;
            {
                TGuardValue<uint32> Options(Session.CharacterOptions1,Session.CharacterOptions1 & ~0x1000u);
                const auto SavedSkills=Gameplay->LastVitals.Skills;
                Session.PlayerVitals.Skills.RemoveAll([](const FACESkillInfo& S){return S.SkillId==50;});
                FACESkillInfo Recklessness;Recklessness.SkillId=50;
                const int32 SkillIndex=Session.PlayerVitals.Skills.Add(Recklessness);
                const auto Band=Manager->FindElementByName(TEXT("basic_recklessness_fill"));
                if(TestTrue(TEXT("Retail Recklessness art exists"),Band.IsValid()))
                {
                    Manager->BringFloatyToFront(Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field")));
                    for(float Scale:{.75f,1.f,2.f})
                    {
                        for(int32 Training:{1,2,3})
                        {
                            Session.PlayerVitals.Skills[SkillIndex].AdvancementClass=Training;
                            Gameplay->LastVitals.Skills=Session.PlayerVitals.Skills;
                            Gameplay->RefreshCombatPanelOverlays();
                            CaptureScreen(FString::Printf(TEXT("Recklessness_%d_%g"),Training,Scale),Scale);
                            const auto* Paint=Canvas->PaintStates.Find(Band->InstanceId);
                            if(Training<2)
                            {
                                TestTrue(TEXT("Untrained Recklessness has no visible art"),!Paint||Paint->Visibility==ESlateVisibility::Collapsed);
                                continue;
                            }
                            if(TestNotNull(TEXT("Recklessness has a rendered paint state"),Paint))
                            {
                                TestEqual(TEXT("Only trained and specialized characters see Recklessness"),
                                    Paint->Visibility!=ESlateVisibility::Collapsed,Training>=2);
                                if(Training>=2)
                                {
                                    TestEqual(TEXT("Idle charge does not clip the Recklessness range"),Paint->W,Band->Width);
                                    TestTrue(TEXT("Idle Recklessness uses its DAT texture"),Paint->Texture.IsValid());
                                    const auto Meter=Band->Parent.Pin();
                                    TestTrue(TEXT("Retail range starts at ten percent"),FMath::IsNearlyEqual(float(Band->X)/Meter->Width,.1f,.01f));
                                    TestTrue(TEXT("Retail range ends at ninety percent"),FMath::IsNearlyEqual(float(Band->X+Band->Width)/Meter->Width,.9f,.01f));
                                }
                            }
                        }
                    }
                }
                Session.PlayerVitals=SavedVitals;Gameplay->LastVitals.Skills=SavedSkills;
                Gameplay->RefreshCombatPanelOverlays();CaptureScreen(TEXT("RecklessnessRestored"));
            }
            Session.CachedC2SPackets.Reset();
            Gameplay->BeginCombatPowerCharge(ACEAttackHeight::Medium,true);
            Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-2.;
            Gameplay->TickCombatAutoAttack(0);
            TestEqual(TEXT("Held full-power attack sends no attack action"),Session.CachedC2SPackets.Num(),0);
            Gameplay->ReleaseCombatPowerCharge();Gameplay->TickCombatAutoAttack(0);
            TestEqual(TEXT("Releasing a held attack sends exactly one action"),Session.CachedC2SPackets.Num(),1);
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                TestEqual(TEXT("Melee release uses the retail targeted attack opcode"),R.ReadUInt32(),ACEGameAction::TargetedMeleeAttack);
                TestEqual(TEXT("Melee release retains selected target"),R.ReadUInt32(),uint32(Target.Guid));
                R.ReadUInt32();TestEqual(TEXT("Holding beyond requested mark transmits full power"),R.ReadFloat(),1.f);
            }
            Session.CachedC2SPackets.Reset();Gameplay->CancelCombatAttack();Gameplay->CancelCombatAttack();
            TestEqual(TEXT("Moving cancels a non-repeat attack once"),Session.CachedC2SPackets.Num(),1);
            for(const auto& Packet:Session.CachedC2SPackets)
            {FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);TestEqual(TEXT("Cancellation uses standard retail networking"),R.ReadUInt32(),ACEGameAction::CancelAttack);}
            // Cancellation is a request; the prior attack remains pending until
            // its server response, even when another combat mode is selected.
            {FACEBinaryWriter W;W.WriteUInt32(Session.PlayerGuid);W.WriteUInt32(1);W.WriteUInt32(ACEGameEvent::AttackDone);W.WriteUInt32(0x36);
             FACEBinaryReader R(W.GetData());Session.HandleGameEvent(R);Gameplay->TickCombatAutoAttack(0);}
            Gameplay->ApplyCombatMode(int32(ACECombatMode::Missile));Session.SetCurrentStance(ACEMotion::StanceBowCombat);
            Gameplay->TickCombatAutoAttack(0); // consume the preceding cancellation revision
            Session.CachedC2SPackets.Reset();Gameplay->BeginCombatPowerCharge(ACEAttackHeight::Medium,true);
            Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-2.;Gameplay->TickCombatAutoAttack(0);
            TestEqual(TEXT("Held bow attack also waits for release at full charge"),Session.CachedC2SPackets.Num(),0);
            Gameplay->ReleaseCombatPowerCharge();Gameplay->TickCombatAutoAttack(0);
            TestEqual(TEXT("Releasing a held bow sends one action"),Session.CachedC2SPackets.Num(),1);
            for(const auto& Packet:Session.CachedC2SPackets)
            {FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);TestEqual(TEXT("Bow release uses standard missile networking"),R.ReadUInt32(),ACEGameAction::TargetedMissileAttack);}
            // Fire/abort high, then hold low while the prior attack's delayed
            // commence/cancellation arrives. A server response must not eat
            // the new input, including its eventual release and chosen height.
            auto CombatEvent=[&](uint32 Type,uint32 Error=0)
            {
                FACEBinaryWriter W;W.WriteUInt32(Session.PlayerGuid);W.WriteUInt32(1);W.WriteUInt32(Type);W.WriteUInt32(Error);
                FACEBinaryReader R(W.GetData());Session.HandleGameEvent(R);Gameplay->TickCombatAutoAttack(0);
            };
			{
				TGuardValue<uint32> Options(Session.CharacterOptions1, Session.CharacterOptions1 & ~0x1001u);
				Gameplay->CancelCombatAttack(); CombatEvent(ACEGameEvent::AttackDone);
				Gameplay->ApplyCombatMode(int32(ACECombatMode::Missile));
				Session.SetCurrentStance(ACEMotion::StanceBowCombat);
				const auto CombatRoot = Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"));
				Manager->BringFloatyToFront(CombatRoot);
				for (float Scale : {.75f, 1.f, 1.5f, 2.f})
				{
					CaptureScreen(FString::Printf(TEXT("MissileSlider_%g"), Scale), Scale);
					const auto PowerSlider = Manager->FindElementByName(TEXT("PowerSlider"));
					float ThumbWidth = 0.f;
					for (const auto& Child : PowerSlider->Children)
						if (Child && Child->ElementId == 1) ThumbWidth = Child->Width;
					for (float Accuracy : {0.f, .5f, 1.f})
					{
						Gameplay->RequestedAttackPower = .25f; Gameplay->RefreshCombatPanelOverlays();
						const auto& Geometry = Canvas->GetCachedGeometry();
						auto Point = [&](float Fraction)
						{
							return Geometry.LocalToAbsolute(Canvas->LayoutToViewport(FVector2D(PowerSlider->GetScreenOrigin())
								+ FVector2D(ThumbWidth*.5f + Fraction*(PowerSlider->Width-ThumbWidth), PowerSlider->Height*.5f)));
						};
						const auto From = Point(.25f), To = Point(Accuracy);
						Canvas->NativeOnMouseButtonDown(Geometry,FPointerEvent(0,From,From,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
						TestTrue(TEXT("Native pointer grabs the accuracy thumb"), Gameplay->bCombatPowerDrag);
						Canvas->NativeOnMouseMove(Geometry,FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
						Canvas->NativeOnMouseButtonUp(Geometry,FPointerEvent(0,To,To,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
						TestTrue(TEXT("Pointer drag retains exact requested accuracy at every UI scale"), FMath::IsNearlyEqual(Gameplay->RequestedAttackPower, Accuracy, .001f));
						Session.CachedC2SPackets.Reset();
						NativeClick(TEXT("MediumAttack"));
						Gameplay->CombatPowerBuildStartTime = FPlatformTime::Seconds()-2.; Gameplay->TickCombatAutoAttack(0);
						int32 Attacks = 0;
						for (const auto& Packet : Session.CachedC2SPackets)
						{
							FACEBinaryReader R(Packet.Value.Payload); R.Skip(24);
							if (R.ReadUInt32() != ACEGameAction::TargetedMissileAttack) continue;
							++Attacks;
							TestEqual(TEXT("Native bow attack targets the selected creature"), R.ReadUInt32(), uint32(Target.Guid));
							TestEqual(TEXT("Native bow attack retains aim height"), R.ReadUInt32(), uint32(ACEAttackHeight::Medium));
							TestTrue(TEXT("Native missile packet honors slider accuracy"), FMath::IsNearlyEqual(R.ReadFloat(), Accuracy, .01f));
						}
						TestEqual(TEXT("Clicking attack after a slider drag sends exactly one missile action"), Attacks, 1);
						Gameplay->CancelCombatAttack(); CombatEvent(ACEGameEvent::AttackDone);
					}
				}
				Session.CharacterOptions1 |= 0x1000u;
				Gameplay->BeginCombatPowerCharge(ACEAttackHeight::Medium,true);
				Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-.4;
				Gameplay->TickCombatAutoAttack(0);
				CaptureScreen(TEXT("AdvancedMissileCharge"));
				TestFalse(TEXT("Advanced combat hides the unused normal slider panel"), CombatRoot->bVisible);
				TestEqual(TEXT("Advanced bow displays Accuracy on its floating charge meter"), Gameplay->JumpChargeLabel->GetText().ToString(), FString(TEXT("Accuracy")));
				TestTrue(TEXT("Advanced meter displays held charge"), Manager->FindElementByName(TEXT("Powerbar"))->MeterFillFraction > .3f);
				Gameplay->SetJumpChargeFraction(.6f);
				TestEqual(TEXT("Jump temporarily owns the floating meter"), Gameplay->JumpChargeLabel->GetText().ToString(), FString(TEXT("Height")));
				Gameplay->SetJumpChargeFraction(0);
				TestEqual(TEXT("Finishing jump restores advanced attack display"), Gameplay->JumpChargeLabel->GetText().ToString(), FString(TEXT("Accuracy")));
				Gameplay->CancelCombatAttack(); Gameplay->RefreshCombatPanelOverlays();
				TestFalse(TEXT("Cancelling advanced attack hides its meter"), Manager->FindElementByName(TEXT("RootGameplay_PowerBar_Field"))->bVisible);
				Session.CharacterOptions1 &= ~0x1000u;
				Gameplay->RefreshCombatPanelOverlays();
				TestTrue(TEXT("Normal combat restores slider panel"), CombatRoot->bVisible);
				CaptureScreen(TEXT("MissileSliderRestored"));
			}
            for(int32 Mode:{int32(ACECombatMode::Melee),int32(ACECombatMode::Missile)})
            {
                Gameplay->CancelCombatAttack();CombatEvent(ACEGameEvent::AttackDone);
                Gameplay->ApplyCombatMode(Mode);Gameplay->RequestedAttackPower=.5f;
                Session.SetCurrentStance(Mode==int32(ACECombatMode::Missile)?ACEMotion::StanceBowCombat:ACEMotion::StanceSwordCombat);
                Gameplay->BeginCombatPowerCharge(ACEAttackHeight::High,true);
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-1.;
                Gameplay->ReleaseCombatPowerCharge();Gameplay->TickCombatAutoAttack(0);
                // Press while the first request has no reply. A delayed commence
                // must neither erase this held charge nor send the second attack.
                Gameplay->BeginCombatPowerCharge(ACEAttackHeight::Low,true);
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-.3;
                const double ManualStart=Gameplay->CombatPowerBuildStartTime;
                CombatEvent(ACEGameEvent::CombatCommenceAttack);
                TestEqual(TEXT("Delayed commence preserves held manual charge time"),Gameplay->CombatPowerBuildStartTime,ManualStart);
                Gameplay->ReleaseCombatPowerCharge();Session.CachedC2SPackets.Reset();
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-2.;Gameplay->TickCombatAutoAttack(0);
                TestEqual(TEXT("Released second attack waits for prior server response"),Session.CachedC2SPackets.Num(),0);
                TestTrue(TEXT("Deferred release remains queued"),Gameplay->bCombatAttackRequestPending);
                CombatEvent(ACEGameEvent::AttackDone);
                TestEqual(TEXT("Queued request rebuilds rather than firing on completion"),Session.CachedC2SPackets.Num(),0);
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-.6;Gameplay->TickCombatAutoAttack(0);
                TestEqual(TEXT("Queued request sends exactly once after rebuilding"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                    TestEqual(TEXT("Queued attack preserves mode"),R.ReadUInt32(),Mode==int32(ACECombatMode::Missile)?ACEGameAction::TargetedMissileAttack:ACEGameAction::TargetedMeleeAttack);
                    TestEqual(TEXT("Queued attack preserves target"),R.ReadUInt32(),uint32(Target.Guid));
                    TestEqual(TEXT("Queued attack preserves height"),R.ReadUInt32(),uint32(ACEAttackHeight::Low));
                    TestEqual(TEXT("Queued attack preserves released power"),R.ReadFloat(),.5f);
                }
                Gameplay->CancelCombatAttack();CombatEvent(ACEGameEvent::AttackDone);
            }
            for(bool ReleaseBeforeDone:{false,true})
            {
                Gameplay->CancelCombatAttack();Session.CachedC2SPackets.Reset();
                Gameplay->BeginCombatPowerCharge(ACEAttackHeight::Low,true);Gameplay->bCombatKeyboardHeld=true;
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-.25;
                CombatEvent(ACEGameEvent::CombatCommenceAttack);
                TestTrue(TEXT("Previous commence preserves the queued low attack"),Gameplay->bCombatAttackRequestPending && Gameplay->bCombatAttackHeld);
                if(ReleaseBeforeDone){Gameplay->bCombatKeyboardHeld=false;Gameplay->ReleaseCombatPowerCharge();}
                CombatEvent(ACEGameEvent::AttackDone,0x36); // retail ActionCancelled
                TestTrue(TEXT("Delayed cancellation preserves the next manual request"),Gameplay->bCombatAttackRequestPending && Gameplay->bCombatPowerCharging);
                TestEqual(TEXT("Cancellation preserves the current held state"),Gameplay->bCombatAttackHeld,!ReleaseBeforeDone);
                TestFalse(TEXT("Cancellation still stops automatic repeat"),Gameplay->bCombatRepeatActive);
                TestEqual(TEXT("A queued/held second shot does not fire on cancellation"),Session.CachedC2SPackets.Num(),0);
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-2.;
                if(!ReleaseBeforeDone)
                {
                    Gameplay->TickCombatAutoAttack(0);
                    TestEqual(TEXT("Second shot remains held at full charge"),Session.CachedC2SPackets.Num(),0);
                    Gameplay->bCombatKeyboardHeld=false;Gameplay->ReleaseCombatPowerCharge();
                }
                Gameplay->TickCombatAutoAttack(0);
                TestEqual(TEXT("Second shot fires once without another press"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                    TestEqual(TEXT("Second shot uses retail missile action"),R.ReadUInt32(),ACEGameAction::TargetedMissileAttack);
                    TestEqual(TEXT("Second shot retains target"),R.ReadUInt32(),uint32(Target.Guid));
                    TestEqual(TEXT("Second shot retains low aim"),R.ReadUInt32(),uint32(ACEAttackHeight::Low));
                }
            }
            CombatEvent(ACEGameEvent::AttackDone);
            Gameplay->CancelCombatAttack();
            Gameplay->ApplyCombatMode(int32(ACECombatMode::Melee));
            Gameplay->bCombatAutoRepeat=true; Gameplay->RequestedAttackPower=.5f;
            Gameplay->BeginCombatPowerCharge(ACEAttackHeight::Medium,true);
            Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-2.;
            Gameplay->ReleaseCombatPowerCharge();Gameplay->TickCombatAutoAttack(0);
            Gameplay->TickCombatAutoAttack(0); // consume initial request
            CombatEvent(ACEGameEvent::AttackDone);
            Session.CachedC2SPackets.Reset();
            Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-.25;
            Gameplay->TickCombatAutoAttack(0);
            TestTrue(TEXT("Repeat power correction preserves second-swing refill"),Gameplay->bCombatPowerCharging && Gameplay->CombatPowerOrAccuracy>.2f);
            Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-.75;
            Gameplay->TickCombatAutoAttack(0);
            TestEqual(TEXT("Automatic refill stops at the selected midpoint"),Gameplay->CombatPowerOrAccuracy,.5f);
            TestEqual(TEXT("Displaying repeat charge sends no extra attack"),Session.CachedC2SPackets.Num(),0);
            CombatEvent(ACEGameEvent::CombatCommenceAttack);
            TestEqual(TEXT("Commence holds the requested power visible during swing"),Gameplay->CombatPowerOrAccuracy,.5f);
            Gameplay->CancelCombatAttack();
            Session.CharacterOptions1|=1u;
            Session.SendTargetedMeleeAttack(Target.Guid,ACEAttackHeight::Low,.25f);
            const int32 Sent=Session.CachedC2SPackets.Num();
            Gameplay->ShowPluginAttack(Target.Guid,ACEAttackHeight::Low,.25f);
            TestEqual(TEXT("Plugin attack displays its actual height"),Gameplay->CombatAttackHeight,uint32(ACEAttackHeight::Low));
            TestEqual(TEXT("Plugin attack displays its actual power marker"),Gameplay->RequestedAttackPower,.25f);
            TestEqual(TEXT("Plugin display does not duplicate the network action"),Session.CachedC2SPackets.Num(),Sent);
            CombatEvent(ACEGameEvent::AttackDone);
            Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-.15;
            Gameplay->TickCombatAutoAttack(0);
            TestTrue(TEXT("Plugin melee repeats animate the ordinary power meter"),Gameplay->bCombatPowerCharging && Gameplay->CombatPowerOrAccuracy>.1f);
            Gameplay->CancelCombatAttack();Gameplay->bCombatAutoRepeat=false;
            for (const int32 Mode : {int32(ACECombatMode::Missile),int32(ACECombatMode::Melee)})
            {
                CombatEvent(ACEGameEvent::AttackDone,0x36);
                Gameplay->ApplyCombatMode(Mode);
                Session.SetCurrentStance(Mode==int32(ACECombatMode::Missile)?ACEMotion::StanceBowCombat:ACEMotion::StanceHandCombat);
                Gameplay->TickCombatAutoAttack(0);
                Gameplay->BeginCombatPowerCharge(ACEAttackHeight::Low,true);Gameplay->bCombatKeyboardHeld=true;
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-.75;
                Session.CachedC2SPackets.Reset();
                Gameplay->SetCombatMovementBlocked(true);Gameplay->TickCombatAutoAttack(0);
                CombatEvent(ACEGameEvent::AttackDone,0x36);
                TestTrue(TEXT("Moving preserves held manual attack input"),Gameplay->bCombatAttackHeld && Gameplay->bCombatKeyboardHeld && Gameplay->bCombatAttackRequestPending);
                TestEqual(TEXT("Moving resets charge to zero"),Gameplay->CombatPowerOrAccuracy,0.f);
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-2.;Gameplay->TickCombatAutoAttack(0);
                TestEqual(TEXT("Charge cannot build or fire while moving"),Session.CachedC2SPackets.Num(),0);
                Gameplay->SetCombatMovementBlocked(false);Gameplay->TickCombatAutoAttack(0);
                TestTrue(TEXT("Stopping restarts held charge from zero"),Gameplay->bCombatPowerCharging && Gameplay->CombatPowerOrAccuracy<.05f);
                Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-2.;Gameplay->ReleaseCombatPowerCharge();Gameplay->TickCombatAutoAttack(0);
                TestEqual(TEXT("Held attack fires after movement without a second press"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                    TestEqual(TEXT("Restarted attack uses the standard retail action"),R.ReadUInt32(),
                        Mode==int32(ACECombatMode::Missile)?ACEGameAction::TargetedMissileAttack:ACEGameAction::TargetedMeleeAttack);
                }
                Gameplay->CancelCombatAttack();Gameplay->TickCombatAutoAttack(0);
                Gameplay->BeginCombatPowerCharge(ACEAttackHeight::Low,true);Gameplay->SetCombatMovementBlocked(true);
                Gameplay->ReleaseCombatPowerCharge();Gameplay->SetCombatMovementBlocked(false);
                Session.CachedC2SPackets.Reset();Gameplay->CombatPowerBuildStartTime=FPlatformTime::Seconds()-2.;Gameplay->TickCombatAutoAttack(0);
                TestEqual(TEXT("Releasing while moving does not create a delayed stray shot"),Session.CachedC2SPackets.Num(),0);
                Gameplay->CancelCombatAttack();
            }
            Gameplay->ApplyCombatMode(int32(ACECombatMode::NonCombat));
            Session.PlayerVitals.bValid=true;Session.PlayerVitals.AvailableExperience=1;
            FACESkillInfo Skill;Skill.SkillId=6;Skill.AdvancementClass=2;Skill.XpSpent=0;
            Session.PlayerVitals.Skills={Skill};Gameplay->SelectedSkillId=Skill.SkillId;Gameplay->ActiveSkillTab=TEXT("SkillPage");
            Session.CachedC2SPackets.Reset();Gameplay->RaiseSelectedStat(1);Gameplay->RaiseSelectedStat(10);
            TestEqual(TEXT("Disabled skill raises never spend partial XP or send packets"),Session.CachedC2SPackets.Num(),0);
            Gameplay->ActiveSkillTab=TEXT("AttributePage");Gameplay->SelectedAttributeRow=0;
            Gameplay->RaiseSelectedStat(1);Gameplay->SelectedAttributeRow=6;Gameplay->RaiseSelectedStat(1);
            TestEqual(TEXT("Insufficient XP also blocks attributes and vitals"),Session.CachedC2SPackets.Num(),0);
            Session.PlayerVitals=SavedVitals;Session.WorldObjects.Remove(Target.Guid);Client->SelectObject(Item.Guid);
            Gameplay->HandleSelectionChanged(Client->GetSelectedObject());
        };
        CheckCombatAndXP();
        {
            const auto SavedAppraisal=Gameplay->LastAppraisal;
            Gameplay->LastAppraisal.ObjectGuid=Item.Guid;
            Gameplay->ShowExamination(true);
            Session.CachedC2SPackets.Reset();Gameplay->ExamineSelectedObject();
            TestFalse(TEXT("Examining the same selection again closes the ID window"),Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"))->bVisible);
            TestEqual(TEXT("Closing ID does not send another appraisal"),Session.CachedC2SPackets.Num(),0);
            Gameplay->ExamineSelectedObject();
            TestEqual(TEXT("Examining after closing requests fresh server data"),Session.CachedC2SPackets.Num(),1);
            Gameplay->LastAppraisal=SavedAppraisal;
        }
        Gameplay->CancelPendingUseWith(); Gameplay->ShowExamination(false);
        auto Activate=[&](FACEWorldObject Object)
        {
            Session.bUseBusy=false;
            Gameplay->CancelPendingUseWith();
            Session.WorldObjects.Add(Object.Guid,Object);
            Session.CachedC2SPackets.Reset();
            Gameplay->UseInventoryItem(Object.Guid);
            TArray<uint32> Actions;
            TArray<uint32> Sequences; Session.CachedC2SPackets.GetKeys(Sequences); Sequences.Sort();
            for (uint32 Sequence:Sequences)
            {
                FACEBinaryReader Wire(Session.CachedC2SPackets[Sequence].Payload);
                Wire.Skip(16); // fragment header
                if (Wire.ReadUInt32()==ACEOpcode::GameAction)
                {
                    Wire.ReadUInt32(); // action sequence
                    Actions.Add(Wire.ReadUInt32());
                }
            }
            // Isolated activation cases do not run a server; retire their pending transaction.
            Session.CancelEquipmentSwap();
            return Actions;
        };
        // F / hand merges once, selects the survivor, and preserves its pack.
        auto LastWire=[&](uint32 Expected, TArray<uint32> Values)
        {
            uint32 Last=0;
            for(const auto& Pair:Session.CachedC2SPackets)
            {
                FACEBinaryReader Candidate(Pair.Value.Payload);
                if(!Candidate.CanRead(28)) continue;
                Candidate.Skip(24);
                // Selecting the merge destination can also request appraisal.
                if(Candidate.ReadUInt32()==Expected) Last=FMath::Max(Last,Pair.Key);
            }
            if (!TestTrue(TEXT("Sort emits a reliable action"),Last!=0)) return;
            FACEBinaryReader Wire(Session.CachedC2SPackets[Last].Payload); Wire.Skip(16);
            TestEqual(TEXT("Sort uses GameAction envelope"),Wire.ReadUInt32(),ACEOpcode::GameAction); Wire.ReadUInt32();
            TestEqual(TEXT("Sort emits requested action"),Wire.ReadUInt32(),Expected);
            if(!TestTrue(TEXT("Inventory action contains all expected fields"),Wire.CanRead(Values.Num()*4))) return;
            for(uint32 Value:Values) TestEqual(TEXT("Sort sends exact server quantity/container"),Wire.ReadUInt32(),Value);
        };
        TFunction<void()> VerifyStackCombining = [&]()
        {
        auto RemoveStack = [&](int32 Guid)
        {
            FACEBinaryWriter Removed; Removed.WriteUInt32(uint32(Guid));
            FACEBinaryReader Reader(Removed.GetData()); Session.HandleInventoryRemoveObject(Reader);
        };
        FACEWorldObject Stack; Stack.Guid=9101; Stack.ContainerId=Player.Guid; Stack.WeenieClassId=999901;
        Stack.StackSize=5; Stack.MaxStackSize=100; Stack.PlacementPosition=10;
        FACEWorldObject Match=Stack; Match.Guid=9102; Match.StackSize=10; Match.PlacementPosition=11;
        FACEWorldObject Pack; Pack.Guid=9104; Pack.ContainerId=Player.Guid;
        Pack.ItemType=ACEItemType::Container; Pack.ItemsCapacity=24;
        FACEWorldObject Match2=Stack; Match2.Guid=9103; Match2.StackSize=20; Match2.ContainerId=Pack.Guid;
        for(const auto& O:{Stack,Match,Match2,Pack}) Session.WorldObjects.Add(O.Guid,O);
        Client->SelectObject(Stack.Guid);
        Session.CachedC2SPackets.Reset();
        TestTrue(TEXT("Owned selection begins sorting"),Client->SortInventoryItem(Stack.Guid));
        LastWire(ACEGameAction::StackableMerge,{uint32(Stack.Guid),uint32(Match.Guid),5});
        TestEqual(TEXT("Merge immediately selects surviving stack"),Client->GetSelectedObject().Guid,Match.Guid);
        const int32 PendingCount=Session.CachedC2SPackets.Num();
        Client->SortInventoryItem(Client->GetSelectedObject().Guid); Client->TickInventorySort(.1f);
        TestEqual(TEXT("Repeated F cannot merge the same stale quantities twice"),Session.CachedC2SPackets.Num(),PendingCount);
        RemoveStack(Stack.Guid); Client->TickInventorySort(.1f);
        TestEqual(TEXT("Source-only update cannot advance the merge"),Session.CachedC2SPackets.Num(),PendingCount);
        TestEqual(TEXT("Deleting consumed stack preserves survivor selection"),Client->GetSelectedObject().Guid,Match.Guid);
        Session.WorldObjects[Match.Guid].StackSize=15; Client->TickInventorySort(.1f);
        TestEqual(TEXT("Server confirmation does not automatically start another operation"),Session.CachedC2SPackets.Num(),PendingCount);
        TestEqual(TEXT("Completed merge clears pending source"),Client->SortSourceGuid,0);
        Session.CachedC2SPackets.Reset();
        Client->SortInventoryItem(Client->GetSelectedObject().Guid);
        LastWire(ACEGameAction::StackableMerge,{uint32(Match.Guid),uint32(Match2.Guid),15});
        TestEqual(TEXT("Next F selects the matching stack inside the backpack"),Client->GetSelectedObject().Guid,Match2.Guid);
        Session.WorldObjects[Match2.Guid].StackSize=35;
        const int32 SecondCount=Session.CachedC2SPackets.Num();
        Client->TickInventorySort(.1f);
        TestTrue(TEXT("Destination update alone still waits for source removal"),Client->SortSourceGuid!=0);
        RemoveStack(Match.Guid);
        Client->TickInventorySort(.1f);
        TestEqual(TEXT("Merge in backpack never sends a move to main pack"),Session.CachedC2SPackets.Num(),SecondCount);
        TestEqual(TEXT("Backpack stack stays in backpack"),Session.WorldObjects[Match2.Guid].ContainerId,Pack.Guid);
        Session.CachedC2SPackets.Reset();
        Client->SortInventoryItem(Client->GetSelectedObject().Guid);
        LastWire(ACEGameAction::PutItemInContainer,{uint32(Match2.Guid),uint32(Player.Guid),0});
        TestEqual(TEXT("Moving without a match retains selection"),Client->GetSelectedObject().Guid,Match2.Guid);
        // If no whole-stack merge fits, fill one stack and retain the remainder.
        Session.WorldObjects[Match2.Guid].StackSize=99;
        Session.CachedC2SPackets.Reset();
        Session.WorldObjects.Add(Stack.Guid,Stack); Client->SortInventoryItem(Stack.Guid);
        LastWire(ACEGameAction::StackableMerge,{uint32(Stack.Guid),uint32(Match2.Guid),1});
        TestEqual(TEXT("Partial merge waits for server source count"),Session.WorldObjects[Stack.Guid].StackSize,5);
        TestEqual(TEXT("Partial merge waits for server target count"),Session.WorldObjects[Match2.Guid].StackSize,99);
        Session.WorldObjects[Stack.Guid].StackSize=4;Session.WorldObjects[Match2.Guid].StackSize=100;
        Client->TickInventorySort(.1f);
        TestEqual(TEXT("Partial merge completes without removing the remainder"),Client->SortSourceGuid,0);

        // Prefer a whole-stack merge even if a partially full candidate comes first.
        Session.WorldObjects[Stack.Guid]=Stack;
        Match.StackSize=99;Session.WorldObjects.Add(Match.Guid,Match);
        Session.WorldObjects[Match2.Guid].StackSize=20;
        Session.CachedC2SPackets.Reset();Client->SortInventoryItem(Stack.Guid);
        LastWire(ACEGameAction::StackableMerge,{uint32(Stack.Guid),uint32(Match2.Guid),5});
        Session.CachedC2SPackets.Reset(); Client->TickInventorySort(9.f);
        TestEqual(TEXT("Missing server confirmation times out without more inventory actions"),Session.CachedC2SPackets.Num(),0);
        Session.WorldObjects.Remove(Match.Guid);

        auto StackReply=[&](int32 Guid,int32 Count)
        {
            FACEBinaryWriter W;W.WriteUInt8(1);W.WriteUInt32(Guid);W.WriteUInt32(Count);W.WriteUInt32(Count);
            FACEBinaryReader R(W.GetData());Session.HandleSetStackSize(R);
        };
        auto SentMerge=[&]()
        {
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                if(R.CanRead(4)&&R.ReadUInt32()==ACEGameAction::StackableMerge)return true;
            }
            return false;
        };
        for(bool TargetFirst:{false,true})
        {
            Stack.Name=TEXT("Pyreal");Stack.ItemType=ACEItemType::Money;Stack.WeenieClassId=273;
            Stack.StackSize=15137;Stack.MaxStackSize=25000;
            Match2=Stack;Match2.Guid=9103;Match2.ContainerId=Pack.Guid;Match2.StackSize=15000;
            Session.WorldObjects[Stack.Guid]=Stack;Session.WorldObjects[Match2.Guid]=Match2;
            Session.CachedC2SPackets.Reset();Client->SelectObject(Stack.Guid);Client->SortInventoryItem(Stack.Guid);
            LastWire(ACEGameAction::StackableMerge,{uint32(Stack.Guid),uint32(Match2.Guid),10000});
            const int32 BeforeReply=Session.CachedC2SPackets.Num();
            Client->SortInventoryItem(Client->GetSelectedObject().Guid);
            TestEqual(TEXT("Repeated F does not duplicate a partial merge"),Session.CachedC2SPackets.Num(),BeforeReply);
            StackReply(TargetFirst?Match2.Guid:Stack.Guid,TargetFirst?25000:5137);
            Client->TickInventorySort(.1f);
            TestTrue(TEXT("Partial merge waits for both quantities in either reply order"),Client->SortSourceGuid!=0);
            StackReply(TargetFirst?Stack.Guid:Match2.Guid,TargetFirst?5137:25000);
            Client->TickInventorySort(.1f);
            TestEqual(TEXT("Both stack updates release the merge guard"),Client->SortSourceGuid,0);
            TestEqual(TEXT("Overflow keeps every pyreal"),Session.WorldObjects[Stack.Guid].StackSize+Session.WorldObjects[Match2.Guid].StackSize,30137);
            TestEqual(TEXT("Remainder stays in its original pack"),Session.WorldObjects[Stack.Guid].ContainerId,Player.Guid);
            TestEqual(TEXT("Filled stack stays in its original backpack"),Session.WorldObjects[Match2.Guid].ContainerId,Pack.Guid);
            TestEqual(TEXT("Filled destination remains selected"),Client->GetSelectedObject().Guid,Match2.Guid);
            Session.CachedC2SPackets.Reset();Client->SortInventoryItem(Client->GetSelectedObject().Guid);
            TestFalse(TEXT("F on a full stack never splits it back into the remainder"),SentMerge());
            Session.CachedC2SPackets.Reset();Client->SortInventoryItem(Stack.Guid);
            TestFalse(TEXT("F on the remainder skips the already-full destination"),SentMerge());
        }
        for(int32 Guid:{Stack.Guid,Match.Guid,Match2.Guid,Pack.Guid}) Session.WorldObjects.Remove(Guid);
        Client->SelectObject(0);
        };
        VerifyStackCombining();
        TFunction<void()> VerifyInventoryDropFeedback = [&]()
        {
            const auto SavedPage=Gameplay->ActivePanelPage;
            const int32 SavedPack=Gameplay->SelectedPackGuid;
            const auto SavedSelf=Session.WorldObjects[Player.Guid];
            const FVector2D SavedCursor=FSlateApplication::Get().GetCursorPos();
            ON_SCOPE_EXIT { FSlateApplication::Get().SetCursorPos(SavedCursor); };
            FACEWorldObject Free;Free.Guid=9110;Free.ContainerId=Player.Guid;Free.ItemType=ACEItemType::Container;
            Free.ItemsCapacity=2;Free.PlacementPosition=0;Free.IconId=0x06004CF7;
            FACEWorldObject Full=Free;Full.Guid=9111;Full.ItemsCapacity=1;Full.PlacementPosition=1;
            FACEWorldObject Source;Source.Guid=9112;Source.ContainerId=Player.Guid;Source.ItemType=ACEItemType::Misc;
            Source.StackSize=5;Source.MaxStackSize=100;Source.WeenieClassId=998871;Source.IconId=0x06004CF7;
            FACEWorldObject Target=Source;Target.Guid=9113;Target.ContainerId=Full.Guid;Target.WeenieClassId=998872;
            FACEWorldObject Hook=Free;Hook.Guid=9114;Hook.ContainerId=0;Hook.ItemsCapacity=1;
            Hook.Name=TEXT("Wall hook");Hook.bIsHouseHook=true;Hook.HookType=1;
            Hook.HookItemTypes=ACEItemType::Misc;Hook.HouseOwnerId=Player.Guid;
            Source.HookType=1;
            for(const auto& Item:{Free,Full,Source,Target,Hook}) Session.WorldObjects.Add(Item.Guid,Item);
            Session.WorldObjects[Player.Guid].ItemsCapacity=Client->GetPackItems(Player.Guid).Num()+10;
            Session.WorldObjects[Player.Guid].ContainersCapacity=10;
            Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
            auto Preview=[&](UBorder* Cell,uint32 Expected)
            {
                const auto& Geo=Cell->GetCachedGeometry();
                const FVector2D Absolute=Geo.LocalToAbsolute(Geo.GetLocalSize()*.5);
                FSlateApplication::Get().SetCursorPos(Absolute);
                Gameplay->InvDragGuid=Source.Guid;Gameplay->InvDragIconDid=Source.IconId;
                Gameplay->InvDragAmount=5;Gameplay->bInvDragPending=true;Gameplay->bInvDragActive=true;
                Gameplay->UpdateInventoryDrag(Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute));
                auto* Marker=Gameplay->PaperDollDragTargetIcon.Get();
                if(!TestNotNull(TEXT("Inventory destination has feedback"),Marker)) return;
                TestTrue(TEXT("Inventory target feedback visible"),Marker->IsVisible());
                TestTrue(TEXT("Inventory drop uses exact retail arrow/ring/blocked texture"),
                    Marker->Background.GetResourceObject()==Resources->ResolveIconTexture(Expected));
                TestEqual(TEXT("Inventory marker follows cell position"),Cast<UCanvasPanelSlot>(Marker->Slot)->GetPosition(),Cast<UCanvasPanelSlot>(Cell->Slot)->GetPosition());
                TestEqual(TEXT("Inventory marker follows cell scale"),Cast<UCanvasPanelSlot>(Marker->Slot)->GetSize(),Cast<UCanvasPanelSlot>(Cell->Slot)->GetSize());
            };
            for(float Scale:{1.f,1.5f})
            {
                Gameplay->SelectedPackGuid=Player.Guid;CaptureScreen(TEXT("InventoryDropLayout"),Scale);
                const int32 FreeIndex=Gameplay->PackSlotGuids.IndexOfByKey(Free.Guid);
                const int32 FullIndex=Gameplay->PackSlotGuids.IndexOfByKey(Full.Guid);
                if(!TestTrue(TEXT("Both backpack targets are visible"),FreeIndex>=0 && FullIndex>=0)) break;
                Preview(Gameplay->PackSlotBgs[FreeIndex],0x060011F7);
                CaptureScreen(TEXT("InventoryBackpackArrow"),Scale);
                Preview(Gameplay->PackSlotBgs[FullIndex],0x060011F8);
                CaptureScreen(TEXT("InventoryBackpackBlocked"),Scale);
                Gameplay->CancelInventoryDrag();
                Gameplay->SelectedPackGuid=Free.Guid;CaptureScreen(TEXT("InventoryFreePack"),Scale);
                Preview(Gameplay->InventorySlotBgs[0],0x060011F9);
                CaptureScreen(TEXT("InventorySlotRing"),Scale);
                Gameplay->CancelInventoryDrag();
                Gameplay->SelectedPackGuid=Full.Guid;CaptureScreen(TEXT("InventoryFullPack"),Scale);
                Preview(Gameplay->InventorySlotBgs[0],0x060011F8);
                Session.WorldObjects[Target.Guid].WeenieClassId=Source.WeenieClassId;
                Preview(Gameplay->InventorySlotBgs[0],0x060011F9); // merges need no empty slot
                Session.WorldObjects[Target.Guid].StackSize=100;
                Preview(Gameplay->InventorySlotBgs[0],0x060011F8);
                Session.WorldObjects[Target.Guid]=Target;
                Gameplay->CancelInventoryDrag();
                TestFalse(TEXT("Cancel clears inventory drop feedback"),Gameplay->PaperDollDragTargetIcon->IsVisible());
                Gameplay->ShowExternalContainer(Hook.Guid);CaptureScreen(TEXT("HookDropLayout"),Scale);
                if(TestTrue(TEXT("Empty house hook has a target square"),Gameplay->ExtItemSlotBgs.Num()>1))
                {
                    auto* Cell=Gameplay->ExtItemSlotBgs[1].Get();
                    Preview(Cell,0x060011F9);CaptureScreen(TEXT("HookDropAccept"),Scale);
                    Session.WorldObjects[Source.Guid].HookType=2;
                    Preview(Cell,0x060011F8);CaptureScreen(TEXT("HookDropReject"),Scale);
                    Session.WorldObjects[Source.Guid].HookType=1;
                    Session.WorldObjects[Hook.Guid].HouseOwnerId=0;
                    Preview(Cell,0x060011F8);
                    Session.WorldObjects[Hook.Guid].HouseOwnerId=Player.Guid;
                }
                Gameplay->CancelInventoryDrag();Gameplay->HideExternalContainer(false);
            }
            TestTrue(TEXT("Reordering within a full pack remains valid"),Gameplay->CanMoveInventoryAmountToContainer(Target.Guid,Full.Guid,5));
            TestFalse(TEXT("Splitting within a full pack needs another slot"),Gameplay->CanMoveInventoryAmountToContainer(Target.Guid,Full.Guid,2));
            TestFalse(TEXT("A backpack cannot contain itself"),Gameplay->CanMoveInventoryAmountToContainer(Full.Guid,Full.Guid,1));
            TestFalse(TEXT("A backpack cannot be nested inside a side pack"),Gameplay->CanMoveInventoryAmountToContainer(Full.Guid,Free.Guid,1));
            Session.WorldObjects[Player.Guid]=SavedSelf;
            for(const auto& Item:{Free,Full,Source,Target,Hook}) Session.WorldObjects.Remove(Item.Guid);
            Gameplay->SelectedPackGuid=SavedPack;Gameplay->ShowPanelPage(SavedPage);
            CaptureScreen(TEXT("InventoryDropRestored"));
        };
        VerifyInventoryDropFeedback();
        FACEWorldObject Device; Device.Guid=3345; Device.Name=TEXT("Charged device");
        Device.ContainerId=Player.Guid; Device.ItemType=ACEItemType::Misc;
        Device.MaxStructure=50; Device.Structure=50; Device.ItemUseable=8;
        auto Actions=Activate(Device);
        TestTrue(TEXT("Non-targeted charged device sends Use"),Actions.Contains(ACEGameAction::Use));
        TestEqual(TEXT("Non-targeted charged device does not arm a target cursor"),Gameplay->PendingUseWithSourceGuid,0);
        Device.ItemUseable=0x080008;
        Actions=Activate(Device);
        TestFalse(TEXT("Charges cannot bypass advertised targeted use"),Actions.Contains(ACEGameAction::Use));
        TestEqual(TEXT("Charged targeted item waits for a target"),Gameplay->PendingUseWithSourceGuid,Device.Guid);
        Device.ItemUseable=9; // No | Contained: the No bit takes precedence.
        Actions=Activate(Device);
        TestFalse(TEXT("Explicitly non-usable item sends no Use"),Actions.Contains(ACEGameAction::Use));
        Device.ItemUseable=4;
        Actions=Activate(Device);
        TestFalse(TEXT("Wield-only device cannot be used from its pack"),Actions.Contains(ACEGameAction::Use));
        Device.ItemUseable=0x080004;
        Actions=Activate(Device);
        TestEqual(TEXT("Wield-only targeted device does not arm use from its pack"),Gameplay->PendingUseWithSourceGuid,0);
        Device.ItemUseable=8;
        Device.ObjectDescriptionFlags=ACEObjectDescFlag::WieldOnUse|ACEObjectDescFlag::WieldLeft;
        Device.ValidLocations=ACEEquipMask::MeleeWeapon|ACEEquipMask::Shield;
        Actions=Activate(Device);
        TestTrue(TEXT("WieldOnUse applies even to charged Misc devices"),Actions.Contains(ACEGameAction::GetAndWieldItem));
        TestFalse(TEXT("WieldOnUse takes precedence over Use"),Actions.Contains(ACEGameAction::Use));
        TestTrue(TEXT("WieldLeft advertised flag selects the left equipment branch"),
            ACEInventoryRules::DetermineOwnedUse(Device,Player.Guid)==EACEOwnedItemUse::WieldLeft);
        Device.ObjectDescriptionFlags=0; Device.ValidLocations=0;
        Device.ItemType=ACEItemType::Misc; Device.ItemsCapacity=24;
        Actions=Activate(Device);
        TestEqual(TEXT("Owned container activation opens its contents"),Gameplay->SelectedPackGuid,Device.Guid);
        TestFalse(TEXT("Owned container navigation does not send Use"),Actions.Contains(ACEGameAction::Use));
        Device.ItemsCapacity=0; Device.ItemType=ACEItemType::Clothing;
        Device.ValidLocations=ACEEquipMask::HandWear; Device.ItemUseable=1;
        TestTrue(TEXT("Equipment location wins over non-usable item flag"),
            ACEInventoryRules::DetermineOwnedUse(Device,Player.Guid)==EACEOwnedItemUse::AutoWear);
        Device.ItemType=0x20000000; Device.ValidLocations=0; Device.WeenieClassId=99999;
        TestTrue(TEXT("Salvage tool comes from ItemType, not a hardcoded WCID"),
            ACEInventoryRules::DetermineOwnedUse(Device,Player.Guid)==EACEOwnedItemUse::Salvage);
        TestEqual(TEXT("Source permissions choose least restrictive retail mode"),ACEInventoryRules::LeastLimitedSourceUse(4|8),8u);
        Device.ItemType=ACEItemType::Caster; Device.ContainerId=0; Device.WielderId=Player.Guid;
        Device.CurrentWieldedLocation=ACEEquipMask::Held;
        TestTrue(TEXT("Ordinary use of an equipped item remains PlaceInBackpack"),
            ACEInventoryRules::DetermineOwnedUse(Device,Player.Guid)==EACEOwnedItemUse::Backpack);

        for (int32 I=1; I<=4; ++I) Gameplay->SetFloatyVisible(FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"),I),false);
        Device=FACEWorldObject(); Device.Guid=3345; Device.ContainerId=Player.Guid;
        Device.Name=TEXT("Mana stone"); Device.ItemType=ACEItemType::ManaStone;
        Device.ItemUseable=0x080008; Device.TargetType=ACEItemType::Jewelry|ACEItemType::Armor|ACEItemType::Clothing|ACEItemType::Caster;
        Device.UiEffects=1; Device.IconId=0x060010F9; Device.PlacementPosition=1;
        Session.WorldObjects.Add(Device.Guid,Device); Session.bUseBusy=false;
        Gameplay->SelectedPackGuid=Player.Guid;
        Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field")); Gameplay->RefreshInventoryOverlays();
        CaptureScreen(TEXT("GameplayManaStone"));
        auto NativeItemClick=[&](int32 Guid, bool bDouble)
        {
            const int32 Index=Gameplay->InventorySlotGuids.IndexOfByKey(Guid);
            if (!TestTrue(TEXT("Native item slot is visible"),Gameplay->InventorySlots.IsValidIndex(Index))) return;
            const auto& Geo=Gameplay->InventorySlots[Index]->GetCachedGeometry();
            const FVector2D Abs=Geo.LocalToAbsolute(Geo.GetLocalSize()*.5);
            const auto& CanvasGeo=Canvas->GetCachedGeometry();
            FPointerEvent Down(0,Abs,Abs,TSet<FKey>{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState());
            FPointerEvent Up(0,Abs,Abs,TSet<FKey>{},EKeys::LeftMouseButton,0,FModifierKeysState());
            Canvas->NativeOnMouseButtonDown(CanvasGeo,Down); Canvas->NativeOnMouseButtonUp(CanvasGeo,Up);
            if (bDouble) { Canvas->NativeOnMouseButtonDoubleClick(CanvasGeo,Down); Canvas->NativeOnMouseButtonUp(CanvasGeo,Up); }
        };
        Session.CachedC2SPackets.Reset();
        NativeItemClick(Device.Guid,true);
        TestEqual(TEXT("Actual Slate double-click arms mana-stone targeting"),Gameplay->PendingUseWithSourceGuid,Device.Guid);
        for (const auto& Pair:Session.CachedC2SPackets)
        {
            FACEBinaryReader Wire(Pair.Value.Payload); Wire.Skip(16);
            if (Wire.ReadUInt32()!=ACEOpcode::GameAction) continue;
            Wire.ReadUInt32();
            TestNotEqual(TEXT("Double-click cannot discharge stone before selecting a target"),Wire.ReadUInt32(),ACEGameAction::UseWithTarget);
        }
        NativeItemClick(Item.Guid,false);
        TestEqual(TEXT("Clicking inventory target completes charged mana use"),Gameplay->PendingUseWithSourceGuid,0);
        auto HasAction=[&](uint32 Expected)
        {
            for (const auto& Pair:Session.CachedC2SPackets)
            {
                FACEBinaryReader Wire(Pair.Value.Payload); Wire.Skip(16);
                if (Wire.ReadUInt32()!=ACEOpcode::GameAction) continue;
                Wire.ReadUInt32(); if (Wire.ReadUInt32()==Expected) return true;
            }
            return false;
        };
        TestTrue(TEXT("Target click emits retail UseWithTarget packet"),HasAction(ACEGameAction::UseWithTarget));
        Session.bUseBusy=false; Session.CachedC2SPackets.Reset();
        Device.UiEffects=0; Session.WorldObjects.Add(Device.Guid,Device);
        Gameplay->UseInventoryItem(Device.Guid); Gameplay->TryCompletePendingUseWithTarget(Item.Guid);
        TestEqual(TEXT("Empty stone waits for destruction confirmation"),Gameplay->ManaStoneConfirmTarget,Item.Guid);
        TestFalse(TEXT("Empty stone sends nothing before confirmation"),HasAction(ACEGameAction::UseWithTarget));
        const auto ConfirmationPixels=CaptureScreen(TEXT("GameplayManaStoneConfirmation"));
        int32 ConfirmationInk=0;
        const auto Body=Manager->FindElementByName(TEXT("ManaStoneConfirmationBody"));
        if (!TestTrue(TEXT("Valid mana target creates a confirmation body"),Body.IsValid())) return false;
        const FIntPoint BodyOrigin=Body->GetScreenOrigin();
        for (int32 Y=BodyOrigin.Y; Y<BodyOrigin.Y+Body->Height; ++Y)
            for (int32 X=BodyOrigin.X; X<BodyOrigin.X+Body->Width; ++X)
                if (ConfirmationPixels.IsValidIndex(Y*1600+X))
                {
                    const auto P=ConfirmationPixels[Y*1600+X]; ConfirmationInk+=P.R>200 && P.G>200 && P.B>200;
                }
        TestTrue(TEXT("Destructive confirmation prompt is visibly rendered"),ConfirmationInk>100);
        NativeClick(TEXT("ManaStoneConfirmationNo"));
        TestEqual(TEXT("No dismisses destructive use"),Gameplay->ManaStoneConfirmSource,0);
        TestFalse(TEXT("No does not send UseWithTarget"),HasAction(ACEGameAction::UseWithTarget));
        Gameplay->UseInventoryItem(Device.Guid); Gameplay->TryCompletePendingUseWithTarget(Item.Guid);
        CaptureScreen(TEXT("GameplayManaStoneConfirmationYes")); NativeClick(TEXT("ManaStoneConfirmationYes"));
        TestTrue(TEXT("Yes sends confirmed UseWithTarget"),HasAction(ACEGameAction::UseWithTarget));
        Session.bUseBusy=false; Session.CachedC2SPackets.Reset();
        Item.ObjectDescriptionFlags=ACEObjectDescFlag::Retained; Session.WorldObjects.Add(Item.Guid,Item);
        Gameplay->UseInventoryItem(Device.Guid); Gameplay->TryCompletePendingUseWithTarget(Item.Guid);
        TestEqual(TEXT("Retained item does not open drain confirmation"),Gameplay->ManaStoneConfirmSource,0);
        TestFalse(TEXT("Retained item cannot be drained"),HasAction(ACEGameAction::UseWithTarget));
        Item.ObjectDescriptionFlags=0;
        Item.SpellDID=1; Item.ContainerId=0; Item.WielderId=Player.Guid;
        Item.CurrentWieldedLocation=Item.ValidLocations=ACEEquipMask::Held;
        Session.WorldObjects.Add(Item.Guid,Item);
        Gameplay->ApplyCombatMode(static_cast<int32>(ACECombatMode::Magic));
        Gameplay->RefreshSpellHotbarOverlays(); Gameplay->RefreshInventoryOverlays();
        CaptureScreen(TEXT("GameplayInnateSpell"));
        TestEqual(TEXT("Held caster supplies innate spell"),Gameplay->BuiltInSpellId,1);
        TestEqual(TEXT("Innate spell retains the caster use source"),Gameplay->BuiltInCasterGuid,Item.Guid);
        TestTrue(TEXT("Innate spell paints its caster icon"),Gameplay->BuiltInSpellIconBorders.Num()==3
            && Gameplay->BuiltInSpellIconBorders[1]->GetVisibility()!=ESlateVisibility::Collapsed);
        bool HasEquippedBadge=false;
        for (const auto& Pair:Gameplay->PaperDollShortcutIcons)
            HasEquippedBadge |= Pair.Value && Pair.Value->GetVisibility()!=ESlateVisibility::Collapsed;
        TestTrue(TEXT("Equipped shortcut shows its inventory numeral"),HasEquippedBadge);
        // Scroll the real DAT arrow and cast: scrolling must not change selection.
        Session.SpellBars.SetNum(8); Session.ActiveSpellBar=0;
        auto& Favorites=Session.SpellBars[0]; Favorites.Reset();
        Gameplay->SelectedCombatSpellSlot=0; Gameplay->SpellHotbarScrollOffset=0;
        Gameplay->RefreshSpellHotbarOverlays();
        CaptureScreen(TEXT("GameplayEmptyNumberedSpellBar"));
		TestEqual(TEXT("Empty spell bank selects the equipped innate spell"), Gameplay->SelectedCombatSpellSlot, -1);
		TestTrue(TEXT("Innate selection frame paints above the caster"),
			Gameplay->BuiltInSpellIconBorders[2]->GetVisibility() != ESlateVisibility::Collapsed
			&& Cast<UCanvasPanelSlot>(Gameplay->BuiltInSpellIconBorders[2]->Slot)->GetZOrder()
			> Cast<UCanvasPanelSlot>(Gameplay->BuiltInSpellIconBorders[1]->Slot)->GetZOrder());
		TestTrue(TEXT("Innate spell name includes the casting item"), Gameplay->SpellcastSpellNameLabel->GetText().ToString().Contains(Item.Name));
		FString InnateTooltip;
		const auto& InnateGeometry = Gameplay->BuiltInSpellIconBorders[0]->GetCachedGeometry();
		TestTrue(TEXT("Innate slot exposes its retail tooltip through the canvas"), Gameplay->GetStatTooltipAt(
			InnateGeometry.LocalToAbsolute(InnateGeometry.GetLocalSize()*.5), InnateTooltip));
		TestTrue(TEXT("Innate tooltip includes item, spell and double-click instruction"),
			InnateTooltip.Contains(Item.Name) && InnateTooltip.Contains(TEXT("Strength Other I"))
			&& InnateTooltip.Contains(TEXT("Double-click to cast this spell")));
        for (int32 I=0; I<Gameplay->SpellBarSpellIds.Num(); ++I)
        {
            const int32 ExpectedDid=I<9 ? 0x060010FA+I : 0x06001A97;
            UTexture2D* ExpectedTexture=Resources->ResolveIconTexture(ExpectedDid);
            TestNotNull(TEXT("Empty spell slot artwork resolves from DAT"),ExpectedTexture);
            TestTrue(TEXT("Empty spell bar has blue 1–9 followed by brown tiles"),
                Gameplay->SpellBarSlotBgs[I]->Background.GetResourceObject()==ExpectedTexture);
            TestTrue(TEXT("Empty numbered tiles have no occupied-spell badge"),
                Gameplay->SpellBarSlotNumIcons[I]->GetVisibility()==ESlateVisibility::Collapsed);
        }
        Favorites={1,2,3}; Gameplay->RefreshSpellHotbarOverlays();
        CaptureScreen(TEXT("GameplayNumberedSpellTooltips"));
        FString SpellTooltip;const auto SpellGeometry=Gameplay->SpellBarIcons[0]->GetCachedGeometry();
        TestTrue(TEXT("Numbered spell icon exposes its name through canvas hover"),Gameplay->GetStatTooltipAt(SpellGeometry.LocalToAbsolute(SpellGeometry.GetLocalSize()*.5),SpellTooltip));
        TestTrue(TEXT("Numbered spell tooltip identifies the spell"),SpellTooltip.Contains(TEXT("Strength Other I")));
		Gameplay->StepCombatSpellSelection(1, false, false);
		TestEqual(TEXT("Next selection moves from innate to first favorite"), Gameplay->SelectedCombatSpellSlot, 0);
		Gameplay->StepCombatSpellSelection(-1, false, false);
		TestEqual(TEXT("Previous selection includes the innate slot"), Gameplay->SelectedCombatSpellSlot, -1);
		Session.WorldObjects[Item.Guid].SpellDID = 2;
		Gameplay->SyncSpellTabSelection();
		TestEqual(TEXT("Changing a held wand spell updates selection before the next paint"), Gameplay->BuiltInSpellId, 2);
		Session.WorldObjects[Item.Guid].WielderId = 0;
		Session.WorldObjects[Item.Guid].CurrentWieldedLocation = 0;
		Gameplay->RefreshSpellHotbarOverlays();
		TestEqual(TEXT("Unequipping clears the innate caster"), Gameplay->BuiltInCasterGuid, 0);
		TestEqual(TEXT("Unequipping falls back to the first favorite"), Gameplay->SelectedCombatSpellSlot, 0);
		for (UBorder* Border : Gameplay->BuiltInSpellIconBorders)
			TestTrue(TEXT("Unequipped innate layers are all hidden"), Border->GetVisibility() == ESlateVisibility::Collapsed);
		Session.WorldObjects[Item.Guid] = Item;
		Gameplay->RefreshSpellHotbarOverlays();
		TestEqual(TEXT("Spell artwork reads its formula's power component"), Dat->GetSpellIconPowerLevel(1), 1u);
		for (int32 I = 0; I < 3; ++I)
		{
			FString Name; uint32 IconId = 0; Dat->TryGetSpellInfo(Favorites[I], Name, IconId);
			auto* Raw = Resources->ResolveIconTexture(IconId);
			auto* Composed = Resources->ResolveSpellIcon(Favorites[I]);
			TestTrue(TEXT("Hotbar uses the retail-composed spell icon"), Composed && Composed != Raw && Gameplay->SpellBarIcons[I]->Background.GetResourceObject() == Composed);
			if (Raw && Composed)
			{
				auto& RawMip = Raw->GetPlatformData()->Mips[0]; auto& FinalMip = Composed->GetPlatformData()->Mips[0];
				const auto* RawPixels = static_cast<const FColor*>(RawMip.BulkData.LockReadOnly());
				const auto* FinalPixels = static_cast<const FColor*>(FinalMip.BulkData.LockReadOnly());
				int32 Mask = 0, Changed = 0;
				for (int32 P = 0; P < 1024; ++P) if (RawPixels[P] == FColor::White) { ++Mask; Changed += FinalPixels[P] != FColor::White; }
				RawMip.BulkData.Unlock(); FinalMip.BulkData.Unlock();
				TestTrue(TEXT("The raw spell's white surround is replaced by authored spell artwork"), Mask > 20 && Changed == Mask);
			}
		}
        TestTrue(TEXT("Occupied spell keeps its separate key badge"),
            Gameplay->SpellBarSlotNumIcons[1]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060019EE)
            && Gameplay->SpellBarSlotNumIcons[1]->GetVisibility()!=ESlateVisibility::Collapsed);
        TestTrue(TEXT("First empty tile after three favorites is blue 4"),
            Gameplay->SpellBarSlotBgs[3]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060010FD));
        const auto LeftArrow=Manager->FindElementUnder(TEXT("Spellcasting_Bank1"),TEXT("ScrollBar_Up"));
        const auto RightArrow=Manager->FindElementUnder(TEXT("Spellcasting_Bank1"),TEXT("ScrollBar_Down"));
        TestTrue(TEXT("Short favorite list hides both scroll arrows"),LeftArrow && RightArrow && !LeftArrow->bVisible && !RightArrow->bVisible);
        TestEqual(TEXT("Short favorite list fills the retail viewport"),Gameplay->SpellBarSpellIds.Num(),13);
        TestEqual(TEXT("Insertion cell is empty"),Gameplay->SpellBarSpellIds.Last(),0);
        CaptureScreen(TEXT("GameplayShortSpellBar"));
        auto DropSpellAtEnd=[&](int32 Id)
        {
            Gameplay->SpellDragId=Id; Gameplay->SpellDragSourceBarSlot=INDEX_NONE;
            Gameplay->bSpellDragPending=Gameplay->bSpellDragActive=true;
            const int32 Last=Gameplay->SpellBarSpellIds.Num()-1;
            const auto& G=Gameplay->SpellBarSlotBgs[Last]->GetCachedGeometry();
            Gameplay->TryFinishSpellDrag(Canvas->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*.5)));
        };
        Session.CachedC2SPackets.Reset(); DropSpellAtEnd(4);
        TestTrue(TEXT("Dropping into end cell sends favorite action"),HasAction(ACEGameAction::AddToSpellBar));
        TestEqual(TEXT("Dropping on a blank tile appends without creating gaps"),Favorites.Num(),4);
        TestEqual(TEXT("Filling end cell retains the empty viewport tiles"),Gameplay->SpellBarSpellIds.Num(),13);
        TestEqual(TEXT("Fresh insertion cell follows the new favorite"),Gameplay->SpellBarSpellIds[4],0);
        TestEqual(TEXT("New end cell remains empty"),Gameplay->SpellBarSpellIds.Last(),0);
        Favorites.Reset(); for(int32 I=1;I<=15;++I) Favorites.Add(I);
        Gameplay->RefreshSpellHotbarOverlays(); CaptureScreen(TEXT("GameplayOverflowSpellBar"));
        TestTrue(TEXT("Overflowing favorite list shows scroll arrows"),LeftArrow->bVisible && RightArrow->bVisible);
        const FVector2D ArrowLayout=FVector2D(RightArrow->GetScreenOrigin())+FVector2D(RightArrow->Width,RightArrow->Height)*.5;
        const auto& G=Canvas->GetCachedGeometry(); const FVector2D ArrowAbsolute=G.LocalToAbsolute(Canvas->LayoutToViewport(ArrowLayout));
        Canvas->NativeOnMouseButtonDown(G,FPointerEvent(0,ArrowAbsolute,ArrowAbsolute,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
        Canvas->NativeOnMouseButtonUp(G,FPointerEvent(0,ArrowAbsolute,ArrowAbsolute,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
        TestEqual(TEXT("Right arrow scrolls one entry"),Gameplay->SpellHotbarScrollOffset,1);
        TestEqual(TEXT("Scrolling preserves absolute selected favorite"),Gameplay->SelectedCombatSpellSlot,0);
        FACEWorldObject SpellRecipient; SpellRecipient.Guid=1235; SpellRecipient.bIsPlayer=true; SpellRecipient.ItemType=ACEItemType::Creature;
        Session.WorldObjects.Add(SpellRecipient.Guid,SpellRecipient);
        Session.SelectedObject.bValid=true; Session.SelectedObject.Guid=SpellRecipient.Guid;
        Session.CachedC2SPackets.Reset(); Gameplay->CastSelectedHotbarSpell();
        bool CastOriginal=false;
        for(const auto& P:Session.CachedC2SPackets)
        {
            FACEBinaryReader Wire(P.Value.Payload); Wire.Skip(16);
            if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;
            Wire.ReadUInt32(); const uint32 Action=Wire.ReadUInt32();
            if(Action==ACEGameAction::CastTargetedSpell) { Wire.ReadUInt32(); CastOriginal |= Wire.ReadUInt32()==1; }
            if(Action==ACEGameAction::CastUntargetedSpell) CastOriginal |= Wire.ReadUInt32()==1;
        }
        TestTrue(TEXT("Cast still sends the favorite selected before scrolling"),CastOriginal);
        Session.CachedC2SPackets.Reset(); Gameplay->ActivateHotbarSlot(1);
        TestEqual(TEXT("Key 2 remains the second absolute favorite after scrolling"),Gameplay->SelectedCombatSpellSlot,1);
        Gameplay->SpellHotbarScrollOffset=5; Gameplay->RefreshSpellHotbarOverlays();
        Gameplay->ActivateHotbarSlot(1);
        TestEqual(TEXT("Repeated scrolling never remaps the first ten bindings"),Gameplay->SelectedCombatSpellSlot,1);

        Gameplay->SpellHotbarScrollOffset=3; Gameplay->RefreshSpellHotbarOverlays(); CaptureScreen(TEXT("GameplaySpellBarEnd"));
        TestEqual(TEXT("Scroll reaches the empty end cell"),Gameplay->SpellBarSpellIds.Last(),0);
        DropSpellAtEnd(16);
        TestEqual(TEXT("End drop grows the favorite list"),Session.SpellBars[0].Num(),16);
        if (Session.SpellBars[0].IsValidIndex(15))
            TestEqual(TEXT("End drop appends after hidden earlier favorites"),Session.SpellBars[0][15],16);
        Gameplay->SelectedCombatSpellSlot=-1; Gameplay->RefreshSpellHotbarOverlays();
        Session.CachedC2SPackets.Reset(); Gameplay->CastSelectedHotbarSpell();
        TestTrue(TEXT("Innate selection casts through item UseWithTarget"),HasAction(ACEGameAction::UseWithTarget));
        const auto CombatFrame=Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"));
        if(CombatFrame)
        {
            const int32 OriginalW=CombatFrame->UserResizeW;
            for(int32 Width:{901,902,1200,1201})
            {
                CombatFrame->UserResizeW=Width-625;
                UACEUIElementManager::ApplyFloatyResizeLayout(CombatFrame);
                Gameplay->RefreshSpellHotbarOverlays();CaptureScreen(FString::Printf(TEXT("WideSpellBar%d"),Width));
                TestEqual(TEXT("Spellbar resizes continuously beyond the old 800-pixel cap"),CombatFrame->Width,Width);
                const auto Cast=Manager->FindElementByName(TEXT("CastSpellButton"));
                TestTrue(TEXT("Cast button stays within the widened spellbar"),Cast && Cast->GetScreenOrigin().X+Cast->Width<=CombatFrame->GetScreenOrigin().X+Width);
            }
            CombatFrame->UserResizeW=OriginalW;UACEUIElementManager::ApplyFloatyResizeLayout(CombatFrame);
            Gameplay->RefreshSpellHotbarOverlays();CaptureScreen(TEXT("SpellBarWidthRestored"));
        }


        // The actual stat widgets must consume recomputed server values and retain
        // base/current details when a selected skill is buffed or debuffed.
        TFunction<void()> VerifyStatFooters = [&]()
        {
        auto ClickStatRow=[&](UBorder* Row, float Fraction)
        {
            const auto& RowGeometry=Row->GetCachedGeometry();
            const FVector2D Absolute=RowGeometry.LocalToAbsolute(RowGeometry.GetLocalSize()*FVector2D(Fraction,.5));
            const auto& Geometry=Canvas->GetCachedGeometry();
            Canvas->NativeOnMouseButtonDown(Geometry,FPointerEvent(0,Absolute,Absolute,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
            Canvas->NativeOnMouseButtonUp(Geometry,FPointerEvent(0,Absolute,Absolute,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
        };
        auto CheckStatHighlight=[&](UBorder* Row, bool Selected)
        {
            TestEqual(TEXT("Row brush matches its current selection"),Row->Background.DrawAs.GetValue(),
                Selected?ESlateBrushDrawType::Image:ESlateBrushDrawType::NoDrawType);
            if (Selected) TestTrue(TEXT("Reselection restores actual retail artwork"),
                Row->Background.GetResourceObject()==Resources->ResolveIconTexture(0x06000F93));
        };
        Session.StatResolver = [Dat](auto& V, const auto& E) { Dat->RecomputePlayerStats(V,E); };
        auto& Stats = Session.PlayerVitals;
        Stats.bValid=true; Stats.Coordination=120; Stats.Focus=180;
        Stats.AvailableSkillCredits=10; Stats.AvailableExperience=94384;
        auto CheckDefaultStatFooter = [&](const TCHAR* Page)
        {
            TestEqual(TEXT("Deselected footer shows skill credit label"),Gameplay->AttrFooterLine1Label->GetText().ToString(),FString(TEXT("Skill Credits Available:")));
            TestEqual(TEXT("Deselected footer shows current server skill credits"),Gameplay->AttrFooterLine1Value->GetText().ToString(),FString::FromInt(Stats.AvailableSkillCredits));
            TestTrue(TEXT("Skill credits are visible"),Gameplay->AttrFooterLine1Value->GetVisibility()!=ESlateVisibility::Collapsed);
            TestEqual(TEXT("Deselected footer retains unassigned XP"),Gameplay->AttrFooterLine2Value->GetText().ToString(),FString(TEXT("94,384")));
            for (const TCHAR* Footer : {TEXT("StatManagement_Footer_Default"),TEXT("StatManagement_Footer_Text"),TEXT("StatManagement_Footer_Meter")})
            {
                auto Element=Manager->FindElementUnder(Page,Footer);
                if(TestTrue(TEXT("Authored stat footer exists"),Element.IsValid()))
                    TestEqual(TEXT("Only the default footer is shown without a selection"),Element->bVisible,FString(Footer)==TEXT("StatManagement_Footer_Default"));
            }
        };
        FACESkillInfo Cooking; Cooking.SkillId=39; Cooking.AdvancementClass=2; Cooking.InitLevel=5; Cooking.Ranks=10;
        FACESkillInfo Retired;Retired.SkillId=1;Retired.Name=TEXT("Axe");Retired.AdvancementClass=3;Retired.Ranks=5231;
        Stats.Skills={Cooking,Retired}; Session.NotifyVitalsChanged();
        Gameplay->ShowPanelPage(TEXT("SkillManagementPanel_Field")); Gameplay->SyncSkillPanelTab(TEXT("SkillPage"));
        Gameplay->SelectedSkillId=39;
        FACEActiveEnchantment CookBuff; CookBuff.SpellId=3; CookBuff.SpellCategory=102;
        CookBuff.StatModType=0x8010; CookBuff.StatModKey=39; CookBuff.StatModValue=40;
        Session.ActiveEnchantments={CookBuff}; Session.NotifyVitalsChanged();
        Client->OnVitalsUpdated.Broadcast(Stats); CaptureScreen(TEXT("GameplayCookingBuffed"));
        TestFalse(TEXT("Retired server weapon skill has no selectable UI row"),Gameplay->SkillRowIds.Contains(1));
        TestEqual(TEXT("Selected skill footer shows total and bonus"),Gameplay->AttrFooterTitle->GetText().ToString(),FString(TEXT("Cooking: 155 (+40)")));
        const int32 CookingRow = Gameplay->SkillRowIds.IndexOfByKey(39);
        if (TestTrue(TEXT("Cooking has a rendered skill row"),CookingRow!=INDEX_NONE))
        {
            TestTrue(TEXT("Cooking displays its authored skill icon"), Gameplay->SkillRowIcons[CookingRow]->GetVisibility() != ESlateVisibility::Collapsed);
            auto* Value=Gameplay->SkillRowValues[CookingRow].Get();
            TestEqual(TEXT("Skill tab shows full buffed Cooking value"),Value->GetText().ToString(),FString(TEXT("155")));
            FString ExpectedTooltip;
            TestTrue(TEXT("Cooking has a DAT tooltip"),Dat->TryGetSkillTooltip(39,ExpectedTooltip));
            TestEqual(TEXT("Skill hover shows retail formula and description"),Value->GetToolTipText().ToString(),ExpectedTooltip);
            const FLinearColor BuffColor=Value->GetColorAndOpacity().GetSpecifiedColor();
            TestTrue(TEXT("Selected buffed skill stays green"),BuffColor.G>BuffColor.R);
            auto* Highlight=Gameplay->SkillRowHighlights[CookingRow].Get();
            for (float Fraction : {.04f,.5f,.95f})
            {
                const auto& HoverGeometry=Highlight->GetCachedGeometry();
                const FVector2D HoverPoint=HoverGeometry.LocalToAbsolute(HoverGeometry.GetLocalSize()*FVector2D(Fraction,.5));
                FString HoverText;
                TestTrue(TEXT("Skill icon, name and value all expose the tooltip"),Gameplay->GetStatTooltipAt(HoverPoint,HoverText));
                TestEqual(TEXT("Skill row hover preserves retail text without an extra name header"),HoverText,ExpectedTooltip);
                ClickStatRow(Highlight,Fraction);
                TestEqual(TEXT("Clicking selected skill immediately deselects it"),Gameplay->SelectedSkillId,0);
                CheckStatHighlight(Highlight,false);
                TestTrue(TEXT("Deselecting skill immediately clears footer"),Gameplay->AttrFooterTitle->GetText().ToString().StartsWith(TEXT("Select")));
                CheckDefaultStatFooter(TEXT("SkillPage"));
                CaptureScreen(TEXT("GameplaySkillCredits"));
                Stats.AvailableSkillCredits=0;
                Client->OnVitalsUpdated.Broadcast(Stats);
                CheckDefaultStatFooter(TEXT("SkillPage"));
                TestEqual(TEXT("Stat updates preserve deselection"),Gameplay->SelectedSkillId,0);
                ClickStatRow(Highlight,Fraction);
                TestEqual(TEXT("Clicking skill again immediately selects it"),Gameplay->SelectedSkillId,39);
                CheckStatHighlight(Highlight,true);
                TestEqual(TEXT("Reselection immediately restores skill footer"),Gameplay->AttrFooterTitle->GetText().ToString(),FString(TEXT("Cooking: 155 (+40)")));
            }
            Session.ActiveEnchantments[0].StatModValue=-20; Session.NotifyVitalsChanged(); Client->OnVitalsUpdated.Broadcast(Stats);
            TestEqual(TEXT("Debuff update refreshes the existing row"),Value->GetText().ToString(),FString(TEXT("95")));
            const FLinearColor DebuffColor=Value->GetColorAndOpacity().GetSpecifiedColor();
            TestTrue(TEXT("Selected debuffed skill becomes red"),DebuffColor.R>DebuffColor.G);
        }
        Session.ActiveEnchantments.Reset(); Session.NotifyVitalsChanged(); Client->OnVitalsUpdated.Broadcast(Stats);
        Gameplay->SyncSkillPanelTab(TEXT("AttributePage"));
        FACEActiveEnchantment FocusBuff=CookBuff; FocusBuff.StatModType=0x8001; FocusBuff.StatModKey=5; FocusBuff.StatModValue=60;
        Session.ActiveEnchantments={FocusBuff}; Session.NotifyVitalsChanged(); Client->OnVitalsUpdated.Broadcast(Stats);
        Gameplay->SelectedAttributeRow=4; Gameplay->RefreshAttributeOverlays();
        CaptureScreen(TEXT("GameplayAttributesBuffed"));
        TestEqual(TEXT("Selected attribute footer shows total and bonus"),Gameplay->AttrFooterTitle->GetText().ToString(),FString(TEXT("Focus: 240 (+60)")));
        TestEqual(TEXT("Coordination is the third retail attribute row"),Gameplay->AttributeRows[2]->GetText().ToString(),FString(TEXT("Coordination")));
        TestEqual(TEXT("Attribute tab shows current Focus"),Gameplay->AttributeRowValues[4]->GetText().ToString(),FString(TEXT("240")));
        TestEqual(TEXT("Attribute tab retains raw Focus"),Gameplay->AttributeRowValues[4]->GetToolTipText().ToString(),FString(TEXT("Base: 180\nCurrent: 240")));
        auto* FocusHighlight=Gameplay->AttributeRowHighlights[4].Get();
        for (float Fraction : {.04f,.5f,.95f})
        {
            ClickStatRow(FocusHighlight,Fraction);
            TestEqual(TEXT("Clicking selected attribute deselects it"),Gameplay->SelectedAttributeRow,INDEX_NONE);
            CheckStatHighlight(FocusHighlight,false);
            TestEqual(TEXT("Deselecting attribute immediately clears footer"),Gameplay->AttrFooterTitle->GetText().ToString(),FString(TEXT("Select an Attribute to Improve")));
            CheckDefaultStatFooter(TEXT("AttributePage"));
            Stats.AvailableSkillCredits=10;
            Client->OnVitalsUpdated.Broadcast(Stats);
            CheckDefaultStatFooter(TEXT("AttributePage"));
            CaptureScreen(TEXT("GameplayAttributeCredits"));
            TestEqual(TEXT("Refreshing does not force Strength selection"),Gameplay->SelectedAttributeRow,INDEX_NONE);
            CheckStatHighlight(Gameplay->AttributeRowHighlights[0],false);
            ClickStatRow(FocusHighlight,Fraction);
            TestEqual(TEXT("Attribute reselection survives refresh"),Gameplay->SelectedAttributeRow,4);
            CheckStatHighlight(FocusHighlight,true);
        }
        ClickStatRow(Gameplay->AttributeRowHighlights[2],.5f);
        TestEqual(TEXT("Selecting a different attribute changes the selected row"),Gameplay->SelectedAttributeRow,2);
        CheckStatHighlight(FocusHighlight,false);
        CheckStatHighlight(Gameplay->AttributeRowHighlights[2],true);
        ClickStatRow(FocusHighlight,.5f);
        CheckStatHighlight(FocusHighlight,true);
        CheckStatHighlight(Gameplay->AttributeRowHighlights[2],false);
        Session.ActiveEnchantments.Reset(); Session.NotifyVitalsChanged(); Client->OnVitalsUpdated.Broadcast(Stats);
        TestEqual(TEXT("Attribute expiry refreshes the tab"),Gameplay->AttributeRowValues[4]->GetText().ToString(),FString(TEXT("180")));

        // Vital rows must agree with their footer: depletion is not a debuff,
        // and innate/gear maximums are not spell bonuses.
        const auto SavedStats=Stats;
        Stats.Endurance=100;Stats.Self=100;Stats.HealthStart=10;Stats.HealthRanks=0;
        Stats.StaminaStart=0;Stats.StaminaRanks=0;Stats.ManaStart=0;Stats.ManaRanks=0;
        Stats.Health=40;Stats.Stamina=50;Stats.Mana=60;
        Stats.StatQualityInts.Add(379,10);Stats.StatQualityInts.Add(390,2);
        auto RefreshVital=[&](int32 Row)
        {
            Session.NotifyVitalsChanged();Client->OnVitalsUpdated.Broadcast(Stats);
            Gameplay->SelectedAttributeRow=Row;Gameplay->AttributeScrollOffset=Row==8?1:0;
            Gameplay->RefreshAttributeOverlays();
        };
        RefreshVital(6);
        const auto Neutral=Gameplay->AttributeRowValues[4]->GetColorAndOpacity().GetSpecifiedColor();
        for(int32 Row:{6,7,8})
        {
            RefreshVital(Row);
            TestTrue(TEXT("Unbuffed vital row is neutral even while depleted"),
                Gameplay->AttributeRowValues[Row-Gameplay->AttributeScrollOffset]->GetColorAndOpacity().GetSpecifiedColor().Equals(Neutral));
            TestFalse(TEXT("Unbuffed vital footer has no bonus suffix"),Gameplay->AttrFooterTitle->GetText().ToString().Contains(TEXT(" (+")));
        }
        RefreshVital(6);
        TestEqual(TEXT("Vital footer includes current/max and treats gear/enlightenment as base"),
            Gameplay->AttrFooterTitle->GetText().ToString(),FString(TEXT("Health: 40/74")));
        CaptureScreen(TEXT("GameplayVitalsUnbuffed"));
        for(int32 Row:{6,7,8})
        {
            auto VitalBuff=CookBuff;VitalBuff.StatModType=0x8002;VitalBuff.StatModKey=1+2*(Row-6);
            for(int32 Amount:{10,-10,0})
            {
                VitalBuff.StatModValue=Amount;Session.ActiveEnchantments=Amount?TArray<FACEActiveEnchantment>{VitalBuff}:TArray<FACEActiveEnchantment>{};
                RefreshVital(Row);
                const auto Color=Gameplay->AttributeRowValues[Row-Gameplay->AttributeScrollOffset]->GetColorAndOpacity().GetSpecifiedColor();
                TestTrue(TEXT("Vital color follows actual modifier and returns to neutral on expiry"),
                    Amount>0?Color.G>Color.R:Amount<0?Color.R>Color.G:Color.Equals(Neutral));
                const FString Title=Gameplay->AttrFooterTitle->GetText().ToString();
                TestTrue(TEXT("Vital footer bonus agrees with row color"),Amount?Title.EndsWith(FString::Printf(TEXT(" (%+d)"),Amount)):!Title.Contains(TEXT(" (")));
            }
        }
        Stats=SavedStats;RefreshVital(4);
        FACERetailAugmentationUIFixture::Check(*this, Gameplay, Client, Dat, Stats);

        // The skill footer shows only the XP earned within the current rank.
        int64 SkillStart=0,SkillCost=0;
        Dat->TryGetSkillXpToNextRank(2,0,SkillStart,nullptr,50);
        Dat->TryGetSkillXpToNextRank(2,int32(SkillStart),SkillCost);
        Gameplay->LastVitals.Skills[0].AdvancementClass=2;
        Gameplay->LastVitals.Skills[0].Ranks=50;
        Gameplay->LastVitals.Skills[0].XpSpent=int32(SkillStart+SkillCost/2);
        Gameplay->SelectedSkillId=39;
        Gameplay->SyncSkillPanelTab(TEXT("SkillPage"));Gameplay->RefreshSkillOverlays();
        auto SkillMeter=Manager->FindElementUnder(TEXT("SkillPage"),TEXT("StatManagement_Footer_Meter"));
        if(TestTrue(TEXT("Retail skill XP meter exists"),SkillMeter.IsValid()))
        {
            for(const auto& Child:SkillMeter->Children)
                if(Child && Child->Type==ACEUI::ElementType::Meter){SkillMeter=Child;break;}
            TestTrue(TEXT("Skill footer fills halfway after half a rank of earned XP"),FMath::IsNearlyEqual(SkillMeter->MeterFillFraction,.5f,.001f));
            Gameplay->LastVitals.Skills[0].XpSpent=int32(SkillStart);Gameplay->RefreshSkillOverlays();
            TestEqual(TEXT("Skill footer resets after a point is earned"),SkillMeter->MeterFillFraction,0.f);
        }
        // A capped skill retains a meaningful value in the footer.
        Gameplay->LastVitals.Skills[0].XpSpent=MAX_uint32;
        Gameplay->SelectedSkillId=39;
        Gameplay->SyncSkillPanelTab(TEXT("SkillPage")); Gameplay->RefreshSkillOverlays();
        TestEqual(TEXT("Maximum trained skill reads Infinite"),Gameplay->AttrFooterLine1Value->GetText().ToString(),FString(TEXT("Infinite")));
        };
        VerifyStatFooters();

        // Titles: exercise the real sorted row hit regions and SetTitle wire action.
        FACEBinaryWriter Titles;
        Titles.WriteUInt32(1); Titles.WriteUInt32(1); Titles.WriteUInt32(60);
        for (uint32 Id=60; Id>0; --Id) Titles.WriteUInt32(Id);
        FACEBinaryReader TitlesReader(Titles.GetData()); Session.HandleCharacterTitle(TitlesReader);
        Gameplay->ShowPanelPage(TEXT("SkillManagementPanel_Field"));
        Gameplay->SyncSkillPanelTab(TEXT("CharacterTitlePage"));
        CaptureScreen(TEXT("GameplayCharacterTitles"));
        const auto TitleList=Manager->FindElementUnder(TEXT("CharacterTitlePage"),TEXT("CharacterTitle_ListBox"));
        const auto SetTitleButton=Manager->FindElementUnder(TEXT("CharacterTitlePage"),TEXT("CharacterTitle_SetAsDisplayButton"));
        TestTrue(TEXT("Title list leaves room for current title and footer"), TitleList && SetTitleButton
            && TitleList->Y==90 && SetTitleButton->Y>=TitleList->Y+TitleList->Height);
        TestTrue(TEXT("Title list is sorted by retail title names"),Gameplay->SortedTitleIds.Num()==60
            && FCString::Strcmp(GetCharacterTitleName(Gameplay->SortedTitleIds[0]),GetCharacterTitleName(Gameplay->SortedTitleIds[1]))<=0);
        for (int32 I=0; I<3; ++I)
        {
            auto* TitleRowWidget=Gameplay->TitleRows[I].Get();
            const auto* RowSlot=Cast<UCanvasPanelSlot>(TitleRowWidget->Slot);
            TestTrue(TEXT("Title hit region uses the retail 24-pixel template"), RowSlot && FMath::IsNearlyEqual(RowSlot->GetSize().Y,24*Canvas->GetLastScaleY(),.1));
            TestNotNull(TEXT("Title rows use retail bitmap glyphs"),Cast<UACERetailTextBlock>(TitleRowWidget)->GetBitmapFont());
            const auto& Geo=TitleRowWidget->GetCachedGeometry();
            TestTrue(TEXT("Title row click is handled"),Gameplay->TryHandleOverlayClick(Canvas->GetElementLayer()->GetCachedGeometry().AbsoluteToLocal(Geo.LocalToAbsolute(Geo.GetLocalSize()*.5)),false));
            TestEqual(TEXT("Click selects the displayed title, not server ordering"),Gameplay->SelectedTitleId,Gameplay->SortedTitleIds[I]);
        }
        Gameplay->SelectedTitleId=Gameplay->SortedTitleIds[0];
        if (Gameplay->SelectedTitleId==Session.DisplayTitleId) Gameplay->SelectedTitleId=Gameplay->SortedTitleIds[1];
        Gameplay->RefreshTitleOverlays(); CaptureScreen(TEXT("GameplayCharacterTitleSelected"));
        Session.CachedC2SPackets.Reset(); NativeClick(TEXT("CharacterTitle_SetAsDisplayButton"));
        TestTrue(TEXT("Set title sends the retail network action"),HasAction(ACEGameAction::TitleSet));
        FACEBinaryWriter TitleUpdate; TitleUpdate.WriteUInt32(Gameplay->SelectedTitleId); TitleUpdate.WriteUInt32(1);
        FACEBinaryReader TitleUpdateReader(TitleUpdate.GetData()); Session.HandleUpdateTitle(TitleUpdateReader);
        Gameplay->RefreshTitleOverlays();
        TestEqual(TEXT("Server display-title response refreshes the panel"),Gameplay->TitleCurrentValue->GetText().ToString(),FString(GetCharacterTitleName(Gameplay->SelectedTitleId)));
        TestTrue(TEXT("Current display title cannot be set again"),SetTitleButton->bGhosted);
        Gameplay->ScrollStatList(-1);
        TestTrue(TEXT("Title scrolling advances visible rows"),Gameplay->TitleListScrollOffset>0);

        Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
        Gameplay->RefreshInventoryOverlays();

        FACEWorldObject ShortcutUse;
        ShortcutUse.Guid=98765; ShortcutUse.Name=TEXT("Shortcut use fixture");
        ShortcutUse.ContainerId=Session.PlayerGuid; ShortcutUse.ItemType=ACEItemType::Food;
        ShortcutUse.ItemUseable=8;
        Session.WorldObjects.Add(ShortcutUse.Guid,ShortcutUse); Session.ShortcutObjects[0]=ShortcutUse.Guid;
        Gameplay->RefreshShortcutOverlays(); CaptureScreen(TEXT("GameplayItemShortcuts"));
        Session.bUseBusy=false; Session.CachedC2SPackets.Reset();
        Gameplay->LastInvClickGuid=0;
        NativeClick(TEXT("ShortcutBar_Shortcut1Button"));
        TestEqual(TEXT("Single shortcut click selects the actual item"),Client->GetSelectedObject().Guid,ShortcutUse.Guid);
        TestFalse(TEXT("Single shortcut click only selects"),HasAction(ACEGameAction::Use));
        NativeClick(TEXT("ShortcutBar_Shortcut1Button"));
        TestTrue(TEXT("Double shortcut click uses the item"),HasAction(ACEGameAction::Use));
        Session.bUseBusy=false; Session.CachedC2SPackets.Reset();
        Gameplay->CombatMode=static_cast<int32>(ACECombatMode::NonCombat);
        Gameplay->ActivateHotbarSlot(0);
        TestTrue(TEXT("Number key still uses an item immediately"),HasAction(ACEGameAction::Use));

        const auto Toolbar=Manager->FindElementByName(TEXT("RootGameplay_FloatyToolbar_Field"));
        TestEqual(TEXT("Full retail toolbar is tall enough for both rows"),Toolbar->Height,132);
        FACEWorldObject SecondShortcut=ShortcutUse; SecondShortcut.Guid=98766; SecondShortcut.Name=TEXT("Second-row use fixture");
        Session.WorldObjects.Add(SecondShortcut.Guid,SecondShortcut);
        Session.ShortcutObjects[17]=SecondShortcut.Guid;
        TFunction<void()> CheckShortcutRows = [&]()
        {
        for (float UIScale : {1.f,2.f})
        {
            CaptureScreen(FString::Printf(TEXT("RetailTwoShortcutRows_%d"),int32(UIScale)),UIScale);
            for (int32 Index=0;Index<18;++Index)
            {
                const auto El=Manager->FindElementByName(FString::Printf(TEXT("ShortcutBar%s_Shortcut%dButton"),Index<9?TEXT(""):TEXT("2"),Index%9+1));
                const FIntPoint O=El->GetScreenOrigin();
                const FVector2D Local=Canvas->LayoutToViewport(FVector2D(O)+FVector2D(16,16));
                TestEqual(TEXT("Every displayed shortcut has an independent hit target"),Gameplay->HitTestShortcutSlot(Local),Index);
                TestTrue(TEXT("Both rows lie within toolbar clipping bounds"),O.Y+El->Height<=Toolbar->GetScreenOrigin().Y+Toolbar->Height-5);
                TestTrue(TEXT("Both rows paint icons/backgrounds"),Gameplay->ShortcutIcons[Index]->GetVisibility()!=ESlateVisibility::Collapsed);
                if(Index>=9 && Index<17)
                    TestTrue(TEXT("Every empty second-row shortcut uses retail blue art"),Gameplay->ShortcutSlotBgs[Index]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060074CF));
                Gameplay->InvDragGuid=ShortcutUse.Guid;Gameplay->bInvDragPending=true;Gameplay->bInvDragActive=true;
                Gameplay->UpdateInventoryDrag(Local);
                auto* Marker=Gameplay->PaperDollDragTargetIcon.Get();
                if(TestNotNull(TEXT("Every hotbar destination has drag feedback"),Marker))
                {
                    TestTrue(TEXT("Hotbar drag feedback uses the retail green circle"),Marker->IsVisible()
                        && Marker->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060011F9));
                    TestTrue(TEXT("Hotbar ring shares the toolbar clipping panel"),Marker->GetParent()==Gameplay->ShortcutClipPanel);
                    const auto* RingSlot=Cast<UCanvasPanelSlot>(Marker->Slot);
                    const auto* CellSlot=Cast<UCanvasPanelSlot>(Gameplay->ShortcutSlotBgs[Index]->Slot);
                    TestEqual(TEXT("Hotbar ring follows the scaled destination"),RingSlot->GetPosition(),CellSlot->GetPosition());
                    TestEqual(TEXT("Hotbar ring keeps the scaled icon size"),RingSlot->GetSize(),CellSlot->GetSize());
                }
            }
            CaptureScreen(FString::Printf(TEXT("RetailHotbarDragRing_%d"),int32(UIScale)),UIScale);
            Gameplay->UpdateInventoryDrag(FVector2D(-100,-100));
            TestFalse(TEXT("Leaving hotbar destinations clears the circle"),Gameplay->PaperDollDragTargetIcon->IsVisible());
            Gameplay->CancelInventoryDrag();
            Session.bUseBusy=false; Session.CachedC2SPackets.Reset(); Gameplay->LastInvClickGuid=0;
            NativeClick(TEXT("ShortcutBar2_Shortcut9Button"));
            TestEqual(TEXT("Second row selects its item"),Client->GetSelectedObject().Guid,SecondShortcut.Guid);
            TestFalse(TEXT("Second row single click does not activate"),HasAction(ACEGameAction::Use));
            NativeClick(TEXT("ShortcutBar2_Shortcut9Button"));
            TestTrue(TEXT("Second row double click sends use"),HasAction(ACEGameAction::Use));
        }
        };
        CheckShortcutRows();
        CaptureScreen(TEXT("RetailShortcutDrag"));
        {
            TGuardValue<FString> SettingsPath(GGameUserSettingsIni,FPaths::ProjectSavedDir()/TEXT("Automation/ToolbarResizeFixture.ini"));
            FConfigFile Config; Config.NoSave=false; Config.bCanSaveAllSections=true;
            GConfig->SetFile(GGameUserSettingsIni,&Config);
            const bool Locked=Manager->IsUiLocked(); Manager->SetUiLocked(false);
            const FIntPoint SavedDrag(Toolbar->UserDragX,Toolbar->UserDragY);
            TestEqual(TEXT("Retail toolbar minimum fits one row"),Toolbar->MinHeight,100);
            TestEqual(TEXT("Retail toolbar maximum fits two rows"),Toolbar->MaxHeight,132);
            for (uint32 Did : {UACEMouseCursorWidget::MoveCursorDid,UACEMouseCursorWidget::ResizeVerticalCursorDid,
                UACEMouseCursorWidget::ResizeHorizontalCursorDid,UACEMouseCursorWidget::ResizeNWSECursorDid,
                UACEMouseCursorWidget::ResizeNESWCursorDid})
                TestNotNull(TEXT("Retail window cursor artwork resolves from DAT"),Resources->ResolveIconTexture(Did));
            for (float UIScale : {1.f,2.f})
            {
                Manager->BringFloatyToFront(Toolbar);
                CaptureScreen(TEXT("ToolbarResizeSetup"),UIScale);
                Toolbar->UserDragX+=400-Toolbar->GetScreenOrigin().X;
                Toolbar->UserDragY+=220-Toolbar->GetScreenOrigin().Y; Toolbar->RecomputeLayoutOffset();
                CaptureScreen(TEXT("ToolbarResizeSetup"),UIScale);
                auto GripPoint=[&](const TCHAR* Name)
                {
                    const auto El=Manager->FindElementByName(Name);
                    return Canvas->LayoutToViewport(FVector2D(El->GetScreenOrigin())+FVector2D(El->Width,El->Height)*.5);
                };
                auto Resize=[&](int32 Delta)
                {
                    const FVector2D Start=GripPoint(TEXT("ToolbarBottomBorder"));
                    const FVector2D Size=Canvas->GetCachedGeometry().GetLocalSize();
                    TestTrue(TEXT("Toolbar bottom border advertises vertical resizing"),Manager->GetWindowCursor(Start,Size)==EMouseCursor::ResizeUpDown);
                    Manager->NotifyMouseDown(Start,Size,EKeys::LeftMouseButton);
                    TestTrue(TEXT("Resize cursor survives capture outside the viewport"),Manager->GetWindowCursor(FVector2D(-10,-10),Size)==EMouseCursor::ResizeUpDown);
                    const FVector2D End=Start+FVector2D(0,Delta*Canvas->GetLastScale2D().Y);
                    Manager->NotifyMouseMove(End,Size);
                    Manager->NotifyMouseUp(End,Size,EKeys::LeftMouseButton);
                };
                for (const TCHAR* Corner : {TEXT("ToolbarBottomLeftCorner"),TEXT("ToolbarBottomRightCorner")})
                    TestTrue(TEXT("Toolbar bottom corners also advertise vertical resizing"),Manager->GetWindowCursor(GripPoint(Corner),Canvas->GetCachedGeometry().GetLocalSize())==EMouseCursor::ResizeUpDown);
                const int32 Top=Toolbar->GetScreenOrigin().Y;
                Resize(-32);
                CaptureScreen(FString::Printf(TEXT("RetailOneShortcutRow_%d"),int32(UIScale)),UIScale);
                TestEqual(TEXT("Dragging up hides second shortcut row"),Toolbar->Height,100);
                TestEqual(TEXT("Resizing bottom keeps toolbar top fixed"),Toolbar->GetScreenOrigin().Y,Top);
                for (int32 Index=0;Index<18;++Index)
                {
                    const auto El=Manager->FindElementByName(FString::Printf(TEXT("ShortcutBar%s_Shortcut%dButton"),Index<9?TEXT(""):TEXT("2"),Index%9+1));
                    const FVector2D Point=Canvas->LayoutToViewport(FVector2D(El->GetScreenOrigin())+FVector2D(16,16));
                    TestEqual(TEXT("Collapsed row cannot select items through the world"),Gameplay->HitTestShortcutSlot(Point),Index<9?Index:INDEX_NONE);
                    TestEqual(TEXT("Only the visible row paints shortcut overlays"),Gameplay->ShortcutIcons[Index]->GetVisibility()==ESlateVisibility::Collapsed,Index>=9);
                }
                TestEqual(TEXT("Collapsed border stays at the bottom after reflow"),Manager->FindElementByName(TEXT("ToolbarBottomBorder"))->GetScreenOrigin().Y,Top+95);
                TestEqual(TEXT("Hidden row does not intercept bottom resize grip"),Gameplay->HitTestShortcutSlot(GripPoint(TEXT("ToolbarBottomBorder"))),INDEX_NONE);
                Manager->SetUiLocked(true); Manager->SaveFloatyLayout();
                TestTrue(TEXT("Locked toolbar has no resize cursor"),Manager->GetWindowCursor(GripPoint(TEXT("ToolbarBottomBorder")),Canvas->GetCachedGeometry().GetLocalSize())==EMouseCursor::Default);
                // Simulate a fresh manager's defaults before restoring the saved layout.
                Manager->SetUiLocked(false); GConfig->SetBool(TEXT("ACEClient.DatHUD"),TEXT("UiLocked"),true,GGameUserSettingsIni);
                Toolbar->UserResizeH=0; UACEUIElementManager::ApplyFloatyResizeLayout(Toolbar);
                Manager->LoadFloatyLayout();
                TestTrue(TEXT("Layout reload restores the saved lock state"),Manager->IsUiLocked());
                TestEqual(TEXT("Collapsed toolbar survives layout reload"),Toolbar->Height,100);
                Manager->SetUiLocked(false); CaptureScreen(TEXT("ToolbarRestored"),UIScale);
                Resize(16); CaptureScreen(FString::Printf(TEXT("RetailPartialShortcutRow_%d"),int32(UIScale)),UIScale);
                TestEqual(TEXT("Retail toolbar can reveal part of second row"),Toolbar->Height,116);
                TestTrue(TEXT("Shortcut art clips instead of stretching when partly revealed"),Gameplay->ShortcutClipPanel->GetClipping()==EWidgetClipping::ClipToBoundsAlways);
                TestEqual(TEXT("Partial row keeps the original icon height"),Cast<UCanvasPanelSlot>(Gameplay->ShortcutIcons[17]->Slot)->GetSize().Y,32.*Canvas->GetLastScale2D().Y);
                Resize(80); CaptureScreen(TEXT("ToolbarExpanded"),UIScale);
                TestEqual(TEXT("Toolbar stops at exactly two rows"),Toolbar->Height,132);
                TestEqual(TEXT("Toolbar retains the retail fixed width"),Toolbar->Width,310);
                TestTrue(TEXT("Cursor outside viewport resets after release"),Manager->GetWindowCursor(FVector2D(-10,-10),Canvas->GetCachedGeometry().GetLocalSize())==EMouseCursor::Default);
            }
            Toolbar->UserDragX=SavedDrag.X; Toolbar->UserDragY=SavedDrag.Y; Toolbar->RecomputeLayoutOffset();
            Manager->SetUiLocked(Locked); CaptureScreen(TEXT("RetailShortcutDrag"));
        }
        auto ShortcutPoint=[&](const TCHAR* Name)
        {
            const auto El=Manager->FindElementByName(Name);
            return Canvas->LayoutToViewport(FVector2D(El->GetScreenOrigin())+FVector2D(16,16));
        };
        const FVector2D FirstPoint=ShortcutPoint(TEXT("ShortcutBar_Shortcut1Button"));
        const FVector2D LastPoint=ShortcutPoint(TEXT("ShortcutBar2_Shortcut9Button"));
        TestTrue(TEXT("Shortcut can be dragged from row one"),Gameplay->TryBeginInventoryDrag(FirstPoint));
        Gameplay->UpdateInventoryDrag(LastPoint); Gameplay->TryFinishInventoryDrag(LastPoint);
        TestFalse(TEXT("Dropping a shortcut clears the green circle"),Gameplay->PaperDollDragTargetIcon->IsVisible());
        TestEqual(TEXT("Shortcut drag moves into row two"),Client->GetShortcutObject(17),ShortcutUse.Guid);
        TestEqual(TEXT("Occupied shortcut swaps back to source slot"),Client->GetShortcutObject(0),SecondShortcut.Guid);
        Session.CachedC2SPackets.Reset();
        TestTrue(TEXT("Second-row shortcut can be dragged out"),Gameplay->TryBeginInventoryDrag(LastPoint));
        Gameplay->UpdateInventoryDrag(FVector2D(700,300)); Gameplay->TryFinishInventoryDrag(FVector2D(700,300));
        TestEqual(TEXT("Dragging out removes the shortcut"),Client->GetShortcutObject(17),0);
        TestTrue(TEXT("Removing a shortcut retains the owned item"),Session.WorldObjects.Contains(ShortcutUse.Guid));
        TestTrue(TEXT("Shortcut removal is sent to server"),HasAction(ACEGameAction::RemoveShortCut));
        TestFalse(TEXT("Wheel no longer remaps shortcut bindings"),Gameplay->ScrollShortcutBar(-1));
        Session.ShortcutObjects[17]=0; Session.WorldObjects.Remove(SecondShortcut.Guid);

        // The whole title and all retail Dragbar edges move an unlocked window.
        const bool WasLocked=Manager->IsUiLocked(); Manager->SetUiLocked(false);
        const auto Panel=Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field"));
        const FIntPoint SavedPanelDrag(Panel->UserDragX,Panel->UserDragY);
        for (const TCHAR* Handle : {TEXT("InvTitleText"),TEXT("PanelTopBorder"),TEXT("PanelLeftBorder"),TEXT("PanelRightBorder")})
        {
            Manager->BringFloatyToFront(Panel); CaptureScreen(TEXT("RetailWindowDrag"));
            const auto El=Manager->FindElementByName(Handle);
            const FVector2D Start=Canvas->LayoutToViewport(FVector2D(El->GetScreenOrigin())+FVector2D(El->Width,El->Height)*.5);
            const int32 OldX=Panel->UserDragX,OldY=Panel->UserDragY;
            TestTrue(TEXT("Movable title and borders display the retail move cursor"),Manager->GetWindowCursor(Start,Canvas->GetCachedGeometry().GetLocalSize())==EMouseCursor::CardinalCross);
            Manager->NotifyMouseDown(Start,Canvas->GetCachedGeometry().GetLocalSize(),EKeys::LeftMouseButton);
            TestTrue(TEXT("Move cursor persists while dragging away from the handle"),Manager->GetWindowCursor(FVector2D(1,1),Canvas->GetCachedGeometry().GetLocalSize())==EMouseCursor::CardinalCross);
            Manager->NotifyMouseMove(Start+FVector2D(-20,12),Canvas->GetCachedGeometry().GetLocalSize());
            Manager->NotifyMouseUp(Start+FVector2D(-20,12),Canvas->GetCachedGeometry().GetLocalSize(),EKeys::LeftMouseButton);
            TestEqual(TEXT("Window follows the grabbed title or frame horizontally"),Panel->UserDragX,OldX-20);
            TestEqual(TEXT("Window follows the grabbed title or frame vertically"),Panel->UserDragY,OldY+12);
        }
        Manager->SetUiLocked(true); CaptureScreen(TEXT("RetailWindowLocked"));
        const auto DragTitle=Manager->FindElementByName(TEXT("InvTitleText"));
        const FVector2D TitlePoint=Canvas->LayoutToViewport(FVector2D(DragTitle->GetScreenOrigin())+FVector2D(80,12));
        const int32 LockedX=Panel->UserDragX;
        TestTrue(TEXT("Locked title does not advertise movement"),Manager->GetWindowCursor(TitlePoint,Canvas->GetCachedGeometry().GetLocalSize())==EMouseCursor::Default);
        Manager->NotifyMouseDown(TitlePoint,Canvas->GetCachedGeometry().GetLocalSize(),EKeys::LeftMouseButton);
        Manager->NotifyMouseMove(TitlePoint+FVector2D(50,0),Canvas->GetCachedGeometry().GetLocalSize());
        Manager->NotifyMouseUp(TitlePoint+FVector2D(50,0),Canvas->GetCachedGeometry().GetLocalSize(),EKeys::LeftMouseButton);
        TestEqual(TEXT("UI lock prevents title dragging"),Panel->UserDragX,LockedX);
        Panel->UserDragX=SavedPanelDrag.X; Panel->UserDragY=SavedPanelDrag.Y; Panel->RecomputeLayoutOffset(); Manager->SetUiLocked(WasLocked);

        // A server close while nearby must retire the panel and pending drag.
        Gameplay->ShowExternalContainer(700);
        Gameplay->InvDragSourcePack=700; Gameplay->InvDragGuid=703; Gameplay->bInvDragPending=true;
        Gameplay->HandleExternalContainerClosed(699);
        TestEqual(TEXT("Unrelated container close preserves current loot panel"),Gameplay->OpenLootContainerGuid,700);
        Gameplay->HandleExternalContainerClosed(700);
        TestEqual(TEXT("Server close immediately retires nearby loot panel"),Gameplay->OpenLootContainerGuid,0);
        TestFalse(TEXT("Server close cancels a drag of inaccessible loot"),Gameplay->bInvDragPending);
        Session.bUseBusy=false; Session.ShortcutObjects[0]=0; Session.WorldObjects.Remove(ShortcutUse.Guid);
        Client->SelectObject(0);
        Gameplay->ShowExamination(true);
        Gameplay->HandleEscape();
        TestFalse(TEXT("Escape closes inspection first"),Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"))->bVisible);
        TestEqual(TEXT("Closing inspection retains equipment panel"),Gameplay->ActivePanelPage,FString(TEXT("InventoryPanel_Field")));
        Gameplay->ActiveOptionsTab=TEXT("ConfigPage");
        Gameplay->HandleEscape();
        TestEqual(TEXT("Escape opens options on Game Play regardless of the last tab"),Gameplay->ActiveOptionsTab,FString(TEXT("GameplayOptionsPage")));
        TestEqual(TEXT("Escape switches directly from equipment to the game menu"),Gameplay->ActivePanelPage,FString(TEXT("OptionsPanel_Field")));
        Gameplay->HandleEscape();
        TestTrue(TEXT("Escape toggles the game menu closed"),Gameplay->ActivePanelPage.IsEmpty());
        TFunction<void()> VerifyEscapeSelection = [&]()
        {
            for (int32 Kind=0;Kind<3;++Kind)
            {
                FACEWorldObject Target;Target.Guid=9120;
                Target.Name=TEXT("Escape selection fixture");
                Target.ItemType=Kind==0 ? ACEItemType::Creature : ACEItemType::Misc;
                Target.ContainerId=Kind==2 ? Player.Guid : 0;
                Session.WorldObjects.Add(Target.Guid,Target);
                Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
                Client->SelectObject(Target.Guid);
                // Exercise the shared handler both through Slate and directly,
                // as used by the controller when the viewport owns focus.
                auto PressEscape=[&]()
                {
                    if(Kind==1) Gameplay->HandleEscape();
                    else TestTrue(TEXT("Canvas consumes Escape"),Canvas->NativeOnPreviewKeyDown(Canvas->GetCachedGeometry(),Escape).IsEventHandled());
                };
                PressEscape();
                TestEqual(TEXT("First Escape deselects world and inventory targets"),Client->GetSelectedObject().Guid,0);
                TestEqual(TEXT("Deselect leaves the backpack page intact"),Gameplay->ActivePanelPage,FString(TEXT("InventoryPanel_Field")));
                TestTrue(TEXT("Deselect leaves the backpack window visible"),Panel->bVisible);
                PressEscape();
                TestEqual(TEXT("Second Escape opens Gameplay options"),Gameplay->ActivePanelPage,FString(TEXT("OptionsPanel_Field")));
                TestEqual(TEXT("Escape chooses Gameplay options tab"),Gameplay->ActiveOptionsTab,FString(TEXT("GameplayOptionsPage")));
                PressEscape();
                TestTrue(TEXT("Third Escape closes options"),Gameplay->ActivePanelPage.IsEmpty());
                PressEscape();
                TestEqual(TEXT("Fourth Escape reopens options"),Gameplay->ActivePanelPage,FString(TEXT("OptionsPanel_Field")));
                Session.WorldObjects.Remove(Target.Guid);
            }
        };
        VerifyEscapeSelection();
        Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));

        FACEWorldObject Partner; Partner.Guid=4567; Partner.Name=TEXT("Trade partner"); Session.WorldObjects.Add(Partner.Guid,Partner);
        Session.TradePartnerGuid=Partner.Guid;
        Session.TradeSelfItems={Device.Guid}; Session.TradePartnerItems={Item.Guid};
        Gameplay->ShowTradePanel(Partner.Guid); CaptureScreen(TEXT("GameplayTrade"));
        Gameplay->SelectedPackGuid=Player.Guid;
        Gameplay->HandleTradeStateChanged(ACEGameEvent::AddToTrade);
        const int32 OfferedSlot=Gameplay->InventorySlotGuids.IndexOfByKey(Device.Guid);
        if (TestTrue(TEXT("Offered item remains visible in the backpack"),Gameplay->InventorySlots.IsValidIndex(OfferedSlot)))
        {
            auto* Slot=Gameplay->InventorySlots[OfferedSlot].Get();
            TestTrue(TEXT("Trade event marks the offered inventory icon"),Slot->Background.GetResourceObject()==
                Resources->ResolveItemForeground(Device.IconId,Device.IconOverlayId,Device.UiEffects,true));
            Session.TradeSelfItems.Reset();
            Gameplay->HandleTradeStateChanged(ACEGameEvent::ResetTrade);
            TestTrue(TEXT("Clearing trade removes the offered mark without another inventory update"),Slot->Background.GetResourceObject()==
                Resources->ResolveItemForeground(Device.IconId,Device.IconOverlayId,Device.UiEffects,false));
            Session.TradeSelfItems={Device.Guid};
            Gameplay->HandleTradeStateChanged(ACEGameEvent::AddToTrade);
        }
        TFunction<void()> CheckTradeAndToolbarDrops=[&]()
        {
            const FVector2D SavedCursor=FSlateApplication::Get().GetCursorPos();
            ON_SCOPE_EXIT { FSlateApplication::Get().SetCursorPos(SavedCursor); };
            FACEWorldObject Gift; Gift.Guid=0x790001; Gift.ContainerId=Player.Guid;
            Gift.Name=TEXT("Trade drop fixture"); Gift.ItemType=ACEItemType::Misc; Gift.IconId=0x060010F9;
            Gift.StackSize=1; Gift.MaxStackSize=1; Gift.WeenieClassId=0x790001;
            Session.WorldObjects.Add(Gift.Guid,Gift);
            auto Begin=[&]()
            {
                Gameplay->CancelInventoryDrag(); Gameplay->InvDragGuid=Gift.Guid; Gameplay->InvDragAmount=1;
                Gameplay->InvDragIconDid=Gift.IconId; Gameplay->bInvDragPending=true; Gameplay->bInvDragActive=true;
            };
            auto* Cell=Gameplay->TradeSelfBackgrounds[0].Get();
            const FVector2D TradePoint=Canvas->GetCachedGeometry().AbsoluteToLocal(
                Cell->GetCachedGeometry().LocalToAbsolute(Cell->GetCachedGeometry().GetLocalSize()*.5));
            FSlateApplication::Get().SetCursorPos(Canvas->GetCachedGeometry().LocalToAbsolute(TradePoint));
            Begin(); Gameplay->UpdateInventoryDrag(TradePoint);
            auto* Mark=Gameplay->PaperDollDragTargetIcon.Get();
            TestTrue(TEXT("Trade destination displays the retail green accept ring"),Mark && Mark->IsVisible()
                && Mark->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060011F9));
            CaptureScreen(TEXT("GameplayTradeDropAccept"));
            Session.WorldObjects[Gift.Guid].Attuned=1; Gameplay->UpdateInventoryDrag(TradePoint);
            TestTrue(TEXT("Untradeable item displays the retail rejection symbol"),Mark && Mark->IsVisible()
                && Mark->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060011F8));
            Session.WorldObjects[Gift.Guid].Attuned=0; Gameplay->CancelInventoryDrag();
            TestFalse(TEXT("Cancelling trade drag clears feedback"),Mark && Mark->IsVisible());

            Gameplay->HidePanel();
            const auto Bag=Manager->FindElementByName(TEXT("InventoryButton"));
            const auto Overlay=Manager->FindElementByName(TEXT("BigBackpack_Icon_DragAccept"));
            const FVector2D BagPoint=Canvas->LayoutToViewport(FVector2D(Bag->GetScreenOrigin())+FVector2D(Bag->Width*.5,Bag->Height*.5));
            FSlateApplication::Get().SetCursorPos(Canvas->GetCachedGeometry().LocalToAbsolute(BagPoint));
            Begin(); Gameplay->UpdateInventoryDrag(BagPoint);
            TestTrue(TEXT("Large toolbar backpack displays its retail drop-in state"),bool(Overlay->bVisible) && Overlay->DefaultState==0x10000046);
            Gameplay->SyncInventoryButtonVisual();
            TestTrue(TEXT("HUD refresh preserves active backpack drag feedback"),bool(Overlay->bVisible));
            CaptureScreen(TEXT("GameplayToolbarBackpackDrop"));
            Gameplay->UpdateInventoryDrag(FVector2D(-100,-100));
            TestFalse(TEXT("Leaving toolbar backpack clears drop-in artwork"),bool(Overlay->bVisible));
            Gameplay->UpdateInventoryDrag(BagPoint); Session.CachedC2SPackets.Reset();
            Gameplay->TryFinishInventoryDrag(BagPoint);
            bool MovedToSelf=false;
            for (const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
                if (Wire.ReadUInt32()!=ACEOpcode::GameAction) continue;
                Wire.ReadUInt32();
                if (Wire.ReadUInt32()==ACEGameAction::PutItemInContainer)
                    MovedToSelf=Wire.ReadUInt32()==uint32(Gift.Guid) && Wire.ReadUInt32()==uint32(Player.Guid);
            }
            TestTrue(TEXT("Toolbar backpack drop moves the item into the player's inventory"),MovedToSelf);
            TestFalse(TEXT("Completing backpack drop clears feedback"),bool(Overlay->bVisible));
            Session.WorldObjects.Remove(Gift.Guid); Gameplay->RefreshTradeOverlays();
            Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
        };
        CheckTradeAndToolbarDrops();
        TestEqual(TEXT("Trade paints partner name"),Gameplay->TradeLabels[0]->GetText().ToString(),Partner.Name);
        TestTrue(TEXT("Trade paints item background"),Gameplay->TradeOtherBackgrounds[0]->GetVisibility()!=ESlateVisibility::Collapsed);
        TestTrue(TEXT("Trade paints all default blank slots"),Gameplay->TradeOtherBackgrounds.Last()->GetVisibility()!=ESlateVisibility::Collapsed);
        TestEqual(TEXT("Trade button has its label"),Gameplay->TradeLabels[4]->GetText().ToString(),FString(TEXT("Trade")));
        TestEqual(TEXT("Clear All button has its label"),Gameplay->TradeLabels[5]->GetText().ToString(),FString(TEXT("Clear All")));
		auto ClickOverlay=[&](UWidget* Widget, FKey Button)
		{
			const FVector2D Point=Widget->GetCachedGeometry().GetAbsolutePosition()+Widget->GetCachedGeometry().GetAbsoluteSize()*.5;
			Canvas->NativeOnMouseButtonDown(Canvas->GetCachedGeometry(),FPointerEvent(0,Point,Point,{Button},Button,0,FModifierKeysState()));
			Canvas->NativeOnMouseButtonUp(Canvas->GetCachedGeometry(),FPointerEvent(0,Point,Point,{},Button,0,FModifierKeysState()));
		};
		for (bool Self : {true,false})
		{
			Session.CachedC2SPackets.Reset();
			ClickOverlay(Self ? Gameplay->TradeSelfSlots[0] : Gameplay->TradeOtherSlots[0],EKeys::LeftMouseButton);
			TestEqual(TEXT("Trade item click selects the offered object"),Client->GetSelectedObject().Guid,Self ? Device.Guid : Item.Guid);
			TestTrue(TEXT("Selected trade item has the retail highlight"),(Self ? Gameplay->TradeSelfSelections[0] : Gameplay->TradeOtherSelections[0])->GetVisibility()!=ESlateVisibility::Collapsed);
			ClickOverlay(Self ? Gameplay->TradeSelfSlots[0] : Gameplay->TradeOtherSlots[0],EKeys::RightMouseButton);
			TestTrue(TEXT("Trade item right-click sends IdentifyObject"),HasAction(ACEGameAction::IdentifyObject));
		}
        CheckTradeAcceptance(*this,Client,Manager,
            [&](uint32 Event,uint32 Source){FACEBinaryWriter W;W.WriteUInt32(Player.Guid);W.WriteUInt32(1);W.WriteUInt32(Event);W.WriteUInt32(Source);FACEBinaryReader R(W.GetData());Session.HandleGameEvent(R);Gameplay->HandleTradeStateChanged(Event);},
            NativeClick,[&](){Session.CachedC2SPackets.Reset();},HasAction);
        TestTrue(TEXT("Declining confirmation keeps offers open"),Gameplay->bTradeOpen);
        CaptureScreen(TEXT("GameplayTradePartnerAccepted"));
        Session.TradeSelfItems.Init(Device.Guid,24); Gameplay->RefreshTradeOverlays();
        CaptureScreen(TEXT("GameplayTradeOverflow"));
        const auto TradeBar=Manager->FindElementByName(TEXT("TradeSelf_ItemListScroll"));
        const FVector2D TradeScrollPoint=FVector2D(TradeBar->GetScreenOrigin())+FVector2D(TradeBar->Width-24,8);
        TestTrue(TEXT("Trade overflow scrollbar responds to input"),Gameplay->TryBeginScrollbarDrag(TradeScrollPoint));
        Gameplay->TryFinishScrollbarDrag(); TestTrue(TEXT("Trade scrollbar changes offered-item viewport"),Gameplay->TradeSelfOffset>0);
        Session.CachedC2SPackets.Reset(); NativeClick(TEXT("CloseSecureTradeButton"));
        TestTrue(TEXT("Authored trade close sends CloseTrade"),HasAction(ACEGameAction::CloseTradeNegotiations));
        TestFalse(TEXT("Closing trade hides the panel"),Gameplay->bTradeOpen);
        TestTrue(TEXT("Closing trade also hides cached backgrounds"),Gameplay->TradeOtherBackgrounds[0]->GetVisibility()==ESlateVisibility::Collapsed);
        Device.ItemsCapacity=24; Device.ItemType=ACEItemType::Container;
        Session.WorldObjects.Add(Device.Guid,Device); Session.ShortcutObjects[1]=Device.Guid;
        Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayPackShortcut"));
        TestTrue(TEXT("Side-pack shortcut paints its bound number"),!Gameplay->PackShortcutIcons.IsEmpty()
            && Gameplay->PackShortcutIcons[0]->GetVisibility()!=ESlateVisibility::Collapsed);

        TestTrue(TEXT("Empty side pack hides capacity fill"),!Gameplay->PackCapacityMeters.IsEmpty() && Gameplay->PackCapacityMeters[0]->GetVisibility()==ESlateVisibility::Collapsed);
        FACEWorldObject Occupant; Occupant.Guid=40001; Occupant.Name=TEXT("Packed item"); Occupant.ContainerId=Device.Guid; Occupant.ItemType=ACEItemType::Misc;
        Occupant.PlacementPosition=0; Occupant.IconId=0x060010F9;
        for(int32 I=0;I<12;++I) { Occupant.Guid=40001+I; Occupant.PlacementPosition=I; Session.UpsertWorldObject(Occupant); }
        Gameplay->SelectedPackGuid=Device.Guid;
        Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayPackCapacity"));
        TestTrue(TEXT("Nonempty side pack displays capacity meter"),Gameplay->PackCapacityMeters[0]->GetVisibility()!=ESlateVisibility::Collapsed);
        TestTrue(TEXT("Capacity is occupied item slots over pack capacity"),FMath::IsNearlyEqual(Gameplay->PackCapacityMeters[0]->GetPercent(),.5f));
        TestTrue(TEXT("Open pack uses the authored open-container marker"),Gameplay->PackSlotSelected[1]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x06005D9C));
		Session.CachedC2SPackets.Reset();
		ClickOverlay(Gameplay->PackSlots[1],EKeys::RightMouseButton);
		TestEqual(TEXT("Pack right-click selects the pack for examination"),Client->GetSelectedObject().Guid,Device.Guid);
		TestTrue(TEXT("Pack right-click sends IdentifyObject"),HasAction(ACEGameAction::IdentifyObject));
		FACEAppraisalInfo Inscribed; Inscribed.ObjectGuid=Device.Guid; Inscribed.Name=Device.Name;
		Inscribed.bSuccess=true; Inscribed.BoolProperties.Add(22,true);
		Gameplay->HandleAppraisal(Inscribed); CaptureScreen(TEXT("GameplayInscriptionPrompt"));
		TestTrue(TEXT("Owned pack exposes inscription editing"),Gameplay->CanEditInscription());
		NativeClick(TEXT("ItemInscriptionText"));
		TestEqual(TEXT("Clicking inscription paper opens its editor"),Gameplay->EditingInscriptionGuid,Device.Guid);
		if (Gameplay->ExamInscriptionEditor)
		{
			Session.CachedC2SPackets.Reset();
			const FString Inscription=TEXT("Supplies\nFor the next adventure.");
			Gameplay->ExamInscriptionEditor->SetText(FText::FromString(Inscription));
			Gameplay->ExamInscriptionEditor->OnTextCommitted.Broadcast(FText::FromString(Inscription),ETextCommit::OnUserMovedFocus);
			bool Written=false;
			for (const auto& Pair:Session.CachedC2SPackets)
			{
				FACEBinaryReader Wire(Pair.Value.Payload);Wire.Skip(16);
				if (Wire.ReadUInt32()!=ACEOpcode::GameAction) continue;
				Wire.ReadUInt32();if (Wire.ReadUInt32()!=0xBF) continue;
				Written=true;TestEqual(TEXT("Inscription packet identifies the pack"),Wire.ReadUInt32(),uint32(Device.Guid));
				TestEqual(TEXT("Inscription packet preserves multiline content"),Wire.ReadString16L(),Inscription);
			}
			TestTrue(TEXT("Editing emits the retail writing action"),Written);
			Gameplay->BeginInscriptionEdit();Session.CachedC2SPackets.Reset();
			Gameplay->HandleInscriptionCommitted(FText::FromString(TEXT("discard")),ETextCommit::OnCleared);
			TestFalse(TEXT("Cancelling inscription edits sends nothing"),HasAction(0xBF));
			Gameplay->BeginInscriptionEdit();
			Gameplay->HandleInscriptionCommitted(FText::GetEmpty(),ETextCommit::OnUserMovedFocus);
			TestTrue(TEXT("An inscription can be erased"),Gameplay->LastAppraisal.Inscription.IsEmpty() && HasAction(0xBF));
		}
		Inscribed.StringProperties.Add(8,TEXT("Someone else"));Gameplay->HandleAppraisal(Inscribed);
		TestFalse(TEXT("Another player's inscription stays read-only"),Gameplay->CanEditInscription());
		Gameplay->ShowExamination(false);CaptureScreen(TEXT("GameplayPackAfterID"));
        TestEqual(TEXT("Paperdoll checkbox uses retail Slots label"),Gameplay->PaperdollSlotsCheckboxLabel->GetText().ToString(),FString(TEXT("Slots")));
        Session.PlayerVitals.AetheriaUnlocked=0; Gameplay->RefreshInventoryOverlays();
        for(const auto& SlotName:{TEXT("Inv_SigilOneSlot"),TEXT("Inv_SigilTwoSlot"),TEXT("Inv_SigilThreeSlot")})
        {
            const auto E=Manager->FindElementByName(SlotName);
            TestTrue(TEXT("Aetheria slot resolves"),E.IsValid());
            if(E) TestFalse(TEXT("Locked aetheria slot is hidden"),E->bVisible);
        }
        FACEBinaryWriter Unlock; Unlock.WriteUInt8(1); Unlock.WriteUInt32(322); Unlock.WriteInt32(5);
        FACEBinaryReader UnlockReader(Unlock.GetData()); Session.HandlePrivateUpdatePropertyInt(UnlockReader);
        Gameplay->RefreshInventoryOverlays();
        TestTrue(TEXT("Network bitfield unlocks blue and red only"),Manager->FindElementByName(TEXT("Inv_SigilOneSlot"))->bVisible
            && !Manager->FindElementByName(TEXT("Inv_SigilTwoSlot"))->bVisible && Manager->FindElementByName(TEXT("Inv_SigilThreeSlot"))->bVisible);
        for(int32 I=0;I<12;++I) Session.WorldObjects.Remove(40001+I);
        Gameplay->SelectedPackGuid=Player.Guid;
        Gameplay->RefreshInventoryOverlays();
        Gameplay->SetJumpChargeFraction(.25f); CaptureScreen(TEXT("GameplayJumpCharge"));
        const auto JumpRoot=Manager->FindElementByName(TEXT("RootGameplay_PowerBar_Field"));
        const auto JumpMeter=Manager->FindElementUnder(TEXT("RootGameplay_PowerBar_Field"),TEXT("Powerbar"));
        TestTrue(TEXT("Jump uses the authored PBM_JUMP display state"),JumpRoot && JumpRoot->PaintState==0x10000042);
        TestTrue(TEXT("Jump meter tracks charge"),JumpMeter && FMath::IsNearlyEqual(JumpMeter->MeterFillFraction,.25f));
        TestTrue(TEXT("Jump bar honors the DAT maximum width"),JumpRoot && JumpRoot->Width<=810);
        TestTrue(TEXT("Jump bar includes the Height label"),Gameplay->JumpChargeLabel && Gameplay->JumpChargeLabel->GetText().ToString()==TEXT("Height"));
        Gameplay->SetJumpChargeFraction(0);
        ScreenSize=FIntPoint(2560,1354); CaptureScreen(TEXT("GameplayLargeInventory"));
        auto DragWidgetToShortcut = [&](UWidget* Source, int32 Guid, int32 Destination)
        {
            const auto Target = Manager->FindElementByName(FString::Printf(TEXT("ShortcutBar_Shortcut%dButton"),Destination+1));
            const FVector2D From=Source->GetCachedGeometry().GetAbsolutePosition()+Source->GetCachedGeometry().GetAbsoluteSize()*0.5;
            const FVector2D To=Canvas->GetCachedGeometry().LocalToAbsolute(
                (FVector2D(Target->GetScreenOrigin())+FVector2D(16,16))*Canvas->GetLastScale2D());
            Session.CachedC2SPackets.Reset();
            Canvas->NativeOnMouseButtonDown(Canvas->GetCachedGeometry(),FPointerEvent(0,From,From,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
            TestEqual(TEXT("Native backpack/item press arms the correct drag"),Gameplay->InvDragGuid,Guid);
            Canvas->NativeOnMouseMove(Canvas->GetCachedGeometry(),FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
            TestTrue(TEXT("Native backpack/item motion becomes a drag"),Gameplay->bInvDragActive);
            CaptureScreen(TEXT("GameplayDraggingItem")); // Tick/layout may refresh between press and release.
            Canvas->NativeOnMouseButtonUp(Canvas->GetCachedGeometry(),FPointerEvent(0,To,To,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
            TestTrue(TEXT("Native backpack/item drop sends AddShortCut"),HasAction(ACEGameAction::AddShortCut));
            TestEqual(TEXT("Native backpack/item drop immediately updates binding"),Session.GetShortcutObject(Destination),Guid);
        };
        if (Gameplay->PackSlots.Num()>1) DragWidgetToShortcut(Gameplay->PackSlots[1],Device.Guid,4);
        TestEqual(TEXT("Moving backpack shortcut removes the old binding"),Session.GetShortcutObject(1),0);
        Item.WielderId=Item.ParentGuid=0; Item.ContainerId=Device.Guid; Item.CurrentWieldedLocation=0; Item.PlacementPosition=0;
        Session.WorldObjects.Add(Item.Guid,Item); Gameplay->SelectedPackGuid=Device.Guid;
        Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayBackpackItemDrag"));
        if (!Gameplay->InventorySlots.IsEmpty()) DragWidgetToShortcut(Gameplay->InventorySlots[0],Item.Guid,5);
        Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayBoundShortcut"));
        TestTrue(TEXT("Occupied hotbar slot paints the same numeral as inventory"),Gameplay->ShortcutNumberIcons.Num()>5
            && Gameplay->ShortcutNumberIcons[5]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060010A3));
        TestEqual(TEXT("Moving item shortcut removes the previous inventory badge"),Session.GetShortcutObject(2),0);
        TestTrue(TEXT("Inventory badge follows item to sixth slot"),Gameplay->InventorySlotOverlays[0]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060010A3));
        if (Gameplay->PackSlots.Num()>1) DragWidgetToShortcut(Gameplay->PackSlots[1],Device.Guid,5);
        TestEqual(TEXT("Occupied shortcut rehomes the displaced item to the right"),Session.GetShortcutObject(6),Item.Guid);
        TestEqual(TEXT("Occupied shortcut removes backpack's prior binding"),Session.GetShortcutObject(4),0);
        CaptureScreen(TEXT("GameplayMainPackDrag"));
        DragWidgetToShortcut(Gameplay->PackSlots[0],Player.Guid,7);
        TestTrue(TEXT("Main backpack also carries its shortcut numeral"),Gameplay->MainPackShortcutIcon
            && Gameplay->MainPackShortcutIcon->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060010A5));
        TestTrue(TEXT("Main backpack shortcut uses bag art instead of the player's creature icon"),
            Gameplay->ShortcutIcons[7]->Background.GetResourceObject()==Resources->ResolveItemForeground(0x06004CF7,0,0));
        Gameplay->SelectedPackGuid=Device.Guid; Session.CachedC2SPackets.Reset(); Gameplay->UseShortcutSlot(8);
        TestEqual(TEXT("Main backpack shortcut opens the player's inventory"),Gameplay->SelectedPackGuid,Player.Guid);
        TestFalse(TEXT("Main backpack shortcut never sends Use on the player"),HasAction(ACEGameAction::Use));
        // gmToolbarUI::UseShortcut: using a kit and acquiring self are not
        // selection changes. Exercise keyboard activation and native pointer
        // press/release on the backpack (the latter also arms inventory drags).
        {
            const auto SavedSelection=Session.SelectedObject;
            const auto SavedShortcuts=Session.ShortcutObjects;
            const int32 SavedCombatMode=Gameplay->CombatMode;
            FACEWorldObject Kit; Kit.Guid=98801; Kit.Name=TEXT("Healing Kit");
            Kit.ContainerId=Player.Guid; Kit.ItemType=ACEItemType::Misc;
            Kit.ItemUseable=0x220008; Kit.TargetType=ACEItemType::Creature; // Contained source, remote or self target.
            FACEWorldObject Monster; Monster.Guid=98802; Monster.Name=TEXT("Combat target");
            Monster.ItemType=ACEItemType::Creature; Monster.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
            Monster.Position=Player.Position;
            Session.WorldObjects.Add(Kit.Guid,Kit); Session.WorldObjects.Add(Monster.Guid,Monster);
            Session.ShortcutObjects[8]=Kit.Guid; Session.ShortcutObjects[7]=Player.Guid;
            for (int32 Mode : {int32(ACECombatMode::Melee),int32(ACECombatMode::Magic)})
            for (int32 Input=0; Input<3; ++Input)
            {
                Gameplay->CombatMode=Mode; Session.bUseBusy=false;
                Gameplay->CancelPendingUseWith(); Session.CachedC2SPackets.Reset();
                Gameplay->LastInvClickGuid=0; Gameplay->LastInvClickTime=0;
                Session.SelectedObject.bValid=true; Session.SelectedObject.Guid=Monster.Guid;
                const auto Serial=Session.SelectedObject.SelectionSerial;
                if (Input==0) Gameplay->UseShortcutSlot(9);
                else if (Input==1)
                {
                    NativeClick(TEXT("ShortcutBar_Shortcut9Button"));
                    TestEqual(TEXT("First kit click keeps the monster selected"),Client->GetSelectedObject().Guid,Monster.Guid);
                    NativeClick(TEXT("ShortcutBar_Shortcut9Button"));
                }
                else
                {
                    Gameplay->HandleNamedClick(TEXT("ShortcutBar_Shortcut9Button"));
                    Gameplay->HandleNamedClick(TEXT("ShortcutBar_Shortcut9Button"));
                }
                TestEqual(TEXT("Kit activation retains combat target"),Client->GetSelectedObject().Guid,Monster.Guid);
                TestEqual(TEXT("Kit arms targeted use"),Gameplay->PendingUseWithSourceGuid,Kit.Guid);
                if (Input==0) Gameplay->UseShortcutSlot(8);
                else if (Input==1) NativeClick(TEXT("ShortcutBar_Shortcut8Button"));
                else Gameplay->HandleNamedClick(TEXT("ShortcutBar_Shortcut8Button"));
                TestEqual(TEXT("Self healing retains combat target"),Client->GetSelectedObject().Guid,Monster.Guid);
                TestEqual(TEXT("No transient selection change during self healing"),Session.SelectedObject.SelectionSerial,Serial);
                TestEqual(TEXT("Self healing does not change stance"),Gameplay->CombatMode,Mode);
                TestEqual(TEXT("Self healing retires use cursor"),Gameplay->PendingUseWithSourceGuid,0);
                TestTrue(TEXT("Self healing emits UseWithTarget"),HasAction(ACEGameAction::UseWithTarget));
                TestFalse(TEXT("Self healing never sends Use on the backpack/player"),HasAction(ACEGameAction::Use));
                LastWire(ACEGameAction::UseWithTarget,{uint32(Kit.Guid),uint32(Player.Guid)});
            }
            Session.bUseBusy=false; Session.ShortcutObjects=SavedShortcuts;
            Session.WorldObjects.Remove(Kit.Guid); Session.WorldObjects.Remove(Monster.Guid);
            Session.SelectedObject=SavedSelection; Gameplay->CombatMode=SavedCombatMode;
        }
        // Native inventory drags and server acknowledgements exercise the cached UI.
        const auto SavedObjects=Session.WorldObjects;
        const auto SavedContents=Session.ContainerContents;
        Session.WorldObjects.Reset(); Session.ContainerContents.Reset(); Session.WorldObjects.Add(Player.Guid,Player);
        for(int32 I=0;I<3;++I)
        {
            FACEWorldObject O; O.Guid=60001+I; O.ContainerId=Player.Guid; O.PlacementPosition=I;
            O.ItemType=ACEItemType::Misc; O.IconId=0x060010F9; O.Name=FString::Printf(TEXT("Item %d"),I+1); Session.UpsertWorldObject(O);
            O.Guid=61001+I; O.ItemType=ACEItemType::Container; O.ItemsCapacity=24;
            O.IconId=0x06004CF7; O.Name=FString::Printf(TEXT("Pack %d"),I+1); Session.UpsertWorldObject(O);
        }
        Gameplay->SelectedPackGuid=Player.Guid; Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayReorderBefore"));
        TestTrue(TEXT("Main backpack capacity excludes side packs"),FMath::IsNearlyEqual(Gameplay->MainPackCapacityMeter->GetPercent(),3.f/102.f));
        TestTrue(TEXT("Main pack uses the container background rather than the player's item type"),
            Gameplay->PackSlotBgs[0]->Background.GetResourceObject()==Resources->ResolveItemBackground(ACEItemType::Container,0));
        int32 EmptyPacks=0;
        for (int32 I=1;I<Gameplay->PackSlots.Num();++I)
        {
            if (Gameplay->PackSlots[I]->GetVisibility()==ESlateVisibility::Collapsed) continue;
            if (Gameplay->PackSlotGuids[I])
            {
                TestTrue(TEXT("Occupied pack paints its retail container background"),
                    Gameplay->PackSlotBgs[I]->GetVisibility()!=ESlateVisibility::Collapsed
                    && Gameplay->PackSlotBgs[I]->Background.GetResourceObject()==Resources->ResolveItemBackground(ACEItemType::Container,0));
                continue;
            }
            ++EmptyPacks;
            // Independent DAT: 21000037 / ItemSlot_Backpack / ItemSlot_Icon / ItemSlot_Empty.
            TestTrue(TEXT("Empty pack uses the authored 06000F6E image at full opacity"),
                Gameplay->PackSlots[I]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x06000F6E)
                && Gameplay->PackSlots[I]->GetRenderOpacity()==1.f);
            TestTrue(TEXT("Empty pack cannot retain a previous occupant's background"),
                Gameplay->PackSlotBgs[I]->GetVisibility()==ESlateVisibility::Collapsed);
        }
        TestEqual(TEXT("Three packs leave four visible placeholders in the seven default slots"),EmptyPacks,4);
        {
            const FACEWorldObject Original=Session.WorldObjects[61003];
            FACEWorldObject Modified=Original; Modified.ItemType=ACEItemType::Misc; Modified.IconUnderlayId=0x060010F9;
            Session.UpsertWorldObject(Modified); Gameplay->RefreshInventoryOverlays();
            TestTrue(TEXT("Pack type and custom underlay changes invalidate the cached background"),
                Gameplay->PackSlotBgs[3]->Background.GetResourceObject()==Resources->ResolveItemBackground(Modified.ItemType,Modified.IconUnderlayId));
            Session.WorldObjects.Remove(Modified.Guid); Gameplay->RefreshInventoryOverlays();
            TestEqual(TEXT("Removing a pack clears its slot's guid"),Gameplay->PackSlotGuids[3],0);
            TestTrue(TEXT("Removing a pack restores the empty artwork and hides the old underlay"),
                Gameplay->PackSlots[3]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x06000F6E)
                && Gameplay->PackSlotBgs[3]->GetVisibility()==ESlateVisibility::Collapsed);
            Session.UpsertWorldObject(Original); Gameplay->RefreshInventoryOverlays();
            Session.WorldObjects[Player.Guid].ContainersCapacity=24;
            Gameplay->PackScrollOffset=24; Gameplay->RefreshInventoryOverlays();
            TestTrue(TEXT("Scrolling augmented pack slots replaces the occupied artwork with placeholders"),
                Gameplay->PackScrollOffset>0 && Gameplay->PackSlotGuids[1]==0
                && Gameplay->PackSlots[1]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x06000F6E)
                && Gameplay->PackSlotBgs[1]->GetVisibility()==ESlateVisibility::Collapsed);
            CaptureScreen(TEXT("GameplayEmptyPackScroll"));
            Session.WorldObjects[Player.Guid].ContainersCapacity=Player.ContainersCapacity;
            Gameplay->PackScrollOffset=0; Gameplay->RefreshInventoryOverlays();
            Gameplay->ShowPanelPage(TEXT("SkillManagementPanel_Field"));
            TestTrue(TEXT("Closing inventory hides pack background overlays"),Gameplay->PackSlotBgs[0]->GetVisibility()==ESlateVisibility::Collapsed);
            Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
            CaptureScreen(TEXT("GameplayPackBackgrounds"));
            TestTrue(TEXT("Reopening inventory restores the occupied pack background"),Gameplay->PackSlotBgs[3]->GetVisibility()!=ESlateVisibility::Collapsed);
        }
        auto DragInventoryWidget=[&](UWidget* Source,UWidget* Target,int32 Guid,int32 ExpectedPosition)
        {
            const auto& Geo=Canvas->GetCachedGeometry();
            const auto From=Source->GetCachedGeometry().LocalToAbsolute(Source->GetCachedGeometry().GetLocalSize()*.5);
            const auto To=Target->GetCachedGeometry().LocalToAbsolute(Target->GetCachedGeometry().GetLocalSize()*.5);
            Session.CachedC2SPackets.Reset();
            Canvas->NativeOnMouseButtonDown(Geo,FPointerEvent(0,From,From,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
            TestEqual(TEXT("Native reorder press selects expected item"),Gameplay->InvDragGuid,Guid);
            Canvas->NativeOnMouseMove(Geo,FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
            Canvas->NativeOnMouseButtonUp(Geo,FPointerEvent(0,To,To,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
            bool Moved=false;
            for(const auto& P:Session.CachedC2SPackets)
            {
                FACEBinaryReader Wire(P.Value.Payload); Wire.Skip(16);
                if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;
                Wire.ReadUInt32(); if(Wire.ReadUInt32()!=ACEGameAction::PutItemInContainer)continue;
                const auto Id=Wire.ReadUInt32(),Container=Wire.ReadUInt32(); const int32 Position=Wire.ReadInt32();
                Moved |= Id==uint32(Guid) && Container==uint32(Player.Guid) && Position==ExpectedPosition;
            }
            TestTrue(TEXT("Native reorder sends authoritative container and insertion index"),Moved);
        };
        auto AcknowledgeReorder=[&](int32 Guid,int32 Position,bool Pack)
        {
            FACEBinaryWriter Prop; Prop.WriteUInt8(1); Prop.WriteUInt32(Guid); Prop.WriteUInt32(2); Prop.WriteUInt32(Player.Guid);
            FACEBinaryReader PR(Prop.GetData()); Session.HandlePublicUpdateInstanceId(PR);
            FACEBinaryWriter Ack; Ack.WriteUInt32(Guid); Ack.WriteUInt32(Player.Guid); Ack.WriteInt32(Position); Ack.WriteInt32(Pack ? 1 : 0);
            FACEBinaryReader AR(Ack.GetData()); Session.HandleInventoryPutObjInContainer(AR);
            Gameplay->RefreshInventoryOverlays(); // no forced cache invalidation
        };
        DragInventoryWidget(Gameplay->InventorySlots[2],Gameplay->InventorySlots[0],60003,0);
        AcknowledgeReorder(60003,0,false);
        TestEqual(TEXT("Item acknowledgement updates visible inventory order"),Gameplay->InventorySlotGuids[0],60003);
        CaptureScreen(TEXT("GameplayReorderItems"));
        DragInventoryWidget(Gameplay->PackSlots[3],Gameplay->PackSlots[1],61003,0);
        AcknowledgeReorder(61003,0,true);
        TestEqual(TEXT("Pack acknowledgement invalidates the inventory cache"),Gameplay->PackSlotGuids[1],61003);
        CaptureScreen(TEXT("GameplayReorderPacks"));
        // Forward insertion is adjusted for removing the source first, as in UIElement_ItemList.
        DragInventoryWidget(Gameplay->PackSlots[1],Gameplay->PackSlots[3],61003,1);
        AcknowledgeReorder(61003,1,true);
        TestEqual(TEXT("Forward pack move has retail insertion order"),Gameplay->PackSlotGuids[2],61003);
        TFunction<void()> CheckGroundBackpackPickup=[&]()
        {
            FACEWorldObject GroundPack;GroundPack.Guid=61999;GroundPack.Name=TEXT("Dropped backpack");
            GroundPack.ItemType=ACEItemType::Container;GroundPack.ObjectDescriptionFlags=ACEObjectDescFlag::Openable;
            GroundPack.ItemsCapacity=24;GroundPack.IconId=0x06004CF7;
            GroundPack.bHasPosition=true;GroundPack.Position=Session.PlayerPosition;Session.UpsertWorldObject(GroundPack);
            TGuardValue<int32> Peace(Gameplay->CombatMode,int32(ACECombatMode::NonCombat));
            CaptureScreen(TEXT("GameplayGroundPackDropTargets"));
            int32 Empty=INDEX_NONE;
            for(int32 I=1;I<Gameplay->PackSlots.Num();++I)
                if(Gameplay->PackSlots[I]->IsVisible() && Gameplay->PackSlotGuids[I]==0){Empty=I;break;}
            TestTrue(TEXT("Ground pack fixture has an empty side slot"),Empty!=INDEX_NONE);
            for(int32 TargetIndex:{1,Empty})if(TargetIndex!=INDEX_NONE)
            {
                Session.PendingInventoryTransfers.Reset();Session.CachedC2SPackets.Reset();
                const auto& Geometry=Gameplay->PackSlots[TargetIndex]->GetCachedGeometry();
                const auto Absolute=Geometry.LocalToAbsolute(Geometry.GetLocalSize()*.5);
                TestTrue(TEXT("Portable ground bag arms an actual drag"),Gameplay->BeginWorldItemDrag(GroundPack.Guid,FVector2D(600,300)));
                Gameplay->UpdateInventoryDrag(Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute));
                Gameplay->TryFinishInventoryDrag(Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute));
                TestTrue(TEXT("Ground backpack can be dropped on an occupied or empty side slot"),HasAction(ACEGameAction::PutItemInContainer));
                LastWire(ACEGameAction::PutItemInContainer,{uint32(GroundPack.Guid),uint32(Player.Guid),uint32(TargetIndex-1)});
            }
            FACEWorldObject PackedItem;PackedItem.Guid=61998;PackedItem.ContainerId=GroundPack.Guid;
            PackedItem.ItemType=ACEItemType::Food;PackedItem.IconId=0x06004CF7;
            Session.UpsertWorldObject(PackedItem);Session.ContainerContents.Add(GroundPack.Guid,{{PackedItem.Guid,0}});
            for(float Scale:{1.f,1.5f})
            {
                Gameplay->ShowExternalContainer(GroundPack.Guid);
                CaptureScreen(TEXT("GameplayGroundPackRoot"),Scale,FVector2D(43,27));
                const auto& Geo=Canvas->GetCachedGeometry();
                const auto& Root=Gameplay->ExtPackSlots[0]->GetCachedGeometry();
                const auto Point=Root.LocalToAbsolute(Root.GetLocalSize()*.5);
                Session.CachedC2SPackets.Reset();
                for(int32 RootClickIndex=0;RootClickIndex<2;++RootClickIndex)
                {
                    Canvas->NativeOnMouseButtonDown(Geo,FPointerEvent(0,Point,Point,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
                    Canvas->NativeOnMouseButtonUp(Geo,FPointerEvent(0,Point,Point,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
                }
                TestTrue(TEXT("Root bag clicks display its items"),Gameplay->ExtItemGuids.Contains(PackedItem.Guid));
                TestFalse(TEXT("Root bag double click browses without picking it up"),HasAction(ACEGameAction::PutItemInContainer));
                Session.PendingInventoryTransfers.Reset();
                DragInventoryWidget(Gameplay->ExtPackSlots[0],Gameplay->PackSlots[1],GroundPack.Guid,0);
                TestEqual(TEXT("Root drag waits for confirmation before moving the bag"),Session.WorldObjects[GroundPack.Guid].ContainerId,0);
            }
            AcknowledgeReorder(GroundPack.Guid,0,true);
            Gameplay->HideExternalContainer(false);
            TestTrue(TEXT("Closing a picked-up root bag retains its contents"),Session.ContainerContents.Contains(GroundPack.Guid));
            TestEqual(TEXT("Root bag pickup preserves item ownership under the bag"),Session.WorldObjects[PackedItem.Guid].ContainerId,GroundPack.Guid);
            Session.ContainerContents.Remove(GroundPack.Guid);Session.WorldObjects.Remove(PackedItem.Guid);
            Session.PendingInventoryTransfers.Reset();Session.WorldObjects.Remove(GroundPack.Guid);
            Session.ContainerContents.Remove(Player.Guid);Gameplay->RefreshInventoryOverlays();
        };
        CheckGroundBackpackPickup();
        {
            FACEWorldObject Chest; Chest.Guid=62000; Chest.ItemType=ACEItemType::Container;
            Chest.ContainersCapacity=2; Chest.ItemsCapacity=24; Chest.IconId=0x06004CF7; Chest.Name=TEXT("Loot background fixture");
            Session.UpsertWorldObject(Chest);
            FACEWorldObject NestedPack=Chest; NestedPack.Guid=62001; NestedPack.ContainerId=Chest.Guid;
            Session.UpsertWorldObject(NestedPack);
            Gameplay->ShowExternalContainer(Chest.Guid); CaptureScreen(TEXT("GameplayLootPackBackground"));
            TestTrue(TEXT("Packs in an external container retain their own retail background"),
                !Gameplay->ExtPackSlotBgs.IsEmpty() && Gameplay->ExtPackSlotBgs[0]->GetVisibility()!=ESlateVisibility::Collapsed
                && Gameplay->ExtPackSlotBgs[0]->Background.GetResourceObject()==Resources->ResolveItemBackground(ACEItemType::Container,0));
            TestTrue(TEXT("Housing chest displays an empty slot beside its installed bag"),
                Gameplay->ExtPackSlots.Num()>2 && Gameplay->ExtPackSlots[2]->IsVisible() && Gameplay->ExtPackGuids[2]==0);
            Session.ContainerContents.Add(Chest.Guid,{{NestedPack.Guid,1}});
            Gameplay->LastLootOverlayHash=~uint64(0);Gameplay->RefreshExternalContainerOverlays();
            TestEqual(TEXT("ViewContents pack tags do not hide installed chest bags"),Gameplay->ExtPackGuids[1],NestedPack.Guid);
            Gameplay->OpenLootSelectedPackGuid=NestedPack.Guid;
            Gameplay->RefreshExternalContainerOverlays();
            NestedPack.IconUnderlayId=0x060010F9; Session.UpsertWorldObject(NestedPack);
            Gameplay->RefreshExternalContainerOverlays();
            TestTrue(TEXT("Nested pack underlay updates even while viewing its contents"),
                !Gameplay->ExtPackSlotBgs.IsEmpty()
                && Gameplay->ExtPackSlotBgs[0]->Background.GetResourceObject()==Resources->ResolveItemBackground(ACEItemType::Container,NestedPack.IconUnderlayId));
            // The chest shows all available slots, even when it contains fewer
            // items than one page. Exercise the real mouse path, not just offsets.
            Session.WorldObjects[Chest.Guid].ItemsCapacity=120;
            const auto ItemBar=Manager->FindElementUnder(TEXT("ExternalContainer"),TEXT("Ext_ItemListScroll"));
            const auto ItemList=Manager->FindElementUnder(TEXT("ExternalContainer"),TEXT("Ext_Container_ItemList"));
            const auto Left=Manager->FindElementUnder(TEXT("Ext_ItemListScroll"),TEXT("ScrollBar_Left"));
            const auto Right=Manager->FindElementUnder(TEXT("Ext_ItemListScroll"),TEXT("ScrollBar_Right"));
            auto Press=[&](FVector2D Layout)
            {
                const auto& Geo=Canvas->GetCachedGeometry();
                const auto Absolute=Geo.LocalToAbsolute(Canvas->LayoutToViewport(Layout));
                Canvas->NativeOnMouseButtonDown(Geo,FPointerEvent(0,Absolute,Absolute,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
            };
            auto Release=[&](FVector2D Layout)
            {
                const auto& Geo=Canvas->GetCachedGeometry();
                const auto Absolute=Geo.LocalToAbsolute(Canvas->LayoutToViewport(Layout));
                Canvas->NativeOnMouseButtonUp(Geo,FPointerEvent(0,Absolute,Absolute,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
            };
            auto Arrow=[&](const TSharedPtr<FACEUIElement>& Element)
            {
                if(!TestTrue(TEXT("Chest scrollbar arrow exists"),Element.IsValid()))return;
                const FVector2D P=FVector2D(Element->GetScreenOrigin())+FVector2D(Element->Width,Element->Height)*.5;
                Press(P);Release(P);
            };
            for(float Scale:{1.f,1.5f}) for(int32 Count:{0,3,120})
            {
                Session.ContainerContents.Add(Chest.Guid,{{NestedPack.Guid,1}});
                for(int32 I=0;I<120;++I)
                {
                    Session.WorldObjects.Remove(62100+I);
                    if(I<Count)
                    {
                        FACEWorldObject ChestItem;ChestItem.Guid=62100+I;ChestItem.ContainerId=Chest.Guid;
                        ChestItem.ItemType=ACEItemType::Misc;ChestItem.IconId=0x06004CF7;ChestItem.PlacementPosition=I;
                        Session.WorldObjects.Add(ChestItem.Guid,ChestItem);
                        Session.ContainerContents[Chest.Guid].Add({ChestItem.Guid,0});
                    }
                }
                for(int32 Container:{Chest.Guid,NestedPack.Guid})
                {
                    Gameplay->OpenLootSelectedPackGuid=Container;Gameplay->ExtItemScrollOffset=0;
                    Gameplay->LastLootOverlayHash=~uint64(0);Gameplay->RefreshExternalContainerOverlays();
                    CaptureScreen(TEXT("GameplayChestScrollbar"),Scale);
                    if(!TestTrue(TEXT("Chest scrollbar has a visible track"),ItemBar && ItemList && ItemBar->bVisible))continue;
                    const int32 Maximum=Session.WorldObjects[Container].ItemsCapacity-FMath::Max(1,ItemList->Width/32);
                    Arrow(Right);TestEqual(TEXT("Chest right arrow advances one slot"),Gameplay->ExtItemScrollOffset,1);
                    Arrow(Left);TestEqual(TEXT("Chest left arrow returns one slot"),Gameplay->ExtItemScrollOffset,0);
                    Arrow(Left);TestEqual(TEXT("Chest left arrow clamps at start"),Gameplay->ExtItemScrollOffset,0);
                    const FVector2D Start=FVector2D(ItemBar->GetScreenOrigin())+FVector2D(20,ItemBar->Height*.5);
                    const FVector2D End=Start+FVector2D(ItemBar->Width,0);
                    Press(Start);TestTrue(TEXT("Chest thumb begins dragging regardless of item count"),Gameplay->IsScrollbarDragActive());
                    const auto& Geo=Canvas->GetCachedGeometry();
                    const auto From=Geo.LocalToAbsolute(Canvas->LayoutToViewport(Start));
                    const auto To=Geo.LocalToAbsolute(Canvas->LayoutToViewport(End));
                    Canvas->NativeOnMouseMove(Geo,FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
                    Release(End);
                    TestFalse(TEXT("Chest mouse release ends scrollbar drag"),Gameplay->IsScrollbarDragActive());
                    TestEqual(TEXT("Chest drag reaches the last capacity slot"),Gameplay->ExtItemScrollOffset,Maximum);
                    Arrow(Right);TestEqual(TEXT("Chest right arrow clamps at end"),Gameplay->ExtItemScrollOffset,Maximum);
                    if(Container==Chest.Guid && Count==120)
                        TestEqual(TEXT("Chest scrolling displays its final item"),Gameplay->ExtItemGuids.Last(),62219);
                    Arrow(Left);TestEqual(TEXT("Chest left arrow works after thumb drag"),Gameplay->ExtItemScrollOffset,Maximum-1);
                    Gameplay->ScrollExternalContainer(1.f);
                    TestEqual(TEXT("Chest wheel shares the scrollbar range"),Gameplay->ExtItemScrollOffset,FMath::Max(0,Maximum-4));
                }
            }
            // External ContainerList is draggable in retail, independently of
            // clicking it to browse the installed pack. Exercise captured input.
            TFunction<void()> CheckChestPackRemoval=[&]()
            {
                FACEWorldObject PackedItem;PackedItem.Guid=62300;PackedItem.ContainerId=NestedPack.Guid;
                PackedItem.ItemType=ACEItemType::Misc;PackedItem.IconId=0x06004CF7;
                Session.UpsertWorldObject(PackedItem);
                Session.ContainerContents.Add(NestedPack.Guid,{{PackedItem.Guid,0}});
                auto ClickPack=[&](FKey Button)
                {
                    const auto& Geo=Canvas->GetCachedGeometry();
                    const auto& Slot=Gameplay->ExtPackSlots[1]->GetCachedGeometry();
                    const auto P=Slot.LocalToAbsolute(Slot.GetLocalSize()*.5);
                    Canvas->NativeOnMouseButtonDown(Geo,FPointerEvent(0,P,P,{Button},Button,0,FModifierKeysState()));
                    if(Button==EKeys::LeftMouseButton)TestEqual(TEXT("Chest pack click reaches the draggable container list"),Gameplay->InvDragGuid,NestedPack.Guid);
                    Canvas->NativeOnMouseButtonUp(Geo,FPointerEvent(0,P,P,{},Button,0,FModifierKeysState()));
                };
                for(float Scale:{1.f,1.5f})
                {
                    Gameplay->OpenLootSelectedPackGuid=Chest.Guid;Gameplay->ExtItemScrollOffset=0;
                    Gameplay->RefreshExternalContainerOverlays();
                    CaptureScreen(TEXT("GameplayChestPackRemoval"),Scale,FVector2D(43,27));
                    Session.CachedC2SPackets.Reset();ClickPack(EKeys::LeftMouseButton);ClickPack(EKeys::LeftMouseButton);
                    TestEqual(TEXT("Repeated chest-pack clicks open its contents"),Gameplay->OpenLootSelectedPackGuid,NestedPack.Guid);
                    TestEqual(TEXT("Chest-pack clicks select the pack"),Client->GetSelectedObject().Guid,NestedPack.Guid);
                    TestFalse(TEXT("Repeated chest-pack clicks do not pick up the pack"),HasAction(ACEGameAction::PutItemInContainer));
                    TestTrue(TEXT("Opening a chest pack displays its contents"),Gameplay->ExtItemGuids.Contains(PackedItem.Guid));
                    Session.CachedC2SPackets.Reset();ClickPack(EKeys::RightMouseButton);
                    LastWire(ACEGameAction::IdentifyObject,{uint32(NestedPack.Guid)});
                    const auto& PackGeometry=Gameplay->ExtPackSlots[1]->GetCachedGeometry();
                    const auto PackCenter=PackGeometry.LocalToAbsolute(PackGeometry.GetLocalSize()*.5);
                    TestTrue(TEXT("Chest pack arms a drag"),Gameplay->TryBeginInventoryDrag(Canvas->GetCachedGeometry().AbsoluteToLocal(PackCenter)));
                    Gameplay->CancelInventoryDrag();
                    TestEqual(TEXT("Cancel leaves the pack in the chest"),Session.WorldObjects[NestedPack.Guid].ContainerId,Chest.Guid);
                    CaptureScreen(TEXT("GameplayChestPackDrag"),Scale,FVector2D(43,27));
                    int32 Destination=1;
                    if(Scale>1.f)for(int32 I=1;I<Gameplay->PackSlotGuids.Num();++I)
                        if(!Gameplay->PackSlotGuids[I]){Destination=I;break;}
                    Session.PendingInventoryTransfers.Reset();
                    DragInventoryWidget(Gameplay->ExtPackSlots[1],Gameplay->PackSlots[Destination],NestedPack.Guid,Destination-1);
                    TestEqual(TEXT("Pack removal waits for the server"),Session.WorldObjects[NestedPack.Guid].ContainerId,Chest.Guid);
                    Gameplay->RefreshExternalContainerOverlays();
                    TestEqual(TEXT("Unconfirmed removal keeps the open chest pack"),Gameplay->OpenLootSelectedPackGuid,NestedPack.Guid);
                    AcknowledgeReorder(NestedPack.Guid,Destination-1,true);
                    Gameplay->RefreshExternalContainerOverlays();
                    TestEqual(TEXT("Confirmed removal returns the chest to its root"),Gameplay->OpenLootSelectedPackGuid,Chest.Guid);
                    TestFalse(TEXT("Removed pack disappears from chest slots"),Gameplay->ExtPackGuids.Contains(NestedPack.Guid));
                    TestTrue(TEXT("Removed pack appears in owned pack slots"),Gameplay->PackSlotGuids.Contains(NestedPack.Guid));
                    TestEqual(TEXT("Moving a pack preserves its item's container"),Session.WorldObjects[PackedItem.Guid].ContainerId,NestedPack.Guid);
                    TestTrue(TEXT("Moving a pack preserves its contents list"),Session.ContainerContents[NestedPack.Guid].ContainsByPredicate(
                        [&](const auto& Entry){return Entry.ItemGuid==PackedItem.Guid;}));
                    // Restore via an authoritative move back into the chest for
                    // the second scale/destination; never mutate optimistically.
                    FACEBinaryWriter Ack;Ack.WriteUInt32(NestedPack.Guid);Ack.WriteUInt32(Chest.Guid);Ack.WriteInt32(0);Ack.WriteInt32(1);
                    FACEBinaryReader Reader(Ack.GetData());Session.HandleInventoryPutObjInContainer(Reader);
                    Gameplay->RefreshInventoryOverlays();Gameplay->RefreshExternalContainerOverlays();
                }
            };
            CheckChestPackRemoval();
            #include "ACERetailBackpackUIFixture.inl"
            Gameplay->HideExternalContainer(false);
            TestTrue(TEXT("Closing loot hides its pack background"),
                Gameplay->ExtPackSlotBgs.IsEmpty() || Gameplay->ExtPackSlotBgs[0]->GetVisibility()==ESlateVisibility::Collapsed);
        }
        Session.WorldObjects=SavedObjects; Session.ContainerContents=SavedContents;
        Gameplay->SelectedPackGuid=Player.Guid; Gameplay->RefreshInventoryOverlays();

        Session.SocketC2S->Close(); Sockets->DestroySocket(Session.SocketC2S); Session.SocketC2S=nullptr;
    }
    else AddError(TEXT("Could not bind inventory loopback fixture"));
    if (Receiver) { Receiver->Close(); Sockets->DestroySocket(Receiver); }

    // Exercise actual software cursor geometry and the inventory's live capture rig.
    {
        const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false)
            .RequiresHitProxies(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
        auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        auto* PreviewGI=NewObject<UGameInstance>(GEngine); World->SetGameInstance(PreviewGI); PreviewGI->Init();
        auto* PreviewDat=PreviewGI->GetSubsystem<UACEDatSubsystem>();
        TestTrue(TEXT("Preview world uses the real DAT subsystem"),PreviewDat->LoadDatDirectory(ACERetailUIScreenTestsDatDirectory()));
        auto* Controller=World->SpawnActor<AACEPlayerController>();
        Controller->Client=Client; Gameplay->PlayerController=Controller;
        TFunction<void()> CheckKeyboardTargets=[&]
        {
            auto& Session=*Client->Session;
            const auto SavedObjects=Session.WorldObjects; const auto SavedSelection=Session.SelectedObject;
            const auto SavedFellowship=Session.Fellowship; const auto SavedPosition=Session.PlayerPosition;
            Session.WorldObjects.Reset();
            FACEPosition Position;Position.CellId=0xDA55001D;Position.Location=FVector(100,100,20);Session.PlayerPosition=Position;
            FACEWorldObject Self;Self.Guid=Session.PlayerGuid;Self.Name=TEXT("Self");Self.ItemType=ACEItemType::Creature;Self.bIsPlayer=true;Self.bHasPosition=true;Self.Position=Position;
            Session.WorldObjects.Add(Self.Guid,Self);
            auto AddObject=[&](int32 Guid,float Distance,bool Player)
            {
                auto Object=Self;Object.Guid=Guid;Object.Position.Location.X+=Distance;Object.bIsPlayer=Player;
                Session.WorldObjects.Add(Guid,Object);
            };
            AddObject(80001,10,true);AddObject(80002,20,true);AddObject(80003,5,false);AddObject(80004,80,true);
            Gameplay->CycleKeyboardSelection(TEXT("Player"),0);
            TestEqual(TEXT("Closest player excludes nearer NPC and self"),Client->GetSelectedObject().Guid,80001);
            Gameplay->CycleKeyboardSelection(TEXT("Player"),1);
            TestEqual(TEXT("Next player follows retail distance ordering"),Client->GetSelectedObject().Guid,80002);
            Gameplay->CycleKeyboardSelection(TEXT("Player"),1);
            TestEqual(TEXT("Next player wraps and excludes distant objects"),Client->GetSelectedObject().Guid,80001);
            Gameplay->CycleKeyboardSelection(TEXT("CompassItem"),0);
            TestEqual(TEXT("Radar selection can choose the nearer NPC"),Client->GetSelectedObject().Guid,80003);
            // Attackable also occurs on non-creatures; it is not a monster type.
            AddObject(80010,12,false);AddObject(80011,15,false);
            Session.WorldObjects[80010].ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
            Session.WorldObjects[80011].ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
            AddObject(80012,2,false);
            Session.WorldObjects[80012].ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
            Session.WorldObjects[80012].PetOwnerId=80001;
            const int32 ItemTypes[]={ACEItemType::Misc,ACEItemType::Portal,ACEItemType::Container,ACEItemType::Misc,ACEItemType::Misc};
            const TCHAR* ItemNames[]={TEXT("Door"),TEXT("Portal"),TEXT("Corpse"),TEXT("Sign"),TEXT("Ground loot")};
            for(int32 I=0;I<UE_ARRAY_COUNT(ItemTypes);++I)
            {
                auto NearbyItem=Self;NearbyItem.Guid=80100+I;NearbyItem.bIsPlayer=false;NearbyItem.ItemType=ItemTypes[I];
                NearbyItem.Name=ItemNames[I];
                NearbyItem.Position.Location.X+=I+1;NearbyItem.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
                if(I==0)NearbyItem.ObjectDescriptionFlags|=ACEObjectDescFlag::Door;
                if(I==2)NearbyItem.ObjectDescriptionFlags|=ACEObjectDescFlag::Corpse;
                Session.WorldObjects.Add(NearbyItem.Guid,NearbyItem);
            }
            Controller->CycleNearbyTarget(true,0);
            TestEqual(TEXT("Closest monster excludes attackable doors, portals, containers and loose items"),Client->GetSelectedObject().Guid,80010);
            Controller->CycleNearbyTarget(true,1);
            TestEqual(TEXT("Next monster selects another creature"),Client->GetSelectedObject().Guid,80011);
            Controller->CycleNearbyTarget(true,1);
            TestEqual(TEXT("Monster traversal wraps without visiting items"),Client->GetSelectedObject().Guid,80010);
            Controller->CycleNearbyTarget(true,-1);
            TestEqual(TEXT("Previous monster also excludes items"),Client->GetSelectedObject().Guid,80011);
            AddObject(80013,74.5f,false);
            Session.WorldObjects[80013].ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
            Controller->CycleNearbyTarget(true,1);
            TestEqual(TEXT("Monster at retail's compass rim remains selectable"),Client->GetSelectedObject().Guid,80013);
            Controller->CycleNearbyTarget(true,-1);
            TestEqual(TEXT("Reverse cycling returns from beyond the compass"),Client->GetSelectedObject().Guid,80011);
            Session.WorldObjects.Remove(80013);
            Controller->CycleNearbyTarget(false,0);
            TestEqual(TEXT("Item traversal still includes doors"),Client->GetSelectedObject().Guid,80100);
            Controller->CycleNearbyTarget(false,1);
            TestEqual(TEXT("Item traversal still includes portals"),Client->GetSelectedObject().Guid,80101);
            TFunction<void()> CheckAutoTarget=[&]()
            {
                // This fixture replaces the subsystem session; restore its normal
                // selection delegate bridge for the end-to-end targeting checks.
                const auto Bridge=Session.OnSelectionChanged.AddLambda([&](const FACESelectedObject& Selection){Client->OnSelectionChanged.Broadcast(Selection);});
                ON_SCOPE_EXIT{Session.OnSelectionChanged.Remove(Bridge);};
                TGuardValue<int32> Attacker(Session.LastAttackerGuid,0);
                TGuardValue<double> AttackerTime(Session.LastAttackerTimeSeconds,0.);
                auto Distant=Session.WorldObjects[80010];Distant.Guid=80013;Distant.Position.Location.X=Self.Position.Location.X+74.9f;
                Session.WorldObjects.Add(Distant.Guid,Distant);
                Client->SelectObject(80011);Controller->CycleNearbyTarget(true,1);
                TestEqual(TEXT("Monster hotkeys reach the outer edge of retail's radar range"),Client->GetSelectedObject().Guid,80013);
                Session.WorldObjects[80010].bDying=Session.WorldObjects[80011].bDying=true;
                Controller->CycleNearbyTarget(true,0);
                TestEqual(TEXT("Nearest monster has no separate 60 metre cutoff"),Client->GetSelectedObject().Guid,80013);
                Session.WorldObjects.Remove(80013);
                Session.WorldObjects[80010].bDying=Session.WorldObjects[80011].bDying=false;
                TGuardValue<uint32> Options(Session.CharacterOptions1,Session.CharacterOptions1|0x2000u);
                TGuardValue<int32> Mode(Gameplay->CombatMode,2);
                TGuardValue<TObjectPtr<AACEPlayerController>> Owner(Gameplay->PlayerController,Controller);
                Client->SelectObject(0);Gameplay->TryAutoTargetOnCombatEnter();
                TestEqual(TEXT("Saved auto-target selects nearest on combat entry without last attacker"),Client->GetSelectedObject().Guid,80010);
                Session.WorldObjects[80010].bDying=true;
                Client->SelectObject(0);Gameplay->TickCombatAutoAttack(.016f);
                TestEqual(TEXT("Death selection loss automatically chooses the next living monster"),Client->GetSelectedObject().Guid,80011);
                Session.WorldObjects[80010].bDying=false;
                Gameplay->TryAutoTargetOnCombatEnter();
                TestEqual(TEXT("Auto-target preserves the player's valid existing target"),Client->GetSelectedObject().Guid,80011);
                Session.CharacterOptions1&=~0x2000u;Client->SelectObject(0);Gameplay->TryAutoTargetOnCombatEnter();
                TestFalse(TEXT("Disabled auto-target leaves an empty selection"),Client->GetSelectedObject().bValid);
                Session.CharacterOptions1|=0x2000u;Gameplay->CombatMode=4;Gameplay->TryAutoTargetOnCombatEnter();
                TestEqual(TEXT("Missile auto-target uses the same monster filter and nearest fallback"),Client->GetSelectedObject().Guid,80010);
                const auto Removed=Session.WorldObjects[80010];
                FACEBinaryWriter Delete;Delete.WriteUInt32(80010);FACEBinaryReader DeleteReader(Delete.GetData());
                Session.HandleObjectDelete(DeleteReader);Gameplay->TickCombatAutoAttack(.016f);
                TestEqual(TEXT("ObjectDelete without a death-motion packet also retargets"),Client->GetSelectedObject().Guid,80011);
                Session.WorldObjects.Add(Removed.Guid,Removed);
                Client->SelectObject(0);Gameplay->TickCombatAutoAttack(.016f);
                TestFalse(TEXT("Explicitly clearing a live monster does not immediately reselect it"),Client->GetSelectedObject().bValid);
                Session.LastAttackerGuid=80011;Session.LastAttackerTimeSeconds=FPlatformTime::Seconds();
                Gameplay->TryAutoTargetOnCombatEnter();
                TestEqual(TEXT("Retail recent-attacker preference precedes nearest-target fallback"),Client->GetSelectedObject().Guid,80011);
                const auto AutoTargetObjects=Session.WorldObjects;
                for(int32 Combat:{2,4}) for(uint32 Cell:{0xDA55001Du,0x0143014Fu})
                {
                    Gameplay->CombatMode=Combat;
                    Session.PlayerPosition=Position;Session.PlayerPosition.CellId=Cell;
                    Session.WorldObjects=AutoTargetObjects;
                    for(auto& Pair:Session.WorldObjects)Pair.Value.Position.CellId=Cell;
                    const float RadarRange=Cell==0xDA55001Du ? 75.f : 25.f;
                    const FString Context=FString::Printf(TEXT(" (mode %d, cell %08X)"),Combat,Cell);
                    auto RecentAttacker=Session.WorldObjects[80011];RecentAttacker.Guid=80013;
                    RecentAttacker.Position.Location.X=Position.Location.X+RadarRange;
                    Session.WorldObjects.Add(RecentAttacker.Guid,RecentAttacker);
                    Session.LastAttackerGuid=RecentAttacker.Guid;Session.LastAttackerTimeSeconds=FPlatformTime::Seconds();
                    auto RetargetAfterDeath=[&]()
                    {
                        Client->SelectObject(80010);
                        Session.WorldObjects[80010].bDying=true;
                        Client->SelectObject(0);Gameplay->TickCombatAutoAttack(.016f);
                        Session.WorldObjects[80010].bDying=false;
                    };
                    RetargetAfterDeath();
                    TestEqual(TEXT("Post-kill retarget chooses nearest instead of farther recent attacker")+Context,Client->GetSelectedObject().Guid,80011);
                    Client->SelectObject(0);Gameplay->TryAutoTargetOnCombatEnter();
                    TestEqual(TEXT("Combat entry may retaliate against an attacker exactly at radar range")+Context,Client->GetSelectedObject().Guid,80013);
                    Session.WorldObjects[80013].Position.Location.X+=.1f;
                    Client->SelectObject(0);Gameplay->TryAutoTargetOnCombatEnter();
                    TestEqual(TEXT("Combat entry rejects recent attackers outside radar range")+Context,Client->GetSelectedObject().Guid,80010);
                    RetargetAfterDeath();
                    TestEqual(TEXT("Post-kill retarget rejects recent attackers outside radar range")+Context,Client->GetSelectedObject().Guid,80011);
                    Session.WorldObjects[80011].Position.Location.X=Position.Location.X-15.f;
                    RetargetAfterDeath();
                    TestEqual(TEXT("Post-kill retarget includes the nearest creature behind the player")+Context,Client->GetSelectedObject().Guid,80011);
                    Session.WorldObjects[80011].bDying=true;
                    RetargetAfterDeath();
                    TestFalse(TEXT("Post-kill retarget stays empty when only off-radar or ineligible creatures remain")+Context,Client->GetSelectedObject().bValid);
                }
                Session.WorldObjects=AutoTargetObjects;Session.PlayerPosition=Position;
            };
            CheckAutoTarget();
            // Keep the input sequence out of the already-large screen test's optimizer unit.
            TFunction<void()> CheckTargetKeys=[&]()
            {
                // Exercise the complete physical-key -> action -> PlayerTick -> target path.
                // Retail CPlayerSystem::SelectNext calls ObjectIsAttackable, which checks
                // InqType & 0x10 before attackability. v78 omitted that creature check.
                // Legacy action IDs Semicolon/Apostrophe are NOT their physical bindings:
                // retail ItemSelectionCommands maps closest=', next=;, previous=L.
                TGuardValue<TObjectPtr<UACEUIGameplayBinder>> BinderGuard(Controller->DatGameplayBinder,Gameplay);
                TGuardValue<int32> CombatGuard(Gameplay->CombatMode,1);
                // This world has no viewport; skip hover-tooltip creation while polling keys.
                TGuardValue<bool> MouseLookGuard(Controller->bMouseLookActive,true);
                Controller->PlayerInput=NewObject<UPlayerInput>(Controller);
                Gameplay->ClearChatEntryFocus();
                ACEInputBindings::Reload();ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
                auto PressTarget=[&](FKey Key,int32 Expected,const TCHAR* Reason)
                {
                    Controller->PlayerInput->FlushPressedKeys();
                    Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key,IE_Pressed,1.f));
                    // PlayerTick processes input once; pre-processing it would consume JustPressed.
                    Controller->PlayerTick(.016f);
                    TestEqual(*FString::Printf(TEXT("%s: key=%s combat=%d"),Reason,*Key.ToString(),Gameplay->CombatMode),
                        Client->GetSelectedObject().Guid,Expected);
                };
                for(int32 Mode:{1,2,4,8}) // peace, melee, missile, magic
                {
                    Gameplay->CombatMode=Mode;Client->SelectObject(80104);
                    PressTarget(EKeys::Apostrophe,80010,TEXT("Closest monster skips all five nearer item types and a peaceful NPC"));
                    PressTarget(EKeys::Semicolon,80011,TEXT("Next monster moves forward"));
                    PressTarget(EKeys::Semicolon,80010,TEXT("Next monster wraps without visiting an item"));
                    PressTarget(EKeys::L,80011,TEXT("Previous monster wraps backwards"));
                    PressTarget(EKeys::Apostrophe,80010,TEXT("Closest always returns to nearest monster"));
                    PressTarget(EKeys::Backslash,80100,TEXT("Closest item selects the door"));
                    for(int32 I=1;I<UE_ARRAY_COUNT(ItemTypes);++I)
                        PressTarget(EKeys::RightBracket,80100+I,ItemNames[I]);
                    PressTarget(EKeys::RightBracket,80100,TEXT("Next item wraps without visiting creatures or players"));
                    PressTarget(EKeys::LeftBracket,80104,TEXT("Previous item wraps backwards"));
                }
                // A stale attackable flag cannot turn a dead creature back into a target.
                Session.WorldObjects[80010].bDying=true;
                PressTarget(EKeys::Apostrophe,80011,TEXT("Dying creature is excluded"));
                Session.WorldObjects[80011].bDying=true;Client->SelectObject(80104);
                PressTarget(EKeys::Apostrophe,80104,TEXT("No monster leaves the selection unchanged"));
                PressTarget(EKeys::Semicolon,80104,TEXT("No monster never falls back to an item"));
                Session.WorldObjects[80010].bDying=false;Session.WorldObjects[80011].bDying=false;
                ACEInputBindings::BeginEdit();
                ACEInputBindings::Set(ACEInputBindings::Action(TEXT("ClosestMonster")),0,FInputChord(EKeys::F7));
                ACEInputBindings::Commit();
                PressTarget(EKeys::Apostrophe,80104,TEXT("Rebinding removes the former closest-monster key"));
                PressTarget(EKeys::F7,80010,TEXT("Rebound closest-monster key retains its type filter"));
                ACEInputBindings::BeginEdit();
                TestTrue(TEXT("Retail target-selection keymap imports"),ACEInputBindings::ImportRetailKeymap(RetailCustomKeymap).bSuccess);
                ACEInputBindings::Commit();
                PressTarget(EKeys::Semicolon,80011,TEXT("Imported retail next-monster binding retains its type filter"));
                PressTarget(EKeys::L,80010,TEXT("Imported retail previous-monster binding retains its direction"));
                PressTarget(EKeys::Backslash,80100,TEXT("Imported retail item binding remains separate"));
                Controller->PlayerInput->FlushPressedKeys();
                ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
            };
            CheckTargetKeys();
            for(int32 Guid:{80010,80011,80100,80101,80102,80103,80104})Session.WorldObjects.Remove(Guid);
            Session.Fellowship.Members.Reset();
            for(int32 Guid:{80002,Self.Guid,80001}){FACEFellowshipMember Member;Member.Guid=Guid;Session.Fellowship.Members.Add(Member);}
            Client->SelectObject(80002);Gameplay->CycleKeyboardSelection(TEXT("Fellow"),1);
            TestEqual(TEXT("Fellowship selection follows member order and includes self"),Client->GetSelectedObject().Guid,Self.Guid);
            FACEWorldObject Stack;Stack.Guid=81000;Stack.Name=TEXT("Stack");Stack.ItemType=ACEItemType::MissileWeapon;Stack.ContainerId=Self.Guid;Stack.StackSize=50;Stack.MaxStackSize=100;
            Session.WorldObjects.Add(Stack.Guid,Stack);Client->SelectObject(Stack.Guid);Gameplay->HandleSelectionChanged(Client->GetSelectedObject());
            Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));Gameplay->RefreshSelectionOverlay();
            TestTrue(TEXT("Selected stacks expose an editable quantity"),Gameplay->StackAmountEntry && Gameplay->StackAmountEntry->IsVisible());
            Gameplay->HandleStackAmountCommitted(FText::FromString(TEXT("999")),ETextCommit::OnEnter);
            TestEqual(TEXT("Stack quantity cannot exceed the actual stack"),Gameplay->SelectedStackAmount,50);
            Gameplay->HandleStackAmountCommitted(FText::FromString(TEXT("7")),ETextCommit::OnEnter);
            TestEqual(TEXT("Retail split-stack quantity is retained for transfer"),Gameplay->SelectedStackAmount,7);
            CaptureScreen(TEXT("GameplayKeyboardStackQuantity"));
            ACEInputBindings::Reload();ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();
            ACEInputBindings::Set(ACEInputBindings::Action(TEXT("ToggleFriendsPanel")),0,FInputChord(EKeys::F3));ACEInputBindings::Commit();
            Controller->PlayerInput=NewObject<UPlayerInput>(Controller);
            auto Press=[&](FKey Key)
            {
                Controller->PlayerInput->FlushPressedKeys();
                Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key,IE_Pressed,1.f));
                Controller->PlayerInput->ProcessInputStack({},.016f,false);
                Gameplay->PollKeyboardActions(Controller);
            };
            TFunction<void()> CheckCombatPowerKeys=[&]
            {
                TGuardValue<int32> KeepCombat(Gameplay->CombatMode,Gameplay->CombatMode);
                TGuardValue<float> KeepPower(Gameplay->RequestedAttackPower,.5f);
                for (int32 Mode:{int32(ACECombatMode::Melee),int32(ACECombatMode::Missile)})
                {
                    Gameplay->CombatMode=Mode;ACEInputBindings::SetCombatContext(Mode);
                    Gameplay->RequestedAttackPower=.5f;Press(EKeys::Insert);
                    TestTrue(TEXT("Insert lowers the requested combat power by one retail step"),FMath::IsNearlyEqual(Gameplay->RequestedAttackPower,2.f/6));
                    Press(EKeys::PageUp);
                    TestTrue(TEXT("Page Up raises the requested combat power"),FMath::IsNearlyEqual(Gameplay->RequestedAttackPower,.5f));
                    Gameplay->RequestedAttackPower=0.f;
                    for(int32 Step=1;Step<=8;++Step)
                    {
                        Press(EKeys::PageUp);
                        TestTrue(TEXT("Power traverses all seven retail positions and clamps at maximum"),FMath::IsNearlyEqual(Gameplay->RequestedAttackPower,FMath::Min(Step,6)/6.f));
                    }
                    for(int32 Step=5;Step>=-2;--Step)
                    {
                        Press(EKeys::Insert);
                        TestTrue(TEXT("Power traverses retail positions downward and clamps at minimum"),FMath::IsNearlyEqual(Gameplay->RequestedAttackPower,FMath::Max(Step,0)/6.f));
                    }
                    Gameplay->RequestedAttackPower=.43f;Press(EKeys::Insert);
                    TestTrue(TEXT("Mouse slider values round to the nearest sixth before stepping"),FMath::IsNearlyEqual(Gameplay->RequestedAttackPower,2.f/6));
                }
                Controller->PlayerInput->FlushPressedKeys();
            };
            CheckCombatPowerKeys();
            {
                TGuardValue<int32> KeepCombat(Gameplay->CombatMode,Gameplay->CombatMode);
                TGuardValue<int32> KeepServerCombat(Session.PlayerVitals.CombatMode,Session.PlayerVitals.CombatMode);
                Session.ClearPendingUse();
                FACEWorldObject UseTarget;UseTarget.Guid=81001;UseTarget.bHasPosition=true;
                UseTarget.Position=Position;UseTarget.Position.Location.X+=10;UseTarget.ItemUseable=32;UseTarget.UseRadius=.6f;
                for(int32 Mode:{1,2,4,8}) for(int32 Kind=0;Kind<3;++Kind)
                {
                    Gameplay->CombatMode=Session.PlayerVitals.CombatMode=Mode;
                    UseTarget.ItemType=Kind==0?ACEItemType::Creature:Kind==1?ACEItemType::Portal:0;
                    UseTarget.ObjectDescriptionFlags=ACEObjectDescFlag::Stuck|(Kind==2?ACEObjectDescFlag::Door:0);
                    Session.WorldObjects.Add(UseTarget.Guid,UseTarget);Client->SelectObject(UseTarget.Guid);
                    Controller->ClearServerMoveTo();
                    const uint32 BeforeUse=Session.NextGameActionSequence;
                    Press(EKeys::R);
                    TestTrue(TEXT("R approaches NPCs, portals and doors in every combat stance"),Controller->bServerMoveToActive);
                    TestEqual(TEXT("R approaches the selected object"),Controller->ServerMoveToTargetGuid,UseTarget.Guid);
                    TestTrue(TEXT("Retail world approach keeps its initial Use pending until server completion"),Session.IsUseBusy());
                    TestTrue(TEXT("R sends Use before approach so the server can supply movement rules"),Session.NextGameActionSequence>BeforeUse);
                    const uint32 AfterUse=Session.NextGameActionSequence;
                    Press(EKeys::R);
                    TestEqual(TEXT("Repeated R during approach does not send duplicate Use or change stance"),Session.NextGameActionSequence,AfterUse);
                    TestEqual(TEXT("Repeated R retains exactly one pending server Use transaction"),Controller->ApproachUseSendCount,1);
                    TestEqual(TEXT("Pending Use belongs to the selected world object"),Session.UseSourceGuid,UseTarget.Guid);
                    TestEqual(TEXT("World use preserves combat stance"),Session.PlayerVitals.CombatMode,Mode);
                    FACEBinaryWriter Done;Done.WriteUInt32(0);
                    FACEBinaryReader DoneReader(Done.GetData());Session.HandleUseDone(DoneReader);
                    TestFalse(TEXT("Server UseDone releases the approach transaction for the next interaction"),Session.IsUseBusy());
                }
                Controller->ClearServerMoveTo();Session.PendingEquipmentGuid=81002;
                Press(EKeys::R);
                TestFalse(TEXT("R still waits for an actual equipment transaction"),Controller->bServerMoveToActive);
                Session.CancelEquipmentSwap();Session.WorldObjects.Remove(UseTarget.Guid);
                Client->SelectObject(Stack.Guid);
            }
            Gameplay->ShowPanelPage(TEXT("SocialPanel_Field"));Gameplay->SyncSocialPanelTab(TEXT("AllegiancePage"));
            Press(EKeys::F3);
            TestEqual(TEXT("Friends shortcut changes social subtab instead of closing the panel"),Gameplay->ActiveSocialTab,FString(TEXT("FriendsPage")));
            TestEqual(TEXT("Friends shortcut keeps the social panel open"),Gameplay->ActivePanelPage,FString(TEXT("SocialPanel_Field")));
            ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
            struct FPanelKeyCase {FKey Key;const TCHAR* Page;const TCHAR* Tab;};
            const FPanelKeyCase PanelCases[]={
                {EKeys::F9,TEXT("SkillManagementPanel_Field"),TEXT("SkillPage")},
                {EKeys::F8,TEXT("SkillManagementPanel_Field"),TEXT("AttributePage")},
                {EKeys::F9,TEXT("SkillManagementPanel_Field"),TEXT("SkillPage")},
                {EKeys::F6,TEXT("SpellManagementPanel_Field"),TEXT("SpellComponentPage")},
                {EKeys::F5,TEXT("SpellManagementPanel_Field"),TEXT("SpellbookPage")},
                {EKeys::F6,TEXT("SpellManagementPanel_Field"),TEXT("SpellComponentPage")},
                {EKeys::F3,TEXT("SocialPanel_Field"),TEXT("AllegiancePage")},
                {EKeys::F4,TEXT("SocialPanel_Field"),TEXT("FellowshipPage")},
                {EKeys::F3,TEXT("SocialPanel_Field"),TEXT("AllegiancePage")},
                {EKeys::F12,TEXT("InventoryPanel_Field"),nullptr},
                {EKeys::F10,TEXT("WorldPanel_Field"),nullptr},
                {EKeys::F11,TEXT("OptionsPanel_Field"),nullptr}};
            for(const auto& Case:PanelCases)
            {
                Press(Case.Key);
                TestEqual(*FString::Printf(TEXT("%s opens its intended panel"),*Case.Key.ToString()),Gameplay->ActivePanelPage,FString(Case.Page));
                if(Case.Tab)TestTrue(*FString::Printf(TEXT("%s selects its intended retail tab"),*Case.Key.ToString()),Manager->FindElementByName(Case.Tab)->bVisible);
            }
            Press(EKeys::F8);Press(EKeys::F8);
            TestTrue(TEXT("Repeating the same Attributes shortcut closes the panel"),Gameplay->ActivePanelPage.IsEmpty());
            ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::P,0,FInputChord(EKeys::F7));ACEInputBindings::Commit();
            Press(EKeys::F7);
            TestTrue(TEXT("Rebound Attributes action selects the Attributes tab"),Manager->FindElementByName(TEXT("AttributePage"))->bVisible);
            TestEqual(TEXT("Rebound Attributes keeps the panel open"),Gameplay->ActivePanelPage,FString(TEXT("SkillManagementPanel_Field")));
            Controller->PlayerInput->FlushPressedKeys();ACEInputBindings::Reload();
            Session.WorldObjects=SavedObjects;Session.SelectedObject=SavedSelection;Session.Fellowship=SavedFellowship;Session.PlayerPosition=SavedPosition;
            Gameplay->HandleSelectionChanged(SavedSelection);Gameplay->RefreshSelectionOverlay();
        };
        CheckKeyboardTargets();
		{
			TGuardValue<TObjectPtr<UACEUIElementManager>> KeepManager(Client->UIElementManager,Manager);
			TGuardValue<TObjectPtr<UACEUILayoutResolver>> KeepLayout(Client->UILayoutResolver,Layout);
			TGuardValue<TObjectPtr<UACEUIResourceResolver>> KeepResources(Client->UIResourceResolver,Resources);
			Controller->DatCanvasWidget=Canvas;Controller->DatGameplayBinder=Gameplay;
			TFunction<void()> CheckHoverCursors = [&]()
			{
				const bool SavedLock=Manager->IsUiLocked();
				const bool SavedShow=Controller->bShowMouseCursor;
				const bool SavedHud=Controller->bUseDatDrivenHud;
				const auto SavedCursorType=Controller->CurrentMouseCursor;
				const FVector2D SavedCursor=FSlateApplication::Get().GetCursorPos();
				const auto Radar=Manager->FindElementByName(TEXT("RootGameplay_Radar_Field"));
				const FIntPoint SavedDrag(Radar->UserDragX,Radar->UserDragY);
				Controller->bShowMouseCursor=true;Controller->bUseDatDrivenHud=true;
				Controller->CurrentMouseCursor=EMouseCursor::Default;
				Manager->SetUiLocked(false);Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
				for (float Scale : {1.f,1.5f})
				{
					CaptureScreen(TEXT("RadarHoverSetup"),Scale);
					for (const TCHAR* Name : {TEXT("RadarDrag"),TEXT("InvTitleText"),TEXT("PanelLeftBorder")})
					{
						const auto Handle=Manager->FindElementByName(Name);
						Manager->BringFloatyToFront(Handle);
						const FGeometry Geo=Canvas->GetCachedGeometry();
						const FVector2D Point=Canvas->LayoutToViewport(FVector2D(Handle->GetScreenOrigin())+FVector2D(Handle->Width,Handle->Height)*.5);
						FSlateApplication::Get().SetCursorPos(Geo.LocalToAbsolute(Point));
						TestEqual(TEXT("Viewport shows move cursor on hover before clicking"),Controller->GetMouseCursor(),EMouseCursor::CardinalCross);
						Manager->SetUiLocked(true);
						TestEqual(TEXT("Locked HUD does not advertise movement"),Controller->GetMouseCursor(),EMouseCursor::Default);
						Manager->SetUiLocked(false);
						if (Handle->ElementName==TEXT("RadarDrag"))
						{
							const FIntPoint Before(Radar->UserDragX,Radar->UserDragY);
							FPointerEvent Down(0,Geo.LocalToAbsolute(Point),Geo.LocalToAbsolute(Point),{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState());
							TestTrue(TEXT("Canvas accepts radar handle press"),Canvas->NativeOnMouseButtonDown(Geo,Down).IsEventHandled());
							const FVector2D End=Point+Canvas->LayoutToViewport(FVector2D(-50,35))-Canvas->LayoutToViewport(FVector2D::ZeroVector);
							Manager->NotifyMouseMove(End,Geo.GetLocalSize());
							TestEqual(TEXT("Radar follows drag at each UI scale"),FIntPoint(Radar->UserDragX,Radar->UserDragY),Before+FIntPoint(-50,35));
							FPointerEvent Up(0,Geo.LocalToAbsolute(End),Geo.LocalToAbsolute(Point),{},EKeys::LeftMouseButton,0,FModifierKeysState());
							Canvas->NativeOnMouseButtonUp(Geo,Up);
						}
					}
					FSlateApplication::Get().SetCursorPos(Canvas->GetCachedGeometry().LocalToAbsolute(FVector2D(500,250)));
					TestEqual(TEXT("Leaving the frame restores the default cursor"),Controller->GetMouseCursor(),EMouseCursor::Default);
				}
				Radar->UserDragX=SavedDrag.X;Radar->UserDragY=SavedDrag.Y;Radar->RecomputeLayoutOffset();Manager->SaveFloatyLayout();
				Manager->SetUiLocked(SavedLock);Controller->bShowMouseCursor=SavedShow;Controller->bUseDatDrivenHud=SavedHud;
				Controller->CurrentMouseCursor=SavedCursorType;
				FSlateApplication::Get().SetCursorPos(SavedCursor);CaptureScreen(TEXT("RadarHoverRestored"));
			};
			CheckHoverCursors();
			Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
			const auto Frame=Manager->FindElementByName(TEXT("InventoryPanel_Field"));
			Controller->bGameplayUiAssetsReady=false;
			TestFalse(TEXT("Portal prefetch yields after building the texture queue"),Controller->TryPrefetchGameplayHudAssets());
			for (int32 Tick=0; Tick<1000 && !Controller->bGameplayUiAssetsReady; ++Tick)
				Controller->TryPrefetchGameplayHudAssets();
			TestTrue(TEXT("Portal prefetch resolves existing HUD resources across ticks"),Controller->bGameplayUiAssetsReady);
			TestTrue(TEXT("Portal prefetch preserves inventory frame identity and visibility"),Manager->FindElementByName(TEXT("InventoryPanel_Field"))==Frame && Frame->bVisible);
			Controller->SetDesktopInterfaceHidden(true);
			TestTrue(TEXT("Desktop interface can be hidden"),Controller->IsDesktopInterfaceHidden());
			TestEqual(TEXT("Hidden interface collapses the whole canvas"),Canvas->GetVisibility(),ESlateVisibility::Collapsed);
			TestTrue(TEXT("Hiding keeps the open inventory state intact"),Frame->bVisible && Gameplay->ActivePanelPage==TEXT("InventoryPanel_Field"));
			Controller->SetDesktopInterfaceHidden(false);
			TestEqual(TEXT("Showing restores the existing canvas"),Canvas->GetVisibility(),ESlateVisibility::Visible);
			TestTrue(TEXT("Showing preserves window identity"),Manager->FindElementByName(TEXT("InventoryPanel_Field"))==Frame);
			Controller->SetDesktopInterfaceHidden(true);
			Gameplay->ToggleKeyboardMappingUI();
			TestFalse(TEXT("Opening keybind capture reveals the hidden interface"),Controller->IsDesktopInterfaceHidden());
			Gameplay->ToggleKeyboardMappingUI();
			Controller->DatCanvasWidget=nullptr;Controller->DatGameplayBinder=nullptr;
		}
        FACEWorldObject Clicked;Clicked.Guid=456;Clicked.SetupId=0x02000001;Clicked.ItemType=ACEItemType::Creature;
        auto* ClickActor=World->SpawnActor<AACEWorldEntityActor>();ClickActor->InitializeFromObject(Clicked,100,true);
        FACESelectedObject FlashSelection;FlashSelection.Guid=456;FlashSelection.bValid=true;FlashSelection.SelectionSerial=10;
        Client->Session->SelectedObject=FlashSelection;
        Gameplay->HandleSelectionChanged(FlashSelection);
        TestTrue(TEXT("Selecting an actual creature creates brightness flash materials"),!Gameplay->FlashMaterials.IsEmpty());
        if (!Gameplay->FlashMaterials.IsEmpty())
        {
            auto* Flash=Gameplay->FlashMaterials[0].Get();
            TestFalse(TEXT("Selection flash has a renderer-compatible parent"),Flash->Parent->IsA<UMaterialInstanceDynamic>());
            UTexture* SourceTexture=nullptr;Gameplay->FlashOriginalMaterials[0]->GetTextureParameterValue(TEXT("Texture"),SourceTexture);
            TestTrue(TEXT("Selection flash preserves the character palette texture"),Flash->K2_GetTextureParameterValue(TEXT("Texture"))==SourceTexture);
        }
        const double FlashDeadline=Gameplay->SelectionFlashUntil;
        FlashSelection.HealthFraction=.5f;Client->Session->SelectedObject=FlashSelection;Gameplay->HandleSelectionChanged(FlashSelection);
        TestEqual(TEXT("Health updates do not retrigger the click brightness flash"),Gameplay->SelectionFlashUntil,FlashDeadline);
        Gameplay->SelectionFlashUntil=0;
        ++FlashSelection.SelectionSerial;Client->Session->SelectedObject=FlashSelection;Gameplay->HandleSelectionChanged(FlashSelection);
        TestTrue(TEXT("Clicking the same target again starts a fresh flash"),Gameplay->SelectionFlashUntil>0);
        Gameplay->SelectionFlashUntil=0;Gameplay->TickSelectionFlash();ClickActor->Destroy();
        // World-model selection flashes must never replace a live or pooled
        // particle's material. Its renderer copies/reuses that material between
        // clicks, which used to retain the flash's extra emissive strength.
        for(uint32 Setup : {0x02000F1Cu,0x02000896u})
        {
            FACEWorldObject Weapon;Weapon.Guid=int32(Setup);Weapon.SetupId=Setup;Weapon.ItemType=ACEItemType::Caster;
            auto* WeaponActor=World->SpawnActor<AACEWorldEntityActor>();WeaponActor->InitializeFromObject(Weapon,100,true);
            auto* Script=WeaponActor->ScriptComponent.Get();
            for(int Frame=0;Frame<20;++Frame)Script->TickComponent(1.f/60,LEVELTICK_All,nullptr);
            const int32 Emitters=Script->ActiveEmitters.Num();
            TestTrue(TEXT("Selection regression uses live authored weapon glows"),Emitters>0);
            FlashSelection.Guid=Weapon.Guid;
            for(int FlashClick=0;FlashClick<20;++FlashClick)
            {
                ++FlashSelection.SelectionSerial;Gameplay->HandleSelectionChanged(FlashSelection);
                for(auto Mesh:Gameplay->FlashMeshes)
                    TestTrue(TEXT("World selection only flashes model parts, never glow meshes"),WeaponActor->Appearance->PartMeshes.Contains(Cast<UProceduralMeshComponent>(Mesh.Get())));
                for(int Frame=0;Frame<3;++Frame)Script->TickComponent(1.f/60,LEVELTICK_All,nullptr);
            }
            TestEqual(TEXT("Repeated world selection retains exactly the authored emitter count"),Script->ActiveEmitters.Num(),Emitters);
            Gameplay->SelectionFlashUntil=0;Gameplay->TickSelectionFlash();WeaponActor->Destroy();
        }
        FACEContractUIFixture::Run(*this,PreviewDat,Gameplay,Manager,Canvas,Resources,Client->Session->Contracts,
            [&](const FString& Name,float Scale){CaptureScreen(Name,Scale);});
        Gameplay->JournalStorageRoot=FPaths::ProjectSavedDir()/TEXT("Automation/JournalFixture");
        Gameplay->HandleNamedClick(TEXT("JournalTab")); CaptureScreen(TEXT("GameplayJournal"));
        TestEqual(TEXT("Journal tab remains selected across refresh"),Gameplay->ActiveQuestTab,FString(TEXT("JournalPage")));
        TestTrue(TEXT("All three quest tab captions are visible"),Gameplay->QuestTabLabels.Num()==3 && Gameplay->QuestTabLabels[2]->GetVisibility()!=ESlateVisibility::Collapsed);
        Gameplay->JournalEntries[1]->SetText(FText::FromString(TEXT("Roof survey")));
        Gameplay->JournalNotes->SetText(FText::FromString(TEXT("Remember the Yaraq roofs.")));
        Gameplay->HandleJournalEdited(FText::GetEmpty());
        Gameplay->JournalFile.Reset(); Gameplay->RefreshQuestOverlays();
        TestEqual(TEXT("Journal notes survive a disk reload"),Gameplay->JournalNotes->GetText().ToString(),FString(TEXT("Remember the Yaraq roofs.")));
        const auto NotesSlate=Gameplay->JournalNotes->TakeWidget();
        NotesSlate->OnFocusReceived(Gameplay->JournalNotes->GetCachedGeometry(),FFocusEvent());
        NotesSlate->OnKeyChar(Gameplay->JournalNotes->GetCachedGeometry(),FCharacterEvent('!',FModifierKeysState(),0,false));
        TestEqual(TEXT("DAT journal editing updates the saved page immediately"),Gameplay->JournalPages[0].Notes,FString(TEXT("Remember the Yaraq roofs.!")));
        NotesSlate->OnKeyDown(Gameplay->JournalNotes->GetCachedGeometry(),FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));
        TestEqual(TEXT("Cancelling an edit restores the saved notes too"),Gameplay->JournalPages[0].Notes,FString(TEXT("Remember the Yaraq roofs.")));
        FString LongNotes;for(int32 Line=0;Line<60;++Line)LongNotes+=FString::Printf(TEXT("Journal line %d\n"),Line);
        Gameplay->JournalNotes->SetText(FText::FromString(LongNotes));CaptureScreen(TEXT("GameplayJournalLongNotes"));
        TestTrue(TEXT("Long DAT notes expose a scroll range"),Gameplay->JournalNotes->GetScrollOffsetOfEnd()>0);
        TestTrue(TEXT("Long journal text remains clipped after UMG property synchronization"),
            Gameplay->JournalNotes->GetCachedWidget()->GetClipping()==EWidgetClipping::ClipToBounds);
        const auto NotesDown=Manager->FindElementUnder(TEXT("NotesScrollbar"),TEXT("ScrollBar_Down"));
        Gameplay->OnElementActivated(NotesDown);
        TestTrue(TEXT("Authored journal scrollbar moves the editable text"),Gameplay->JournalNotes->GetScrollOffset()>0);
        Gameplay->JournalNotes->SetScrollOffset(Gameplay->JournalNotes->GetScrollOffsetOfEnd());CaptureScreen(TEXT("GameplayJournalNotesEnd"));
        TestTrue(TEXT("The last journal lines remain reachable"),FMath::IsNearlyEqual(Gameplay->JournalNotes->GetScrollOffset(),Gameplay->JournalNotes->GetScrollOffsetOfEnd()));
        Gameplay->JournalNotes->SetText(FText::FromString(TEXT("Remember the Yaraq roofs.")));
        Gameplay->HandleNamedClick(TEXT("PageListTab")); CaptureScreen(TEXT("GameplayJournalPages"));
        TestTrue(TEXT("Page list shows saved titles"),Gameplay->QuestRows[0]->GetText().ToString().Contains(TEXT("Roof survey")));
        Gameplay->JournalPages.SetNum(1); // Keep reruns independent of the saved sorting fixture below.
        for (int32 Page=1;Page<40;++Page) { auto& Note=Gameplay->JournalPages.AddDefaulted_GetRef(); Note.Title=FString::Printf(TEXT("Entry %d"),Page); }
        Gameplay->RefreshQuestOverlays(); Gameplay->ScrollQuestList(-1.f);
        TestTrue(TEXT("Page list scrolls to later saved pages"),Gameplay->JournalScrollOffset==3 && Gameplay->QuestRowIds[0]==-4);
        Gameplay->JournalSearch=TEXT("Entry 39"); Gameplay->RefreshQuestOverlays();
        TestTrue(TEXT("Journal search reaches pages beyond the visible rows"),Gameplay->JournalFilteredCount==1 && Gameplay->QuestRowIds[0]==-40);
        Gameplay->JournalSearch.Reset(); Gameplay->JournalPages.SetNum(1); Gameplay->JournalScrollOffset=0;
        Gameplay->JournalPages[0].Title=TEXT("Zebra");
        Gameplay->JournalPages.AddDefaulted_GetRef().Title=TEXT("Alpha");
        Gameplay->JournalPageIndex=0;
        Gameplay->HandleNamedClick(TEXT("TitleSortButton"));
        TestEqual(TEXT("Title sorting orders the visible list"),Gameplay->QuestRowIds[0],-2);
        TestEqual(TEXT("Title sorting preserves stored page numbers"),Gameplay->JournalPages[0].Title,FString(TEXT("Zebra")));
        TestEqual(TEXT("Sorting preserves the currently edited page"),Gameplay->JournalPageIndex,0);
        Gameplay->HandleNamedClick(TEXT("TitleSortButton"));
        TestEqual(TEXT("Repeated title click reverses the list"),Gameplay->QuestRowIds[0],-1);
        Gameplay->JournalPages[1].TimerEnd=double(FDateTime::UtcNow().ToUnixTimestamp())+3600;
        Gameplay->HandleNamedClick(TEXT("TimerSortButton"));
        TestEqual(TEXT("Retail timer sort shows running timers before stopped timers"),Gameplay->QuestRowIds[0],-2);
        Gameplay->HandleNamedClick(TEXT("PageSortButton"));
        TestEqual(TEXT("Page sort restores the original numbered order"),Gameplay->QuestRowIds[0],-1);
        Gameplay->JournalPages[0].Title=TEXT("A long journal title near its cap");
        Gameplay->JournalPages[0].Label=TEXT("Quest notes");
        Gameplay->JournalPages[0].TimerEnd=double(FDateTime::UtcNow().ToUnixTimestamp())-1;
        Gameplay->RefreshQuestOverlays();CaptureScreen(TEXT("GameplayJournalColumns"));
        const auto JournalRow=Gameplay->JournalRowElements[0];
        TestEqual(TEXT("Journal rows use the retail four-column template"),JournalRow->Children.Num(),4);
        TestEqual(TEXT("Selected journal row uses the retail selection artwork"),JournalRow->DefaultState,uint32(6));
        TestTrue(TEXT("Journal cells restore DAT ink instead of retaining parchment or contract colors"),
            Gameplay->QuestRows[0]->GetColorAndOpacity().GetSpecifiedColor().Equals(FLinearColor::White));
        TestTrue(TEXT("Journal page title stays inside its own column"),Gameplay->QuestRows[0]->GetCachedGeometry().GetLocalSize().X<=90);
        TestTrue(TEXT("Expired journal timers show retail Ready text"),Gameplay->JournalLabels.ContainsByPredicate([](const auto& Label){return Label && Label->GetVisibility()!=ESlateVisibility::Collapsed && Label->GetText().ToString()==TEXT("Ready");}));
        Gameplay->JournalPages.SetNum(1);
        Gameplay->HandleNamedClick(TEXT("ContractsTab")); Gameplay->RefreshQuestOverlays();
        TestFalse(TEXT("Journal row art hides when leaving the page list"),JournalRow->bVisible);
        TestEqual(TEXT("Returning to contracts restores its page"),Gameplay->ActiveQuestTab,FString(TEXT("ContractsPage")));
        Gameplay->ShowPanelPage(TEXT("SpellManagementPanel_Field"));
        Client->Session->KnownSpells={1,2,3,4,5,6,7,8,9};
        Gameplay->HandleNamedClick(TEXT("SpellbookTab"));Gameplay->TickRefresh();
        CaptureScreen(TEXT("GameplaySpellbook"));
        if (!Gameplay->SpellbookRowIds.IsEmpty())
        {
            const auto* SpellRowWidget=Gameplay->SpellbookRowBackgrounds[0].Get();
            const auto G=SpellRowWidget->GetCachedGeometry();
            Gameplay->TryBeginSpellDrag(Canvas->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*.5)));
            TestEqual(TEXT("Clicking a spellbook row selects its spell"),Gameplay->SelectedSpellbookId,Gameplay->SpellbookRowIds[0]);
            const auto Template=UACEUILayoutResolver::LoadTemplate(0x21000037,0x10000343);
            const auto* Selected=Template->Children.FindByPredicate([](const auto& C){return C->ElementName==TEXT("ItemSlot_Icon_Selected");});
            TestTrue(TEXT("Selected spellbook row uses authored highlight"),Selected && Gameplay->SpellbookRowSelections[0]->Background.GetResourceObject()==Resources->ResolveIconTexture((*Selected)->ImageFileId));
            Gameplay->CancelSpellDrag();
            CaptureScreen(TEXT("GameplaySpellbookSelected"));
        }

        Gameplay->bComponentsShowCarriedOnly=false; // Stress the scrollbar with the full DAT catalog.
        Gameplay->HandleNamedClick(TEXT("SpellComponentTab"));Gameplay->TickRefresh();
        CaptureScreen(TEXT("GameplayComponents"));
        TestTrue(TEXT("Component window populates from the DAT component table"),Gameplay->ComponentRowWcids.ContainsByPredicate([](int32 Wcid){return Wcid>0;}));
        if (const auto CompBar=Manager->FindElementUnder(TEXT("SpellComponentPage"),TEXT("SpellComponents_ComponentList_Scrollbar")))
        {
            const FVector2D Grab=FVector2D(CompBar->GetScreenOrigin())+FVector2D(8,24);
            TestTrue(TEXT("Components scrollbar accepts a drag"),Gameplay->TryBeginScrollbarDrag(Grab));
            Gameplay->UpdateScrollbarDrag(Grab+FVector2D(0,180));
            const float DragFraction=Gameplay->ScrollDragFraction;
            TestTrue(TEXT("Dragging components reaches later entries"),Gameplay->ComponentScrollOffset>0);
            Gameplay->RefreshComponentOverlays(); Gameplay->UpdateScrollbarDrag(Grab+FVector2D(0,180));
            TestEqual(TEXT("Content refresh does not make the scrollbar jump"),Gameplay->ScrollDragFraction,DragFraction);
            Gameplay->TryFinishScrollbarDrag();
            CaptureScreen(TEXT("GameplayComponentsScrolled"));
        }
        // September 11 follow-up: real editable DAT glyphs, refill accounting and command packets.
        auto& Session=*Client->Session;
        // A fresh loopback sender keeps these commands isolated from game servers.
        FSocket* ComponentReceiver=Sockets->CreateSocket(NAME_DGram,TEXT("Component fixture receiver"),false);
        auto ComponentAddress=Sockets->CreateInternetAddr();ComponentAddress->SetIp(TEXT("127.0.0.1"),ValidAddress);ComponentAddress->SetPort(0);
        if (!ComponentReceiver || !ComponentReceiver->Bind(*ComponentAddress)) return false;
        ComponentReceiver->GetAddress(*ComponentAddress);
        Session.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("Component fixture sender"),false);
        Session.ServerC2SAddr=ComponentAddress;
        auto HasAction=[&](uint32 Expected)
        {
            for (const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
                if (Wire.ReadUInt32()!=ACEOpcode::GameAction) continue;
                Wire.ReadUInt32(); if (Wire.ReadUInt32()==Expected) return true;
            }
            return false;
        };
        // Retail Give (CPlayerSystem action 0x10000040) uses the selection
        // preceding the inventory item, not an extra Use-target click.
        {
            TGuardValue<int32> KeepPrevious(Session.PreviousSelectedObjectGuid,Session.PreviousSelectedObjectGuid);
            TGuardValue<int32> KeepRecipient(Session.GiveRecipientGuid,Session.GiveRecipientGuid);
            TGuardValue<uint32> KeepOptions(Session.CharacterOptions1,Session.CharacterOptions1 & ~0x04000000u);
            const auto SavedSelection=Session.SelectedObject;
            FACEWorldObject Gift;Gift.Guid=99110;Gift.Name=TEXT("Give stack");Gift.ItemType=ACEItemType::Misc;
            Gift.ContainerId=Session.PlayerGuid;Gift.StackSize=50;Gift.MaxStackSize=100;
            FACEWorldObject Recipient;Recipient.Guid=99111;Recipient.Name=TEXT("Quest collector");
            Recipient.ItemType=ACEItemType::Creature;Recipient.bHasPosition=true;
            Recipient.Position=Session.PlayerPosition;Recipient.Position.Location.X+=10;
            Session.WorldObjects.Add(Gift.Guid,Gift);Session.WorldObjects.Add(Recipient.Guid,Recipient);
            Session.ClearPendingUse();Gameplay->CancelPendingUseWith();
            ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();
            ACEInputBindings::Set(ACEInputBindings::Action(TEXT("SelectionGive")),0,FInputChord(EKeys::F7));ACEInputBindings::Commit();
            Controller->PlayerInput=NewObject<UPlayerInput>(Controller);
            auto Select=[&](int32 Guid)
            {
                Client->SelectObject(Guid);Gameplay->HandleSelectionChanged(Client->GetSelectedObject());
            };
            auto Give=[&]()
            {
                Controller->ClearServerMoveTo();Session.CachedC2SPackets.Reset();
                Controller->PlayerInput->FlushPressedKeys();
                Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::F7,IE_Pressed,1.f));
                Controller->PlayerInput->ProcessInputStack({},.016f,false);
                Gameplay->PollAdditionalKeyboardActions(Controller);
                TestEqual(TEXT("Give never leaves an item awaiting a Use target"),Gameplay->GetPendingUseWithSourceGuid(),0);
                TestFalse(TEXT("Give leaves the mouse out of Use-target mode"),Controller->bPendingUseTargeting);
                TestFalse(TEXT("Give never sends Use or UseWithTarget"),HasAction(ACEGameAction::Use)||HasAction(ACEGameAction::UseWithTarget));
            };
            Select(Recipient.Guid);Select(Gift.Guid);Gameplay->SelectedStackAmount=7;
            Select(Gift.Guid);
            TestEqual(TEXT("Reselecting an item preserves its recipient"),Client->GetPreviousSelectedObjectGuid(),Recipient.Guid);
            Give();
            TestTrue(TEXT("Rebound Give sends directly to the existing recipient"),HasAction(ACEGameAction::GiveObjectRequest));
            int32 GiveCount=0;
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader Wire(Packet.Value.Payload);Wire.Skip(16);
                if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;
                Wire.ReadUInt32();if(Wire.ReadUInt32()!=ACEGameAction::GiveObjectRequest)continue;
                ++GiveCount;
                TestEqual(TEXT("Give wire recipient is the previous selection"),int32(Wire.ReadUInt32()),Recipient.Guid);
                TestEqual(TEXT("Give wire source is the inventory item"),int32(Wire.ReadUInt32()),Gift.Guid);
                TestEqual(TEXT("Give wire preserves the chosen partial stack"),Wire.ReadInt32(),7);
            }
            TestEqual(TEXT("One Give press emits one handoff"),GiveCount,1);
            TestEqual(TEXT("Give returns selection to the recipient like retail"),Client->GetSelectedObject().Guid,Recipient.Guid);
            Select(Gift.Guid);Give();
            TestTrue(TEXT("Selecting another gift reuses the returned recipient"),HasAction(ACEGameAction::GiveObjectRequest));

            FACEWorldObject Pack;Pack.Guid=99112;Pack.ContainerId=Session.PlayerGuid;Pack.ItemType=ACEItemType::Container;
            FACEWorldObject OtherGift=Gift;OtherGift.Guid=99113;OtherGift.ContainerId=Pack.Guid;
            Session.WorldObjects.Add(Pack.Guid,Pack);Session.WorldObjects.Add(OtherGift.Guid,OtherGift);
            Select(Recipient.Guid);Gameplay->SelectInventoryGuid(Session.PlayerGuid);
            Gameplay->SelectInventoryGuid(Pack.Guid);Select(OtherGift.Guid);Select(Gift.Guid);
            TestEqual(TEXT("Browsing main pack, backpack and other items preserves Give recipient"),Client->GetGiveRecipientGuid(),Recipient.Guid);
            TestEqual(TEXT("Ordinary previous selection still tracks the actual inventory selection"),Client->GetPreviousSelectedObjectGuid(),OtherGift.Guid);
            Give();TestTrue(TEXT("Give works after browsing inventory"),HasAction(ACEGameAction::GiveObjectRequest));
            // Opening a shop selects its first merchandise entry automatically.
            // That selection must not steal the merchant from the Give key.
            {
                TGuardValue<TArray<FACEWorldObject>> KeepStock(Session.VendorMerchandise,Session.VendorMerchandise);
                TGuardValue<uint32> KeepTypes(Session.VendorItemTypes,MAX_uint32);
                TGuardValue<int32> KeepMin(Session.VendorMinValue,-1),KeepMax(Session.VendorMaxValue,-1);
                auto Merchant=Recipient;Merchant.ObjectDescriptionFlags|=ACEObjectDescFlag::Vendor;
                auto Stock=OtherGift;Stock.ContainerId=Merchant.Guid;
                Session.WorldObjects[Merchant.Guid]=Merchant;Session.WorldObjects[Stock.Guid]=Stock;
                Session.WorldObjects[Gift.Guid].Value=100;Session.VendorMerchandise={Stock};
                Select(Merchant.Guid);Gameplay->ShowVendorPanel(Merchant.Guid);
                TestEqual(TEXT("Shop still selects its first stock item"),Client->GetSelectedObject().Guid,Stock.Guid);
                Select(Gift.Guid);Gameplay->SelectedStackAmount=Gift.StackSize;Give();
                TestEqual(TEXT("Shop browsing retains the merchant"),Client->GetGiveRecipientGuid(),Merchant.Guid);
                TestTrue(TEXT("Give adds the carried item to the selected merchant's sell cart"),Gameplay->VendorSellCart.ContainsByPredicate([&](const auto& Entry){return Entry.Value==Gift.Guid&&Entry.Key==Gift.StackSize;}));
                TestFalse(TEXT("Merchant Give follows retail selling instead of sending an NPC gift"),HasAction(ACEGameAction::GiveObjectRequest));
                Gameplay->HideVendorPanel();Session.WorldObjects[Recipient.Guid]=Recipient;
            }
            Session.WorldObjects.Remove(Pack.Guid);Session.WorldObjects.Remove(OtherGift.Guid);

            Select(0);Select(Gift.Guid);Give();
            TestFalse(TEXT("Missing recipient sends no Give"),HasAction(ACEGameAction::GiveObjectRequest));
            TestEqual(TEXT("Missing recipient keeps the item selected"),Client->GetSelectedObject().Guid,Gift.Guid);
            TestTrue(TEXT("Missing recipient shows a target prompt"),Gameplay->TransientInfoText &&
                Gameplay->TransientInfoText->GetText().ToString().Contains(TEXT("select a creature or a character")));
            Select(Recipient.Guid);Select(Gift.Guid);Session.DeleteWorldObject(Recipient.Guid);
            TestEqual(TEXT("A removed recipient is forgotten while an item is selected"),Client->GetGiveRecipientGuid(),0);
            Session.WorldObjects.Add(Recipient.Guid,Recipient);Give();
            TestFalse(TEXT("Reusing a deleted recipient GUID requires a new selection"),HasAction(ACEGameAction::GiveObjectRequest));
            for(int32 TargetGuid:{0,Session.PlayerGuid,99112})
            {
                Select(TargetGuid);Select(Gift.Guid);Give();
                TestFalse(TEXT("Cleared, self or vanished recipients cannot receive a Give"),HasAction(ACEGameAction::GiveObjectRequest));
            }
            Recipient.ItemType=0;Recipient.ObjectDescriptionFlags=ACEObjectDescFlag::Door;
            Session.WorldObjects.Add(Recipient.Guid,Recipient);Select(Recipient.Guid);Select(Gift.Guid);Give();
            TestFalse(TEXT("Door selection cannot become a Give recipient"),HasAction(ACEGameAction::GiveObjectRequest));
            Recipient.ObjectDescriptionFlags=ACEObjectDescFlag::Stuck;Recipient.RadarBlipColor=1;
            Session.WorldObjects.Add(Recipient.Guid,Recipient);Select(Recipient.Guid);Select(Gift.Guid);Give();
            TestTrue(TEXT("Custom collector properties support Give without a hardcoded model/type"),HasAction(ACEGameAction::GiveObjectRequest));
            Select(Gift.Guid);Session.PendingEquipmentGuid=Gift.Guid;Give();
            TestFalse(TEXT("Give still respects an active inventory transaction"),HasAction(ACEGameAction::GiveObjectRequest));
            Session.CancelEquipmentSwap();
            Session.TradeSelfItems.Add(Gift.Guid);Give();
            TestFalse(TEXT("Items already in trade cannot also be given"),HasAction(ACEGameAction::GiveObjectRequest));
            Session.TradeSelfItems.Remove(Gift.Guid);
            Recipient.bIsPlayer=true;Session.WorldObjects.Add(Recipient.Guid,Recipient);
            Select(Recipient.Guid);Select(Gift.Guid);Give();
            TestTrue(TEXT("Direct player gifts use the same Give path"),HasAction(ACEGameAction::GiveObjectRequest));
            Session.CharacterOptions1|=0x04000000u;Select(Gift.Guid);Gameplay->SelectedStackAmount=3;Give();
            TestTrue(TEXT("Give respects the player's secure-trade option"),HasAction(ACEGameAction::OpenTradeNegotiations));
            TestFalse(TEXT("Secure-trade preference prevents an immediate gift"),HasAction(ACEGameAction::GiveObjectRequest));
            TestEqual(TEXT("Secure trade retains the chosen stack quantity"),Gameplay->PendingTradeItemAmount,3);
            Gameplay->PendingTradeItemGuid=Gameplay->PendingTradeItemPartner=0;
            Controller->ClearServerMoveTo();Controller->PlayerInput->FlushPressedKeys();
            Session.WorldObjects.Remove(Gift.Guid);Session.WorldObjects.Remove(Recipient.Guid);
            Session.SelectedObject=SavedSelection;Gameplay->HandleSelectionChanged(SavedSelection);
            ACEInputBindings::Reload();Session.CachedC2SPackets.Reset();
        }
        Gameplay->bComponentsShowCarriedOnly=true;
        const auto& Component=Dat->GetSpellComponents()[0];
        const auto SavedDesired=Session.DesiredComponents;
        Session.DesiredComponents.Reset(); Session.DesiredComponents.Add(Component.Wcid,100);
        FACEWorldObject Comp;Comp.Guid=99120;Comp.WeenieClassId=Component.Wcid;Comp.IconId=Component.IconDid;
        Comp.Name=Component.Name;Comp.ItemType=0x1000;Comp.StackSize=40;Comp.MaxStackSize=1000;Comp.ContainerId=Player.Guid;
        Session.WorldObjects.Add(Comp.Guid,Comp);
        Gameplay->ComponentScrollOffset=0; Gameplay->RefreshComponentOverlays(); CaptureScreen(TEXT("GameplayComponentsRetail"));
        auto EntryPtr=Gameplay->ComponentDesiredEntries.FindByPredicate([&](const auto& E){return E && E->ContextId==int32(Component.Wcid) && E->GetVisibility()!=ESlateVisibility::Collapsed;});
        if (TestNotNull(TEXT("Component target is an editable field"),EntryPtr))
        {
            auto* Entry=EntryPtr->Get();
            auto InputWindow=SNew(SVirtualWindow).Size(FVector2D(ScreenSize));
            InputWindow->SetIsFocusable(true);InputWindow->SetContent(Slate);
            FSlateApplication::Get().RegisterVirtualWindow(InputWindow);
            {
                TGuardValue<uint32> Options(Session.CharacterOptions1,Session.CharacterOptions1 & ~0x00000800u);
                TGuardValue<TObjectPtr<UACEUIGameplayBinder>> ChatBinder(Controller->DatGameplayBinder,Gameplay);
                TGuardValue<TObjectPtr<UACEUICanvasWidget>> ChatCanvas(Controller->DatCanvasWidget,Canvas);
                auto& App=FSlateApplication::Get();
                Gameplay->ChatEntry->SetText(FText::GetEmpty());
                for (int32 Cycle=0; Cycle<4; ++Cycle)
                {
                    App.ClearKeyboardFocus();
                    TestTrue(TEXT("Every fresh Enter press opens chat without a camera tick or key-up"),
                        Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Enter,IE_Pressed,1.f)));
                    TestTrue(TEXT("Enter focuses the editable chat field"),Gameplay->IsChatEntryFocused());
                    for (TCHAR C:FString(TEXT("focus regression")))
                        App.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
                    App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
                    TestTrue(TEXT("Enter sends and clears the current chat draft"),Gameplay->ChatEntry->GetText().IsEmpty());
                    TestTrue(TEXT("Sending schedules restoration of gameplay keyboard focus"),Gameplay->bPendingChatRefocus);
                    TestEqual(TEXT("Default chat mode returns to gameplay"),Gameplay->PendingChatRefocusWindow,INDEX_NONE);
                    Gameplay->bPendingChatRefocus=false;
                }
                App.ClearKeyboardFocus();
                TestTrue(TEXT("Slash opens chat from gameplay"),Controller->InputKey(
                    FInputKeyEventArgs::CreateSimulated(EKeys::Slash,IE_Pressed,1.f)));
                for (TCHAR C:FString(TEXT("/help")))
                    App.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
                TestEqual(TEXT("Slash command contains exactly one prefix"),Gameplay->ChatEntry->GetText().ToString(),FString(TEXT("/help")));
                Gameplay->ChatEntry->SetText(FText::GetEmpty()); App.ClearKeyboardFocus();
                TestTrue(TEXT("Slash also opens chat from focused UI chrome"),Canvas->NativeOnPreviewKeyDown(
                    Canvas->GetCachedGeometry(),FKeyEvent(EKeys::Slash,FModifierKeysState(),0,false,0,0)).IsEventHandled());
                App.ClearKeyboardFocus();
            }
            Session.CachedC2SPackets.Reset(); Entry->SetText(FText::GetEmpty());
            FSlateApplication::Get().SetKeyboardFocus(Entry->TakeWidget());
            for (TCHAR C:FString(TEXT("175"))) FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
            TestEqual(TEXT("Component field accepts routed keyboard characters"),Entry->GetText().ToString(),FString(TEXT("175")));
            TestTrue(TEXT("Component focus suppresses world keyboard actions"),Gameplay->IsChatEntryFocused());
            CaptureScreen(TEXT("GameplayComponentEditing"));
            FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
            TestEqual(TEXT("Typed target saves exact quantity"),Session.DesiredComponents.FindRef(Component.Wcid),175);
            TestTrue(TEXT("Typed component target sends retail action"),HasAction(ACEGameAction::SetDesiredComponentLevel));
            FSlateApplication::Get().SetKeyboardFocus(Entry->TakeWidget());
            FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent('9',FModifierKeysState(),0,false));
            FSlateApplication::Get().ProcessKeyDownEvent(Escape);
            TestEqual(TEXT("Escape cancels the edit without closing Components"),Gameplay->ActivePanelPage,FString(TEXT("SpellManagementPanel_Field")));
            TestEqual(TEXT("Escape restores the original quantity text"),Entry->GetText().ToString(),FString(TEXT("175")));
            FSlateApplication::Get().UnregisterVirtualWindow(InputWindow);
            Entry->SetText(FText::FromString(TEXT("99999")));Entry->Commit(ETextCommit::OnEnter);
            TestEqual(TEXT("Retail target limit is 5000"),Session.DesiredComponents.FindRef(Component.Wcid),5000);
            Entry->SetText(FText::FromString(TEXT("2")));Entry->Commit(ETextCommit::OnCleared);
            TestEqual(TEXT("Cancel does not alter the saved target"),Session.DesiredComponents.FindRef(Component.Wcid),5000);
        }
        #include "ACERetailStackUIFixture.inl"
        // VendorSellUI splits a partial stack before sending a sale: the amount
        // field alone does not stop ACE/GDLE selling the source's entire object.
        [&]()
        {
            const int32 OldVendor=Gameplay->OpenVendorGuid, OldSessionVendor=Session.OpenVendorGuid;
            // This fixture constructs its own session; mirror the live
            // selection bridge before exercising the editable stack quantity.
            const auto SelectionBridge=Session.OnSelectionChanged.AddLambda([&](const FACESelectedObject& Selection){Client->OnSelectionChanged.Broadcast(Selection);});
            ON_SCOPE_EXIT{Session.OnSelectionChanged.Remove(SelectionBridge);};
            const auto OldSellCart=Gameplay->VendorSellCart;
            const auto OldSelection=Session.SelectedObject;
            FACEWorldObject Notes;Notes.Guid=99301;Notes.WeenieClassId=20630;
            Notes.Name=TEXT("Trade Note (250,000)");Notes.ItemType=ACEItemType::PromissoryNote;
            Notes.StackSize=250;Notes.MaxStackSize=250;Notes.Value=62500000;Notes.ContainerId=Player.Guid;
            FACEWorldObject Existing=Notes;Existing.Guid=99302;Existing.StackSize=1;Existing.Value=250000;
            Session.WorldObjects.Add(Notes.Guid,Notes);Session.WorldObjects.Add(Existing.Guid,Existing);
            Session.OpenVendorGuid=99300;Gameplay->ShowVendorPanel(99300);
            Gameplay->VendorSellCart.Reset();Gameplay->SelectInventoryGuid(Notes.Guid);
            Gameplay->StackAmountEntryGuid=Notes.Guid;
            Gameplay->HandleStackAmountCommitted(FText::FromString(TEXT("1")),ETextCommit::OnEnter);
            Gameplay->SelectInventoryGuid(Notes.Guid);
            TestEqual(TEXT("Grabbing the selected MMD preserves the one-unit split"),Gameplay->SelectedStackAmount,1);
            Gameplay->SetVendorPage(2);CaptureScreen(TEXT("VendorPartialStack"));
            const auto SellList=Manager->FindElementUnder(TEXT("VendorSellPage"),TEXT("VendorSellList"));
            if(TestTrue(TEXT("MMD sell drop target exists"),SellList.IsValid()))
            {
                Session.CachedC2SPackets.Reset();
                Gameplay->InvDragGuid=Notes.Guid;Gameplay->InvDragSourcePack=Player.Guid;
                Gameplay->InvDragShortcutSlot=INDEX_NONE;Gameplay->bInvDragPending=true;Gameplay->bInvDragActive=true;
                Gameplay->TryFinishInventoryDrag(FVector2D(SellList->GetScreenOrigin())+FVector2D(15,15));
                TestTrue(TEXT("Dropping one MMD requests a real stack split"),HasAction(ACEGameAction::StackableSplitToContainer));
                TestTrue(TEXT("Original 250-MMD stack is not offered while splitting"),Gameplay->VendorSellCart.IsEmpty());
                TestEqual(TEXT("Pending split records exactly one MMD"),Gameplay->VendorSellSplit.Amount,1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader Wire(Packet.Value.Payload);Wire.Skip(16);
                    if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;
                    Wire.ReadUInt32();if(Wire.ReadUInt32()!=ACEGameAction::StackableSplitToContainer)continue;
                    TestEqual(TEXT("Split packet names original stack"),Wire.ReadUInt32(),uint32(Notes.Guid));
                    TestEqual(TEXT("Split stays in the source's backpack"),Wire.ReadUInt32(),uint32(Player.Guid));
                    Wire.ReadInt32();TestEqual(TEXT("Split packet requests one MMD"),Wire.ReadInt32(),1);
                }
                const int32 Requests=Session.CachedC2SPackets.Num();
                Gameplay->AddInventoryGuidToVendorSellCart(Notes.Guid);Gameplay->SellVendorCart(false);
                TestEqual(TEXT("Repeated drop/sell while waiting sends no duplicate action"),Session.CachedC2SPackets.Num(),Requests);
                FACEWorldObject Created=Existing;Created.Guid=99303;
                Session.WorldObjects.Add(Created.Guid,Created);Gameplay->UpdateVendorSellSplit();
                TestTrue(TEXT("New object alone cannot complete split before source update"),Gameplay->VendorSellCart.IsEmpty());
                Session.WorldObjects[Notes.Guid].StackSize=249;Gameplay->UpdateVendorSellSplit();
                TestEqual(TEXT("Confirmed split creates one sell row"),Gameplay->VendorSellCart.Num(),1);
                if(Gameplay->VendorSellCart.Num()==1)
                {
                    TestEqual(TEXT("Sell row uses new object, not source or pre-existing MMD"),Gameplay->VendorSellCart[0].Value,Created.Guid);
                    TestEqual(TEXT("Sell row contains exactly one MMD"),Gameplay->VendorSellCart[0].Key,1);
                }
                TestFalse(TEXT("Dropping a split never sells automatically"),HasAction(ACEGameAction::Sell));
                Session.CachedC2SPackets.Reset();Gameplay->SellVendorCart(true);
                TestTrue(TEXT("Sell sends the confirmed MMD"),HasAction(ACEGameAction::Sell));
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader Wire(Packet.Value.Payload);Wire.Skip(16);
                    if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;
                    Wire.ReadUInt32();if(Wire.ReadUInt32()!=ACEGameAction::Sell)continue;
                    TestEqual(TEXT("Sale addresses current vendor"),Wire.ReadUInt32(),99300u);
                    TestEqual(TEXT("Sale contains one object"),Wire.ReadUInt32(),1u);
                    TestEqual(TEXT("Sale contains one MMD"),Wire.ReadInt32(),1);
                    TestEqual(TEXT("Sale uses the new split GUID"),Wire.ReadUInt32(),uint32(Created.Guid));
                }
                TestEqual(TEXT("Original stack retains the other 249 MMDs"),Session.WorldObjects[Notes.Guid].StackSize,249);
                Session.CachedC2SPackets.Reset();Session.SendSellItems(99300,{{1,Notes.Guid}});
                TestFalse(TEXT("Protocol guard rejects partial quantity with unsplit source GUID"),HasAction(ACEGameAction::Sell));
                Gameplay->AddInventoryGuidToVendorSellCart(Notes.Guid,249);
                TestFalse(TEXT("Whole stack needs no split"),HasAction(ACEGameAction::StackableSplitToContainer));
                Gameplay->SellVendorCart(false);
                TestTrue(TEXT("Whole-stack Sell All remains supported"),HasAction(ACEGameAction::Sell));

                // Reverse the reply order; unrelated matches must not be selected.
                Session.WorldObjects[Notes.Guid]=Notes;Gameplay->VendorSellCart.Reset();
                Gameplay->AddInventoryGuidToVendorSellCart(Notes.Guid,2);
                Session.WorldObjects[Notes.Guid].StackSize=248;Gameplay->UpdateVendorSellSplit();
                TestTrue(TEXT("Source-only reply waits for the new stack"),Gameplay->VendorSellCart.IsEmpty());
                Created.Guid=99304;Created.StackSize=2;Created.Value=500000;Created.ContainerId=0;
                Session.WorldObjects.Add(Created.Guid,Created);Gameplay->UpdateVendorSellSplit();
                TestTrue(TEXT("Uncontained new object waits for containment"),Gameplay->VendorSellCart.IsEmpty());
                Session.WorldObjects[Created.Guid].ContainerId=Player.Guid;Gameplay->UpdateVendorSellSplit();
                TestTrue(TEXT("Containment completes source-first split"),Gameplay->VendorSellCart.ContainsByPredicate([&](auto& P){return P.Value==Created.Guid&&P.Key==2;}));

                // Adjusting the quantity after adding a whole stack uses the same
                // split handshake before honoring the explicit Sell button press.
                Gameplay->VendorSellCart.Reset();Session.WorldObjects[Notes.Guid]=Notes;
                Gameplay->AddInventoryGuidToVendorSellCart(Notes.Guid,250);
                Gameplay->SelectInventoryGuid(Notes.Guid);Gameplay->SelectedStackAmount=3;
                Session.CachedC2SPackets.Reset();Gameplay->SellVendorCart(true);
                TestTrue(TEXT("Selected partial sale waits for its split"),Gameplay->VendorSellSplit.bSellWhenReady);
                TestFalse(TEXT("Selected partial sale cannot send the original stack"),HasAction(ACEGameAction::Sell));
                Session.WorldObjects[Notes.Guid].StackSize=247;
                Created.Guid=99305;Created.StackSize=3;Created.ContainerId=Player.Guid;
                Session.WorldObjects.Add(Created.Guid,Created);Gameplay->UpdateVendorSellSplit();
                TestTrue(TEXT("Confirmed selected sale resumes once"),HasAction(ACEGameAction::Sell));
                const int32 SentSaleCount=Session.CachedC2SPackets.Num();Gameplay->UpdateVendorSellSplit();
                TestEqual(TEXT("Repeated refresh cannot repeat sale"),Session.CachedC2SPackets.Num(),SentSaleCount);
                TestTrue(TEXT("Completed selected sale removes its cart entry"),Gameplay->VendorSellCart.IsEmpty());

                Session.WorldObjects[Notes.Guid]=Notes;Gameplay->AddInventoryGuidToVendorSellCart(Notes.Guid,1);
                Gameplay->HandleNamedClick(TEXT("VendorSellClearAll_Button"));
                TestEqual(TEXT("Clear All cancels pending staging"),Gameplay->VendorSellSplit.SourceGuid,0);
                Gameplay->VendorSellCart.Reset();Session.WorldObjects[Notes.Guid]=Notes;
                Gameplay->AddInventoryGuidToVendorSellCart(Notes.Guid,1);
                Gameplay->VendorSellSplit.Deadline=0;Gameplay->UpdateVendorSellSplit();
                TestEqual(TEXT("Unconfirmed split expires safely"),Gameplay->VendorSellSplit.SourceGuid,0);
                TestTrue(TEXT("Failed split never offers original stack"),Gameplay->VendorSellCart.IsEmpty());
                Gameplay->AddInventoryGuidToVendorSellCart(Notes.Guid,1);Gameplay->HideVendorPanel();
                TestEqual(TEXT("Closing vendor cancels pending split staging"),Gameplay->VendorSellSplit.SourceGuid,0);
            }
            Gameplay->VendorSellSplit={};Gameplay->OpenVendorGuid=OldVendor;Session.OpenVendorGuid=OldSessionVendor;
            Gameplay->VendorSellCart=OldSellCart;Session.SelectedObject=OldSelection;Gameplay->HandleSelectionChanged(OldSelection);
            for(int32 Guid:{99301,99302,99303,99304,99305})Session.WorldObjects.Remove(Guid);
        }();
        FACERetailVendorUIFixture::CheckAll(*this,Session,Client,Gameplay,Canvas,Manager,Resources,Player,
            Comp,Component.Wcid,SavedDesired,[&](const FString& Name,float Scale){CaptureScreen(Name,Scale);},
            [&](const TCHAR* Name){NativeClick(Name);},HasAction);
        for (const auto& Command : TArray<TPair<FString,uint32>>{{TEXT("/age"),ACEGameAction::QueryAge},{TEXT("@birth"),ACEGameAction::QueryBirth},{TEXT("/pkl"),ACEGameAction::EnterPkLite},{TEXT("/pklite"),ACEGameAction::EnterPkLite}})
        {
            Session.CachedC2SPackets.Reset();Gameplay->TryDispatchChatCommand(Command.Key);
            TestTrue(TEXT("Misc command sends its dedicated protocol action"),HasAction(Command.Value));
            TestFalse(TEXT("Misc command does not leak into public chat"),HasAction(ACEGameAction::Talk));
        }
        FString AgeReply;
        const auto AgeHandle=Session.OnChatMessage.AddLambda([&](const FString& T,const FString&,int32){AgeReply=T;});
        FACEBinaryWriter Age;Age.WriteUInt32(Player.Guid);Age.WriteUInt32(1);Age.WriteUInt32(ACEGameEvent::QueryAgeResponse);
        Age.WriteString16L(TEXT(""));Age.WriteString16L(TEXT("5d 3h 13m 1s"));FACEBinaryReader AgeReader(Age.GetData());Session.HandleGameEvent(AgeReader);
        TestTrue(TEXT("Server age response reaches displayed chat"),AgeReply.Contains(TEXT("5d 3h 13m 1s")));Session.OnChatMessage.Remove(AgeHandle);
        const FString CommandPath=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/Misc command fixture.txt"));
        const FString MirrorPath=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/Misc command output.txt"));
        const bool SavedLogging=Gameplay->bChatMirrorToFile;
        const FString SavedMirror=Gameplay->ChatMirrorFilePath;
        FFileHelper::SaveStringToFile(TEXT("/help %DATE%\n/version\n"),*CommandPath);
        FFileHelper::SaveStringToFile(TEXT("existing log line\n"),*MirrorPath);
        Gameplay->TryDispatchChatCommand(TEXT("/log \"")+MirrorPath+TEXT("\""));
        Session.CachedC2SPackets.Reset();
        Gameplay->TryDispatchChatCommand(TEXT("/loadfile \"")+CommandPath+TEXT("\""));
        Gameplay->TryDispatchChatCommand(TEXT("/help fillcomps"));
        Gameplay->TryDispatchChatCommand(TEXT("/log"));
        TestFalse(TEXT("Log without a name stops file output"),Gameplay->bChatMirrorToFile);
        FString Mirrored;FFileHelper::LoadFileToString(Mirrored,*MirrorPath);
        TestTrue(TEXT("Log appends instead of replacing an existing file"),Mirrored.StartsWith(TEXT("existing log line")));
        TestTrue(TEXT("Command files expand the retail date and weekday"),Mirrored.Contains(FDateTime::Now().ToFormattedString(TEXT("%Y-%m-%d-%a"))));
        TestTrue(TEXT("Command files execute local commands"),Mirrored.Contains(PLATFORM_ANDROID ? TEXT("AC:VR") : TEXT("AC:Unreal")));
        TestTrue(TEXT("Refill help explains the vendor buy list"),Mirrored.Contains(TEXT("choose Buy All")));
        TestFalse(TEXT("Local command files and help do not send public chat"),HasAction(ACEGameAction::Talk));
        Gameplay->bChatMirrorToFile=SavedLogging;Gameplay->ChatMirrorFilePath=SavedMirror;
        const auto SavedVitals=Session.PlayerVitals;
        Session.PlayerVitals.bValid=true;Session.PlayerVitals.AgeSeconds=443581;Session.PlayerVitals.NumDeaths=6;
        Session.PlayerVitals.StatQualityInts.Add(98,1698189042);Session.PlayerVitals.StatQualityInts.Add(354,6);
        Session.PlayerVitals.StatQualityInts.Add(355,8);Session.PlayerVitals.StatQualityInts.Add(362,3);
        Session.PlayerVitals.StatQualityInts.Add(294,1);Session.PlayerVitals.StatQualityInts.Add(326,1);
        Session.PlayerVitals.ChessRank=0;
        for (int32 Property : {233,238,309,310,299}) Session.PlayerVitals.StatQualityInts.Add(Property,1);
        Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));Gameplay->HandleNamedClick(TEXT("BurdenIndicator"));Gameplay->TickRefresh();
        TestEqual(TEXT("Small pack opens Character Information"),Gameplay->ActivePanelPage,FString(TEXT("CharacterInfoPanel_Field")));
        TestEqual(TEXT("Character Information title is visible"),Gameplay->CharacterInfoTitle->GetText().ToString(),FString(TEXT("Character Information")));
        TestEqual(TEXT("Character Information fills the available panel height"),Manager->FindElementUnder(TEXT("CharacterInfoPanel_Field"),TEXT("CharacterInfoText"))->Height,
            Manager->FindElementByName(TEXT("CharacterInfoPanel_Field"))->Height-25);
        const auto Info=Gameplay->PanelBodyText->GetText().ToString();
        for (const TCHAR* Part:{TEXT("You were born"),TEXT("13 minutes 1 second"),TEXT("Natural Resistances"),TEXT("Innate Self"),TEXT("Daggers"),TEXT("Bows"),TEXT("Naturalist"),TEXT("Infused Creature"),TEXT("Jack of All Trades")})
            TestTrue(Part,Info.Contains(Part));
        CaptureScreen(TEXT("GameplayCharacterInformation"));
        if (Gameplay->CharacterInfoScroll) { Gameplay->CharacterInfoScroll->SetScrollOffset(1000);CaptureScreen(TEXT("GameplayCharacterInformationScrolled")); }
        Session.PlayerVitals=SavedVitals;
        Gameplay->ShowPanelPage(TEXT("MiniGamePanel_Field"));Gameplay->bChessActive=true;Gameplay->ChessMyColor=0;Gameplay->ResetChessBoard();Gameplay->RefreshMiniGameOverlays();
        CaptureScreen(TEXT("GameplayChessPieces"));
        int32 PieceCount=0;for (const auto& Icon:Gameplay->ChessSquareIcons) if (Icon && Icon->GetVisibility()!=ESlateVisibility::Collapsed) {++PieceCount;TestNotNull(TEXT("Chess piece resolves to a DAT texture"),Icon->Background.GetResourceObject());}
        TestEqual(TEXT("Starting chess game paints all 32 pieces"),PieceCount,32);
        TestFalse(TEXT("Chess hides the unrelated Pass action"),Manager->FindElementUnder(TEXT("MiniGamePanel_Field"),TEXT("MiniGame_Pass"))->bVisible);
        Gameplay->bChessActive=false;
        Gameplay->RefreshShortcutOverlays();
        for(int32 I=0;I<9;++I) if(!Session.GetShortcutObject(I))
            TestTrue(TEXT("Empty first-nine shortcuts keep the blue numbered DAT background"),Gameplay->ShortcutSlotBgs[I]->Background.GetResourceObject()==Resources->ResolveIconTexture(0x060010FA+I));
        Gameplay->ShowPanelPage(TEXT("WorldPanel_Field"));
        Gameplay->SyncWorldPanelTab(TEXT("MapPage")); Gameplay->RefreshWorldOverlays();
        CaptureScreen(TEXT("GameplayMapHover"));
        if (const auto MapImage=Manager->FindElementUnder(TEXT("MapPage"),TEXT("Map")))
        {
            // Arwic's authored map hover rectangle is 190,88 to 199,96.
            const FVector2D NativePoint=FVector2D(MapImage->GetScreenOrigin())
                +FVector2D(194.f*MapImage->Width/257.f,92.f*MapImage->Height/267.f);
            FString Town;
            const FVector2D Absolute=Canvas->GetCachedGeometry().LocalToAbsolute(NativePoint*Canvas->GetLastScale2D());
            TestTrue(TEXT("Map exposes a town hover label"),Gameplay->GetMapTooltipAt(Absolute,Town));
            TestEqual(TEXT("Map hover resolves Arwic"),Town,FString(TEXT("Arwic")));
        }
        Client->Session->House.bQueried=true; Client->Session->House.bOwned=true; Client->Session->House.HouseType=1;
        Client->Session->House.BuyTime=1750000000; Client->Session->House.RentTime=1750000000;
        FACEHousePayment RentFixture; RentFixture.Name=TEXT("Pyreal"); RentFixture.PluralName=TEXT("Pyreals");
        RentFixture.Required=1000; RentFixture.Paid=1000; Client->Session->House.Rent={RentFixture};
        Gameplay->SyncWorldPanelTab(TEXT("HousePage")); Gameplay->RefreshWorldOverlays();
        CaptureScreen(TEXT("GameplayHousing"));
        TestTrue(TEXT("Housing uses the retail Bought label"),Gameplay->HouseTextRows.ContainsByPredicate(
            [](UTextBlock* Label){return Label && Label->GetText().ToString().StartsWith(TEXT("Bought:"));}));
        Client->Session->House=FACEHouseInfo();

        Gameplay->ShowPanelPage(TEXT("SocialPanel_Field"));
		{
			const auto SavedEnchantments=Session.ActiveEnchantments;
			Session.ActiveEnchantments.Reset();
			for (int32 Id=1;Id<1000 && Session.ActiveEnchantments.Num()<24;++Id)
			{
				uint32 Flags=0,Target=0;if(!Dat->TryGetSpellTargeting(Id,Flags,Target) || !(Flags&4))continue;
				FACEActiveEnchantment E; E.SpellId=Id; E.SpellCategory=Id; E.bBeneficial=false;
				E.Duration=Id==1 ? -1 : 3661.9f; E.PowerLevel=100; Session.ActiveEnchantments.Add(E);
			}
			FACEActiveEnchantment Time; Time.Duration=3661.9f;
			TestEqual(TEXT("Retail effect timer includes hours, minutes and seconds, truncating fractions"),Gameplay->FormatEnchantmentRemaining(Time),FString(TEXT("1:01:01")));
			Time.Duration=61.9f; TestEqual(TEXT("Short timer uses m:ss"),Gameplay->FormatEnchantmentRemaining(Time),FString(TEXT("1:01")));
			Time.ReceivedAt=FPlatformTime::Seconds()-10;
			TestEqual(TEXT("Effect timer advances between server updates"),Gameplay->FormatEnchantmentRemaining(Time),FString(TEXT("0:51")));
			Time.Duration=-1; TestTrue(TEXT("Permanent effect has no duration label"),Gameplay->FormatEnchantmentRemaining(Time).IsEmpty());
			Gameplay->ShowPanelPage(TEXT("PositiveEffectsPanel_Field")); Gameplay->TickRefresh();
			TestEqual(TEXT("Effect rows use the authored 32px template"),Gameplay->EffectsRowElements[0]->Height,32);
			TestEqual(TEXT("Opening effects does not invent a selected spell"),Gameplay->SelectedEffectsSpellId,0);
			Gameplay->SelectedEffectsSpellId=1;
			for (int32 SpellId:Gameplay->EffectsListSpellIds)
			{
				FString ValidDescription;
				if (Dat->TryGetSpellDescription(SpellId,ValidDescription) && !ValidDescription.IsEmpty())
				{Gameplay->SelectedEffectsSpellId=SpellId;break;}
			}
			Gameplay->RefreshEffectsOverlays(true);
			FString EffectDescription; Dat->TryGetSpellDescription(Gameplay->SelectedEffectsSpellId,EffectDescription);
			TestTrue(TEXT("Selected effect shows the actual spell description"),!EffectDescription.IsEmpty() && Gameplay->EffectsInfoLabel->GetText().ToString().Contains(EffectDescription));
			for (int32 EffectRow=0;EffectRow<Gameplay->EffectsListSpellIds.Num();++EffectRow)
			{
				auto* NameSlot=Cast<UCanvasPanelSlot>(Gameplay->EffectsListRows[EffectRow]->Slot);
				auto* TimeSlot=Cast<UCanvasPanelSlot>(Gameplay->EffectsListDurations[EffectRow]->Slot);
				TestTrue(TEXT("Effect name cannot overlap its separate duration column"),NameSlot && TimeSlot && NameSlot->GetPosition().X+NameSlot->GetSize().X<=TimeSlot->GetPosition().X+.01);
				TestFalse(TEXT("Duration is not appended to the spell name"),Gameplay->EffectsListRows[EffectRow]->GetText().ToString().Contains(TEXT(":")));
			}
			CaptureScreen(TEXT("GameplayEffectsRetailColumns"));
			Gameplay->EffectsScrollOffset=999; Gameplay->RefreshEffectsOverlays(true); CaptureScreen(TEXT("GameplayEffectsRetailScrolled"));
			TestEqual(TEXT("Retail spell flags override missing server beneficial flags"),Gameplay->EffectsContentCount,24);
			auto Stronger=Session.ActiveEnchantments[0]; Stronger.PowerLevel=200; Session.ActiveEnchantments.Add(Stronger);
			Gameplay->RefreshEffectsOverlays(true); TestEqual(TEXT("Superseded enchantment is not a second active row"),Gameplay->EffectsContentCount,24);
			const auto PositiveFixtures=Session.ActiveEnchantments;
			Session.ActiveEnchantments.Reset();
			for(int32 Id=1;Id<1000 && Session.ActiveEnchantments.Num()<24;++Id)
			{
				uint32 Flags=0,Target=0;if(!Dat->TryGetSpellTargeting(Id,Flags,Target) || (Flags&4))continue;
				FACEActiveEnchantment E;E.SpellId=Id;E.SpellCategory=Id;E.bBeneficial=true;E.Duration=180;Session.ActiveEnchantments.Add(E);
			}
			FACEActiveEnchantment Cooldown;Cooldown.SpellId=0x8001;Cooldown.SpellCategory=0x8001;Cooldown.bCooldown=true;Session.ActiveEnchantments.Add(Cooldown);
			Gameplay->ShowPanelPage(TEXT("NegativeEffectsPanel_Field")); Gameplay->TickRefresh(); CaptureScreen(TEXT("GameplayHarmfulEffectsRetailColumns"));
			TestEqual(TEXT("Debuff list follows spell flags and excludes cooldowns"),Gameplay->EffectsContentCount,24);
			// Death and debuff-only purge must update both panels, retaining
			// permanent equipment effects and revealing an underlying weaker buff.
			// Keep this fixture out of the already large screen-test function.
			TFunction<void()> CheckDeathEffects=[&]()
			{
			auto GearBuff=PositiveFixtures[0];GearBuff.Duration=-1;GearBuff.Layer=1;GearBuff.bBeneficial=true;
			auto CastBuff=PositiveFixtures[1];CastBuff.Duration=180;CastBuff.Layer=2;CastBuff.bBeneficial=true;
			CastBuff.SpellCategory=GearBuff.SpellCategory;CastBuff.PowerLevel=GearBuff.PowerLevel+100;
			auto GearCurse=Session.ActiveEnchantments[0];GearCurse.Duration=-1;GearCurse.bBeneficial=false;
			auto CastCurse=Session.ActiveEnchantments[1];CastCurse.Duration=180;CastCurse.bBeneficial=false;
			FACEActiveEnchantment Vitae;Vitae.SpellId=666;Vitae.bVitae=true;Vitae.Duration=-1;Vitae.StatModValue=.95f;
			Session.ActiveEnchantments={GearBuff,CastBuff,GearCurse,CastCurse,Cooldown,Vitae};
			Gameplay->EffectsScrollOffset=0;Gameplay->ShowPanelPage(TEXT("PositiveEffectsPanel_Field"));Gameplay->TickRefresh();
			TestTrue(TEXT("Timed stronger buff suppresses equipment buff before death"),Gameplay->EffectsListSpellIds.Contains(CastBuff.SpellId) && !Gameplay->EffectsListSpellIds.Contains(GearBuff.SpellId));
			FACEBinaryWriter EmptyPurge;FACEBinaryReader PurgeReader(EmptyPurge.GetData());
			Session.HandleMagicPurgeBadEnchantments(PurgeReader);
			Gameplay->ShowPanelPage(TEXT("NegativeEffectsPanel_Field"));Gameplay->TickRefresh();
			TestEqual(TEXT("Debuff-only purge keeps permanent equipment curse"),Gameplay->EffectsContentCount,1);
			TestTrue(TEXT("Correct retained harmful spell is displayed"),Gameplay->EffectsListSpellIds.Contains(GearCurse.SpellId));
			Gameplay->ShowPanelPage(TEXT("PositiveEffectsPanel_Field"));Gameplay->TickRefresh();
			TestTrue(TEXT("Debuff-only purge leaves beneficial timed cast in place"),Gameplay->EffectsListSpellIds.Contains(CastBuff.SpellId));
			Session.HandleMagicPurgeEnchantments(PurgeReader);Gameplay->TickRefresh();
			TestEqual(TEXT("Death retains equipment, cooldown and Vitae records"),Session.ActiveEnchantments.Num(),4);
			TestEqual(TEXT("Buff panel contains only the surviving equipment buff"),Gameplay->EffectsContentCount,1);
			TestTrue(TEXT("Equipment buff becomes visible after stronger cast is purged"),Gameplay->EffectsListSpellIds.Contains(GearBuff.SpellId) && !Gameplay->EffectsListSpellIds.Contains(CastBuff.SpellId));
			CaptureScreen(TEXT("GameplayEquipmentEffectsAfterDeath"));
			Gameplay->ShowPanelPage(TEXT("NegativeEffectsPanel_Field"));Gameplay->TickRefresh();
			TestEqual(TEXT("Death retains permanent harmful effect without exposing Vitae or cooldown"),Gameplay->EffectsContentCount,1);
			// A later authoritative removal (unequip/mana loss) still removes it.
			FACEBinaryWriter RemoveGear;RemoveGear.WriteUInt16(GearCurse.SpellId);RemoveGear.WriteUInt16(GearCurse.Layer);
			FACEBinaryReader RemoveGearReader(RemoveGear.GetData());Session.HandleMagicRemoveEnchantment(RemoveGearReader);Gameplay->TickRefresh();
			TestEqual(TEXT("Explicit removal clears the harmful panel"),Gameplay->EffectsContentCount,0);
			};
			CheckDeathEffects();
			Session.ActiveEnchantments=SavedEnchantments;
			Gameplay->ShowPanelPage(TEXT("SocialPanel_Field"));
		}
        for (const TCHAR* AuditTab:{TEXT("AllegiancePage"),TEXT("FellowshipPage"),TEXT("FriendsPage"),TEXT("SquelchPage")})
        {
            Gameplay->SyncSocialPanelTab(AuditTab);Gameplay->TickRefresh();CaptureScreen(FString(TEXT("GameplayAudit"))+AuditTab);
        }
        {
            const auto SavedFellowship=Session.Fellowship;
            const uint32 SavedOptions=Session.CharacterOptions1;
            Session.Fellowship=FACEFellowshipInfo();
            Gameplay->SyncSocialPanelTab(TEXT("FellowshipPage"));Gameplay->TickRefresh();
            const TPair<const TCHAR*,int32> Checks[]={{TEXT("IgnoreFellowshipRequests"),2},{TEXT("FellowshipAutoAcceptRequests"),0x12},{TEXT("FellowshipShareXP"),0x0f},{TEXT("FellowshipShareLoot"),0x11}};
            for(const auto& Check:Checks)
            {
                const auto El=Manager->FindElementUnder(TEXT("FellowshipPage"),Check.Key);
                if(!TestTrue(TEXT("Retail fellowship checkbox exists"),El.IsValid()))continue;
                const FIntPoint Origin=El->GetScreenOrigin();
                const auto Hit=Manager->HitTestCanvas(Origin.X+El->Width/2,Origin.Y+El->Height/2);
                TestTrue(FString::Printf(TEXT("%s label is a clickable retail checkbox"),Check.Key),Hit==El);
                const bool Before=Client->IsCharacterOptionSet(Check.Value);
                Session.CachedC2SPackets.Reset();
                const FVector2D Point=FVector2D(Origin)+FVector2D(El->Width/2,El->Height/2);
                Manager->NotifyMouseDown(Point,Canvas->GetCachedGeometry().GetLocalSize(),EKeys::LeftMouseButton);
                Manager->NotifyMouseUp(Point,Canvas->GetCachedGeometry().GetLocalSize(),EKeys::LeftMouseButton);
                TestEqual(TEXT("Actual pointer click toggles the server-backed option"),Client->IsCharacterOptionSet(Check.Value),!Before);
                TestTrue(TEXT("Checkbox emits character option action"),HasAction(ACEGameAction::SetSingleCharacterOption));
            }
            Gameplay->FellowshipNameEntry->SetText(FText::GetEmpty());
            TestFalse(TEXT("Empty name disables Create like retail"),Gameplay->CanActivateFellowshipControl(TEXT("CreateFellowshipButton")));
            Session.CachedC2SPackets.Reset();Gameplay->HandleNamedClick(TEXT("CreateFellowshipButton"));
            TestFalse(TEXT("Empty name cannot silently create a generic fellowship"),HasAction(ACEGameAction::FellowshipCreate));
            Client->SendSetSingleCharacterOption(0x0f,false);
            Gameplay->FellowshipNameEntry->SetText(FText::FromString(TEXT("Retail Test")));
            Session.CachedC2SPackets.Reset();Gameplay->HandleNamedClick(TEXT("CreateFellowshipButton"));
            TestTrue(TEXT("Named fellowship can be created"),HasAction(ACEGameAction::FellowshipCreate));
            for(const auto& P:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(P.Value.Payload);R.Skip(24);if(R.ReadUInt32()!=ACEGameAction::FellowshipCreate)continue;
                TestEqual(TEXT("Create uses typed name"),R.ReadString16L(),FString(TEXT("Retail Test")));
                TestEqual(TEXT("Create uses displayed XP preference rather than stale default"),R.ReadUInt32(),0u);
            }
            FACEFellowshipMember Self;Self.Guid=Player.Guid;Self.Name=TEXT("Leader");Self.Level=100;Self.HealthCur=250;Self.HealthMax=500;Self.StaminaCur=300;Self.StaminaMax=400;Self.ManaCur=50;Self.ManaMax=200;
            FACEFellowshipMember Other=Self;Other.Guid=991122;Other.Name=TEXT("Fellow");
            auto& F=Session.Fellowship;F.bValid=true;F.Name=TEXT("Retail Test");F.LeaderGuid=Self.Guid;F.Members={Self,Other};
            Gameplay->SelectedFellowGuid=Other.Guid;Gameplay->RefreshFellowshipOverlays();Gameplay->RefreshSocialButtonLabels();
            TestEqual(TEXT("Fellowship member uses the retail two-line height"),Gameplay->FellowRowElements[0]->Height,32);
            const auto HealthMeter=Manager->FindElementUnder(TEXT("FellowsListBox"),TEXT("FellowHealth"));
            TestTrue(TEXT("Fellowship health is a live meter"),HealthMeter&&FMath::IsNearlyEqual(HealthMeter->MeterFillFraction,.5f));
            for(int32 I=2;I<9;++I){Other.Guid=991122+I;Other.Name=FString::Printf(TEXT("Fellow %d"),I+1);F.Members.Add(Other);}
            Gameplay->FellowScrollOffset=999;Gameplay->RefreshFellowshipOverlays();
            TestEqual(TEXT("Nine-member fellowship scrolls to its final member"),Gameplay->FellowRowGuids[Gameplay->FellowVisibleRows-1],F.Members.Last().Guid);
            Gameplay->FellowScrollOffset=0;Gameplay->RefreshFellowshipOverlays();
            TestTrue(TEXT("Leader can dismiss another member"),Gameplay->CanActivateFellowshipControl(TEXT("FellowDismissButton")));
            TestTrue(TEXT("Leader can transfer leadership"),Gameplay->CanActivateFellowshipControl(TEXT("FellowLeaderButton")));
            Gameplay->SelectedFellowGuid=Self.Guid;
            TestFalse(TEXT("Leader cannot dismiss self"),Gameplay->CanActivateFellowshipControl(TEXT("FellowDismissButton")));
            F.bOpen=true;Gameplay->RefreshSocialButtonLabels();
            TestTrue(TEXT("Open fellowship button now says Close"),Gameplay->SocialButtonLabels.ContainsByPredicate([](const UTextBlock* T){return T&&T->GetText().ToString()==TEXT("Close");}));
            F.LeaderGuid=Other.Guid;Gameplay->RefreshFellowshipOverlays();
            for(const TCHAR* Name:{TEXT("FellowDismissButton"),TEXT("FellowLeaderButton"),TEXT("FellowOpenButton"),TEXT("FellowDisbandButton")})
            {
                Session.CachedC2SPackets.Reset();Gameplay->HandleNamedClick(Name);
                TestTrue(TEXT("Leader-only controls cannot emit packets for ordinary members"),Session.CachedC2SPackets.IsEmpty());
            }
            CaptureScreen(TEXT("GameplayFellowshipMember"));
            Session.Fellowship=SavedFellowship;Session.CharacterOptions1=SavedOptions;
            Gameplay->FellowshipNameEntry->SetText(FText::GetEmpty());
            Gameplay->RefreshFellowshipOverlays();Gameplay->RefreshSocialButtonLabels();
        }
        Session.SocketC2S->Close();Sockets->DestroySocket(Session.SocketC2S);Session.SocketC2S=nullptr;
        ComponentReceiver->Close();Sockets->DestroySocket(ComponentReceiver);
        for (const TCHAR* Page:{TEXT("WorldPanel_Field"),TEXT("CharacterInfoPanel_Field"),TEXT("PositiveEffectsPanel_Field"),
            TEXT("NegativeEffectsPanel_Field"),TEXT("LinkStatusPanel_Field"),TEXT("VitaePanel_Field"),
            TEXT("BookPanel_Field"),TEXT("MiniGamePanel_Field"),TEXT("AbusePanel_Field"),TEXT("UrgentAssistancePanel_Field")})
        {
            Gameplay->ShowPanelPage(Page);Gameplay->TickRefresh();CaptureScreen(FString(TEXT("GameplayAudit"))+Page);
        }
        Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
        {
            const auto InventoryTitleElement=Manager->FindElementUnder(TEXT("InventoryPanel_Field"),TEXT("InvTitleText"));
            TestTrue(TEXT("Inventory retains its retail title strip"),InventoryTitleElement && InventoryTitleElement->ImageFileId==0x06004CFA && InventoryTitleElement->Height==25);
            if (InventoryTitleElement)
            {
                TestEqual(TEXT("Inventory retains the retail 18px font"),InventoryTitleElement->FontId,0x40000001u);
                FACEDatFont TitleFont; Resources->ResolveFont(InventoryTitleElement->FontId,TitleFont);
                const FString LongTitle=TEXT("Inventory of An Adventurer With A Very Long Character Name");
                const FString Fitted=ACEDatText::Ellipsize(TitleFont,LongTitle,InventoryTitleElement->Width-10);
                TestTrue(TEXT("Long inventory title ends with an ellipsis"),Fitted.EndsWith(TEXT("...")) && Fitted.Len()<LongTitle.Len());
                TestTrue(TEXT("Inventory ellipsis fits inside native margins"),ACEDatText::Layout(TitleFont,Fitted,InventoryTitleElement->Width-10,true)[0].Width<=InventoryTitleElement->Width-10);
            }
        }
        // Earlier fixture actions send requests without server replies. This
        // isolated targeting/scale check starts idle; busy precedence is covered
        // with real request/completion events in MouseCursors.
        Client->Session->ResetCursorWaits();
        const int32 PreviousCursorCombatMode=Client->Session->PlayerVitals.CombatMode;
        Client->Session->PlayerVitals.CombatMode=1;
        Controller->MouseCursorWidget=NewObject<UACEMouseCursorWidget>(); Controller->MouseCursorWidget->Initialize();
        auto CursorSlate=Controller->MouseCursorWidget->TakeWidget();
        Controller->RetailCursorDefaultTex=Resources->ResolveIconTexture(0x06004D68);
        Controller->RetailCursorTargetTex=Resources->ResolveIconTexture(0x06004D73);
        Controller->RetailCursorTargetValidTex=Resources->ResolveIconTexture(0x06005E6B);
        Controller->RetailCursorTargetInvalidTex=Resources->ResolveIconTexture(0x06005E6A);
        Controller->bRetailCursorInstalled=true;
        UImage* CursorArt=nullptr;
        Controller->MouseCursorWidget->WidgetTree->ForEachWidget([&](UWidget* W){ if (auto* I=Cast<UImage>(W)) CursorArt=I; });
        Controller->SetPendingUseTargeting(true); CursorSlate->SlatePrepass(1.f);
        if (TestNotNull(TEXT("Target cursor contains DAT image widget"),CursorArt))
        {
            TestTrue(TEXT("Arming an item selects retail untargeted crosshair"),CursorArt->GetBrush().GetResourceObject()==Controller->RetailCursorTargetTex);
            const FVector2D Size=CursorArt->GetBrush().ImageSize;
            TestTrue(TEXT("Software cursor has explicit centered desired size"),CursorSlate->GetDesiredSize().Equals(FVector2f(Size*2),.01f));
            const auto* Slot=Cast<UCanvasPanelSlot>(CursorArt->Slot);
            TestTrue(TEXT("Retail target hotspot remains at the pointer"),Slot && (Slot->GetPosition()-Size).Equals(FVector2D(-14,-14),.01));
            for(float Scale:{.5f,1.5f,3.f})
            {
                Controller->MouseCursorWidget->SetCursorScale(Scale);CursorSlate->SlatePrepass(1.f);
                TestTrue(TEXT("Cursor artwork scales independently of the UI"),CursorArt->GetBrush().ImageSize.Equals(FVector2f(Size*Scale),.01f));
                TestTrue(TEXT("Scaling preserves the precise cursor hotspot"),Slot && (Slot->GetPosition()-Size*Scale).Equals(FVector2D(-14,-14)*Scale,.01));
                TestTrue(TEXT("Scaled software cursor retains centered bounds"),CursorSlate->GetDesiredSize().Equals(FVector2f(Size*Scale*2),.01f));
            }
            Controller->MouseCursorWidget->SetCursorScale(1.f);
            Controller->ApplyRetailMouseCursor(true,true);
            TestTrue(TEXT("Compatible target selects green retail target art"),CursorArt->GetBrush().GetResourceObject()==Controller->RetailCursorTargetValidTex);
            Controller->ApplyRetailMouseCursor(true,false);
            TestTrue(TEXT("Incompatible target selects red retail target art"),CursorArt->GetBrush().GetResourceObject()==Controller->RetailCursorTargetInvalidTex);
            Controller->SetPendingUseTargeting(false);
            TestTrue(TEXT("Completed use restores retail arrow"),CursorArt->GetBrush().GetResourceObject()==Controller->RetailCursorDefaultTex);
        }
        Client->Session->PlayerVitals.CombatMode=PreviousCursorCombatMode;
        FACEWorldObject Stone; Stone.Guid=9876; Stone.ItemType=ACEItemType::ManaStone;
        Stone.ContainerId=Player.Guid; Stone.ItemUseable=0x000A0008; Stone.TargetType=ACEItemType::Caster|ACEItemType::Creature;
        Client->Session->WorldObjects.Add(Stone.Guid,Stone);
        Gameplay->PendingUseWithSourceGuid=Stone.Guid;
        TestTrue(TEXT("Owned mana target passes retail cursor rules"),Gameplay->IsPendingUseTargetCompatible(Item.Guid));
        FACEWorldObject Enemy; Enemy.Guid=9877; Enemy.ItemType=ACEItemType::Creature;
        Client->Session->WorldObjects.Add(Enemy.Guid,Enemy);
        TestFalse(TEXT("Contained target rules reject an unowned creature"),Gameplay->IsPendingUseTargetCompatible(Enemy.Guid));
        Player.ItemType=ACEItemType::Creature; Client->Session->WorldObjects.Add(Player.Guid,Player);
        TestTrue(TEXT("Mana targeting permits the player-wide target"),Gameplay->IsPendingUseTargetCompatible(Player.Guid));
        Client->Session->TradeSelfItems.Add(Item.Guid);
        TestFalse(TEXT("Offered trade item uses incompatible targeting cursor"),Gameplay->IsPendingUseTargetCompatible(Item.Guid));
        Client->Session->TradeSelfItems.Reset(); Gameplay->PendingUseWithSourceGuid=0;
        Player.SetupId=0x02000001; Player.MotionTableId=0x09000001;
        Client->Session->WorldObjects.Add(Player.Guid,Player); Gameplay->LastVitals.HeritageGroup=1;
        const auto BeforeModelAppraisal=Gameplay->LastAppraisal;
        Gameplay->LastAppraisal=FACEAppraisalInfo();
        Gameplay->LastAppraisal.ObjectGuid=Player.Guid;Gameplay->LastAppraisal.Name=TEXT("Character inspection preview");
        Gameplay->LastAppraisal.bIsCreature=true;Gameplay->LastAppraisal.bSuccess=true;
        Gameplay->LastAppraisal.StringProperties.Add(5,TEXT("War Mage"));
        Gameplay->LastAppraisal.IntProperties={{113,1},{188,1}};
        Gameplay->LastAppraisal.Level=275;Gameplay->LastAppraisal.Strength=245;
        Gameplay->LastAppraisal.Health=378;Gameplay->LastAppraisal.MaxHealth=378;
        Gameplay->ShowExamination(true);Gameplay->RefreshExaminationOverlay();
        if (TestNotNull(TEXT("Inspection creates a creature model capture"),Gameplay->ExamPaperDollCapture.Get()))
        {
            auto* Capture=Gameplay->ExamPaperDollCapture.Get();
            auto* Actor=Gameplay->ExamPaperDollPreviewActor.Get();
            TestEqual(TEXT("Inspection uses retail model heading"),Actor->FindComponentByClass<UACECharacterAppearanceComponent>()->MeshFacingYawDegrees,ACERetailPaperDoll::HeadingDegrees);
            FBox Bounds(ForceInit);TArray<UMeshComponent*> Meshes;Actor->GetComponents(Meshes);
            for (auto* Mesh:Meshes) if (Mesh->IsVisible())
            {
                Bounds+=Mesh->Bounds.GetBox();
            }
            TestTrue(TEXT("Inspection model has visible geometry"),bool(Bounds.IsValid));
            const float Aspect=float(Gameplay->ExamPaperDollRenderTarget->SizeX)/Gameplay->ExamPaperDollRenderTarget->SizeY;
            const FVector Size=Bounds.GetSize();
            const float ExpectedDistance=FMath::Max(Size.Z,Size.X/Aspect)*1.2071068f+Size.Y*.5f;
            TestTrue(TEXT("Inspection camera frames actual bounds instead of a fixed human height"),Capture->GetComponentLocation().Equals(Bounds.GetCenter()-FVector(0,ExpectedDistance,0),.1));
            TestTrue(TEXT("Inspection preview is isolated from the playfield"),Actor->GetActorLocation().Equals(FVector(-50000,-48000,-50000),.1));
            if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
            for (int32 I=0;I<3;++I) { Capture->CaptureScene();FlushRenderingCommands(); }
            TArray<FFloat16Color> Pixels;Gameplay->ExamPaperDollRenderTarget->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Pixels);
            int32 Covered=0;for(const auto& Pixel:Pixels) Covered+=Pixel.A.GetFloat()<.5f;
            AddInfo(FString::Printf(TEXT("Inspection capture covers %d/%d pixels"),Covered,Pixels.Num()));
            TestTrue(TEXT("Rendered inspection model occupies a useful share of the viewport"),Covered>Pixels.Num()/20 && Covered<Pixels.Num()*3/4);
            CaptureScreen(TEXT("GameplayCharacterInspectionWithModel"));
            auto* Appearance = Actor->FindComponentByClass<UACECharacterAppearanceComponent>();
            TestTrue(TEXT("Inspection uses its own view instead of world distance culling"),Appearance->bPreviewCapture);
            float HighestHead = 0.f;
            for (int32 Frame=0; Frame<90; ++Frame)
            {
                Appearance->TickComponent(1.f/30.f,LEVELTICK_All,nullptr);
                TArray<UPrimitiveComponent*> Parts; Actor->GetComponents(Parts);
                for (auto* Part : Parts) if (Part->GetName()==TEXT("ACEPart_16"))
                {
                    Part->UpdateBounds();
                    const FVector HeadTop=Part->Bounds.Origin+FVector(0,0,Part->Bounds.BoxExtent.Z);
                    const FVector CameraSpace=Capture->GetComponentTransform().InverseTransformPosition(HeadTop);
                    HighestHead=FMath::Max(HighestHead,float(CameraSpace.Z/(CameraSpace.X*FMath::Tan(FMath::DegreesToRadians(22.5f)))));
                }
            }
            AddInfo(FString::Printf(TEXT("Animated inspection head top in vertical clip space: %.3f"),HighestHead));
            TestTrue(TEXT("Animated inspection keeps the entire head inside the camera"), HighestHead < 1.f);
            Capture->CaptureScene();FlushRenderingCommands();CaptureScreen(TEXT("GameplayAnimatedInspection"));
            for (uint32 Setup : {0x02001A12u,0x02001A16u,0x0200196Du,0x020005C4u,0x02001A18u})
            {
                FACEWorldObject Subject=Player; Subject.SetupId=Setup; Subject.MotionTableId=Setup==0x020005C4 ? 0 : 0x09000001;
                // Some Undead setups intentionally contain a null head. The
                // server supplies it through ObjDesc; exercise that replacement.
                const auto* HumanSetup=Dat->GetOrBuildSetupMesh(0x02000001,100.f);
                if (Setup!=0x020005C4 && HumanSetup && HumanSetup->Parts.IsValidIndex(16))
                {
                    FACEObjDescAnimPartChange Head;
                    Head.PartIndex=16;Head.PartId=HumanSetup->Parts[16].GfxObjId;
                    Subject.Appearance.AnimPartChanges.Add(Head);
                }
                TestTrue(TEXT("Marketplace subject appearance is available"),Gameplay->ApplyPreviewCaptureAppearance(Actor,Capture,Subject,100.f));
                for (UProceduralMeshComponent* Part : Appearance->PartMeshes) if (Part && Part->GetName()==TEXT("ACEPart_16"))
                {
                    AddInfo(FString::Printf(TEXT("Inspection head %08X replacement=%08X visible=%d hidden=%d sections=%d shown=%d pos=%s"),
                        Setup,Subject.Appearance.AnimPartChanges.IsEmpty()?0:Subject.Appearance.AnimPartChanges.Last().PartId,
                        Part->IsVisible(),Part->bHiddenInGame,Part->GetNumSections(),Capture->ShowOnlyComponents.Contains(Part),*Part->GetComponentLocation().ToString()));
                    TestTrue(TEXT("Replaced inspection head is visible to the capture"),Setup==0x020005C4 ||
                        (Part->IsVisible() && !Part->bHiddenInGame && Part->GetNumSections()>0 && Capture->ShowOnlyComponents.Contains(Part)));
                }
                float HeadTopClip=0;
                for (int32 Frame=0;Frame<90;++Frame)
                {
                    Appearance->TickComponent(1.f/30.f,LEVELTICK_All,nullptr);
                    TArray<UPrimitiveComponent*> Parts;Actor->GetComponents(Parts);
                    for(auto* Part:Parts) if(Part->GetName()==TEXT("ACEPart_16"))
                    {
                        Part->UpdateBounds();
                        const FVector Top=Part->Bounds.Origin+FVector(0,0,Part->Bounds.BoxExtent.Z);
                        const FVector P=Capture->GetComponentTransform().InverseTransformPosition(Top);
                        HeadTopClip=FMath::Max(HeadTopClip,float(P.Z/(P.X*FMath::Tan(FMath::DegreesToRadians(22.5f)))));
                    }
                }
                AddInfo(FString::Printf(TEXT("Inspection setup %08X animated head top %.3f"),Setup,HeadTopClip));
                TestTrue(TEXT("Marketplace character head stays inside inspection frame"),HeadTopClip<1.f);
                Capture->CaptureScene();FlushRenderingCommands();CaptureScreen(FString::Printf(TEXT("GameplayInspection_%08X"),Setup));
            }
            Player.Scale=.5f;Client->Session->WorldObjects.Add(Player.Guid,Player);Gameplay->RefreshExaminationOverlay();
            TestTrue(TEXT("Inspection reframes an unchanged setup after scale changes"),FMath::IsNearlyEqual(Actor->GetActorScale3D().X,.5f,.001f));
            Player.Scale=1.f;Client->Session->WorldObjects.Add(Player.Guid,Player);
        }
        Gameplay->ShowExamination(false);Gameplay->RefreshExaminationOverlay();
        TestEqual(TEXT("Closing inspection hides its model"),Gameplay->ExamPaperDollModelImage->GetVisibility(),ESlateVisibility::Collapsed);
        TestFalse(TEXT("Closed inspection stops spending time on invisible animation"),
            Gameplay->ExamPaperDollPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>()->IsComponentTickEnabled());
        Gameplay->ReleaseExamPaperDollPreview();Gameplay->LastAppraisal=BeforeModelAppraisal;
        Gameplay->bShowPaperdollSlots=false; Gameplay->RefreshPaperDollPreview();
        if (TestNotNull(TEXT("Inventory constructs a renderable equipped-player preview"),Gameplay->PaperDollCapture.Get()))
        {
            TestTrue(TEXT("Human portrait uses retail camera location"),(Gameplay->PaperDollCapture->GetComponentLocation()-Gameplay->PaperDollPreviewActor->GetActorLocation()).Equals(FVector(-12,-240,88),.01));
            TestTrue(TEXT("Portrait camera looks along retail positive Y without tilt"),Gameplay->PaperDollCapture->GetRelativeRotation().Equals(FRotator(0,90,0),.01));
            TestEqual(TEXT("Retail inventory heading is applied to geometry, not the camera actor"),
                Gameplay->PaperDollPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>()->MeshFacingYawDegrees,191.3679f);
            if (FApp::CanEverRender())
            {
                if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
                Gameplay->PaperDollCapture->CaptureScene(); FlushRenderingCommands();
                TArray<FColor> Pixels; Gameplay->PaperDollRenderTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
                const int32 W=Gameplay->PaperDollRenderTarget->SizeX,H=Gameplay->PaperDollRenderTarget->SizeY;
                TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(W,H,Pixels,PNG);
                FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/RetailParity/PaperDollHuman.png")));
                int32 Lit=0; for (const FColor& P:Pixels) if (FMath::Max3(P.R,P.G,P.B)>80) ++Lit;
                TestTrue(TEXT("Retail portrait actually draws the player's geometry"),Lit>500);
                // Inverse opacity is 1 in empty capture pixels and 0 on opaque geometry.
                TArray<FFloat16Color> LinearPixels;
                Gameplay->PaperDollRenderTarget->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(LinearPixels);
                TestTrue(TEXT("Portrait background stays transparent for the DAT panel"),
                    LinearPixels.Num()==W*H && LinearPixels[0].A.GetFloat()>.99f);
                int32 Opaque=0; for (const auto& Pixel:LinearPixels) if (Pixel.A.GetFloat()<.01f) ++Opaque;
                TestTrue(TEXT("Portrait alpha contains visible character geometry"),Opaque>500);
                FWidgetRenderer DollRenderer(true,true);
                auto* DollTarget=FWidgetRenderer::CreateTargetFor(FVector2D(W,H),TF_Bilinear,true);
                DollRenderer.DrawWidget(DollTarget,Gameplay->PaperDollModelImage->TakeWidget(),FVector2D(W,H),0.f);
                FlushRenderingCommands();
                TArray<FColor> DollPixels; DollTarget->GameThread_GetRenderTargetResource()->ReadPixels(DollPixels);
                int32 DollOpaque=0;for(const auto& Pixel:DollPixels) if(Pixel.A>200) ++DollOpaque;
                TArray64<uint8> DollPNG;FImageUtils::PNGCompressImageArray(W,H,DollPixels,DollPNG);
                FFileHelper::SaveArrayToFile(DollPNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/RetailParity/PaperDollComposite.png")));
                TestTrue(TEXT("Portrait widget preserves visible foreground pixels"),DollOpaque>500);
                TestTrue(TEXT("Portrait widget clears the capture background"),DollPixels.Num()==W*H && DollPixels[0].A<3);
                const auto Composite=CaptureScreen(TEXT("GameplayRetailPaperDoll"));
                Gameplay->PaperDollModelImage->SetColorAndOpacity(FLinearColor::Transparent);
                const auto Background=CaptureScreen(TEXT("GameplayPaperDollBackground"));
                const auto El=Manager->FindElementUnder(TEXT("InventoryPanel_Field"),TEXT("PaperDoll"));
                const auto& LayerGeometry=Canvas->GetElementLayer()->GetCachedGeometry();
                int32 Compared=0,Changed=0,ModelChanged=0;
                if (El && Composite.Num()==ScreenSize.X*ScreenSize.Y && Background.Num()==Composite.Num())
                    for (int32 Y=8;Y<El->Height-8;Y+=4) for (int32 X=8;X<El->Width-8;X+=4)
                    {
                        const int32 CaptureX=X*W/El->Width, CaptureY=Y*H/El->Height;
                        const float InverseOpacity=LinearPixels[CaptureY*W+CaptureX].A.GetFloat();
                        const FVector2D Screen=Canvas->GetCachedGeometry().AbsoluteToLocal(
                            LayerGeometry.LocalToAbsolute(FVector2D(El->GetScreenOrigin())+FVector2D(X,Y)));
                        const int32 SX=FMath::RoundToInt(Screen.X),SY=FMath::RoundToInt(Screen.Y);
                        if (SX<0 || SY<0 || SX>=ScreenSize.X || SY>=ScreenSize.Y) continue;
                        const auto C=Composite[SY*ScreenSize.X+SX],B=Background[SY*ScreenSize.X+SX];
                        const bool Differs=FMath::Abs(int32(C.R)-B.R)>2 || FMath::Abs(int32(C.G)-B.G)>2 || FMath::Abs(int32(C.B)-B.B)>2;
                        if (InverseOpacity>.999f) { ++Compared; if (Differs) ++Changed; }
                        else if (InverseOpacity<.01f && Differs) ++ModelChanged;
                    }
                TestTrue(TEXT("The composited widget visibly draws the character over the background"),ModelChanged>50);
                AddInfo(FString::Printf(TEXT("Portrait composite: %d background samples, %d changed, %d visible model samples"),Compared,Changed,ModelChanged));
                TestTrue(TEXT("Portrait composites over the actual panel artwork without a filled rectangle"),Compared>100 && Changed<Compared/20);
                Gameplay->PaperDollModelImage->SetColorAndOpacity(FLinearColor::White);
            }
            FACESelectedObject DollSelection; DollSelection.bValid=true; DollSelection.Guid=Player.Guid;
            DollSelection.SelectionSerial=Gameplay->LastSelection.SelectionSerial+1;
            Gameplay->HandleSelectionChanged(DollSelection);
            TestTrue(TEXT("Selecting the player flashes the equipment model"),Gameplay->FlashMeshes.ContainsByPredicate([&](const auto& Mesh){return Mesh.IsValid() && Mesh->GetOwner()==Gameplay->PaperDollPreviewActor;}));
            Gameplay->SelectionFlashUntil=0; Gameplay->TickSelectionFlash();
            TestTrue(TEXT("Selection flash restores all original materials"),Gameplay->FlashMeshes.IsEmpty());
            Player.Scale=.7f; Client->Session->WorldObjects.Add(Player.Guid,Player); Gameplay->RefreshPaperDollPreview();
            TestTrue(TEXT("Short characters retain the fixed retail camera distance"),
                (Gameplay->PaperDollCapture->GetComponentLocation()-Gameplay->PaperDollPreviewActor->GetActorLocation()).Equals(FVector(-12,-240,88),.01));
            TestTrue(TEXT("Paperdoll geometry retains server character scale"),Gameplay->PaperDollPreviewActor->GetActorScale3D().Equals(FVector(.7f),.001));
            Player.Scale=1.f; Client->Session->WorldObjects.Add(Player.Guid,Player);
            Gameplay->LastVitals.HeritageGroup=8; Gameplay->RefreshPaperDollPreview();
            TestTrue(TEXT("Heritage change updates portrait framing without an equipment change"),(Gameplay->PaperDollCapture->GetComponentLocation()-Gameplay->PaperDollPreviewActor->GetActorLocation()).Equals(FVector(-12,-340,100),.01));
        }
        // Exercise actual DAT mask coordinates through drag/drop and the network writer.
		FSocket* EquipmentReceiver = Sockets->CreateSocket(NAME_DGram, TEXT("Equipment fixture receiver"), false);
		auto EquipmentAddress = Sockets->CreateInternetAddr(); EquipmentAddress->SetIp(TEXT("127.0.0.1"), ValidAddress); EquipmentAddress->SetPort(0);
		if (!EquipmentReceiver || !EquipmentReceiver->Bind(*EquipmentAddress)) return false;
		EquipmentReceiver->GetAddress(*EquipmentAddress);
		Session.SocketC2S = Sockets->CreateSocket(NAME_DGram, TEXT("Equipment fixture sender"), false);
		Session.ServerC2SAddr = EquipmentAddress;
        // Offline command dispatch exercises the same writer as the chat entry.
        auto Dispatch=[&](const TCHAR* Command,uint32 Action)
        {
            Session.CachedC2SPackets.Reset();Gameplay->TryDispatchChatCommand(Command,nullptr,nullptr);
            TArray<uint8> Body;bool FoundAction=false;
            for (const auto& Pair:Session.CachedC2SPackets)
            {
                FACEBinaryReader Wire(Pair.Value.Payload);Wire.Skip(16);
                if (Wire.ReadUInt32()!=ACEOpcode::GameAction) continue;
                Wire.ReadUInt32();if (Wire.ReadUInt32()!=Action) continue;FoundAction=true;
                Body.Append(Pair.Value.Payload.GetData()+28,Pair.Value.Payload.Num()-28);
            }
            TestTrue(*FString::Printf(TEXT("%s sends action %04X"),Command,Action),FoundAction);
            return Body;
        };
        Session.LastTellSenderGuid=0x50000002;Session.LastTellSenderName=TEXT("Remote Friend");
        auto ReplyBytes=Dispatch(TEXT("/r hello"),ACEGameAction::Tell);
        FACEBinaryReader ReplyWire(ReplyBytes);
        TestEqual(TEXT("Reply uses name-based tells across landblocks"),ReplyWire.ReadString16L(),FString(TEXT("hello")));
        TestEqual(TEXT("Reply names the last player teller"),ReplyWire.ReadString16L(),FString(TEXT("Remote Friend")));
        auto SquelchBytes=Dispatch(TEXT("/squelch Remote Friend"),ACEGameAction::ModifyCharacterSquelch);
        FACEBinaryReader SquelchWire(SquelchBytes);
        TestEqual(TEXT("Squelch enable flag"),SquelchWire.ReadUInt32(),1u);SquelchWire.ReadUInt32();
        TestEqual(TEXT("Squelch preserves a multiword name"),SquelchWire.ReadString16L(),FString(TEXT("Remote Friend")));
        TestEqual(TEXT("Squelch defaults to retail AllChannels, not Broadcast"),SquelchWire.ReadUInt32(),1u);
        auto UnsquelchBytes=Dispatch(TEXT("/unsquelch -reply -tell"),ACEGameAction::ModifyCharacterSquelch);
        FACEBinaryReader UnsquelchWire(UnsquelchBytes);TestEqual(TEXT("Unsquelch disable flag"),UnsquelchWire.ReadUInt32(),0u);
        UnsquelchWire.ReadUInt32();UnsquelchWire.ReadString16L();TestEqual(TEXT("Squelch category reaches server"),UnsquelchWire.ReadUInt32(),3u);
        auto NoTellBytes=Dispatch(TEXT("/notell on"),ACEGameAction::ModifyGlobalSquelch);
        FACEBinaryReader NoTellWire(NoTellBytes);TestEqual(TEXT("Notell on suppresses tells"),NoTellWire.ReadUInt32(),1u);
        TestEqual(TEXT("Notell sends Tell category"),NoTellWire.ReadUInt32(),3u);
        auto ChannelBytes=Dispatch(TEXT("/f hello"),ACEGameAction::ChatChannel);
        FACEBinaryReader ChannelWire(ChannelBytes);TestEqual(TEXT("Fellow shortcut sends the real channel id"),ChannelWire.ReadUInt32(),ACEChatChannel::Fellow);
        auto OnBytes=Dispatch(TEXT("/on help"),ACEGameAction::AddChannel);
        FACEBinaryReader OnWire(OnBytes);TestEqual(TEXT("Channel membership uses Help id"),OnWire.ReadUInt32(),1024u);
        Dispatch(TEXT("/index"),ACEGameAction::IndexChannels);
        Dispatch(TEXT("/clist help"),ACEGameAction::ListChannels);
        Dispatch(TEXT("/off help"),ACEGameAction::RemoveChannel);
        Dispatch(TEXT("/motd"),0x0255);
        Dispatch(TEXT("/motd set Fixture welcome"),0x0254);
        Dispatch(TEXT("/allegiance name set Fixture guild"),0x0033);
        Dispatch(TEXT("/allegiance ban list"),0x02A3);
        Dispatch(TEXT("/allegiance ban add Remote Friend"),0x02A1);
        Dispatch(TEXT("/allegiance ban remove Remote Friend"),0x02A2);
        Dispatch(TEXT("/allegiance chat gag Remote Friend"),0x0041);
        Dispatch(TEXT("/allegiance chat kick Remote Friend, Fixture reason"),0x02A0);
        Dispatch(TEXT("/allegiance lock bypass Remote Friend"),0x0040);
        Dispatch(TEXT("/allegiance lock check"),0x003F);
        Dispatch(TEXT("/allegiance house guest open"),0x0042);
        auto OfficerBytes=Dispatch(TEXT("/allegiance officer add 2 Remote Friend"),0x003B);
        FACEBinaryReader OfficerWire(OfficerBytes);TestEqual(TEXT("Officer action puts name before level"),OfficerWire.ReadString16L(),FString(TEXT("Remote Friend")));
        TestEqual(TEXT("Officer level is serialized"),OfficerWire.ReadUInt32(),2u);
        auto TitleBytes=Dispatch(TEXT("/allegiance title set 3 Steward"),0x003C);
        FACEBinaryReader TitleWire(TitleBytes);TestEqual(TEXT("Officer title action puts level before title"),TitleWire.ReadUInt32(),3u);
        TestEqual(TEXT("Officer title is serialized"),TitleWire.ReadString16L(),FString(TEXT("Steward")));
        // Use the actual General dropdown destination and entry dispatch, not
        // only the /cg shortcut. The server supplies the room id at login.
        const uint32 SavedOptions=Session.CharacterOptions2,SavedGeneral=Session.TurbineGeneralChannel;
        Session.CharacterOptions2=~0u;Session.TurbineGeneralChannel=0x4321;
        Gameplay->ChatSendChannel=7;Session.CachedC2SPackets.Reset();
        const FString GeneralText=TEXT("hey");
        TestTrue(TEXT("General dropdown sends from the chat entry"),Gameplay->TrySendChatFromEntry(&GeneralText,0));
        bool GeneralPacket=false,LocalSay=false;
        for (const auto& Pair:Session.CachedC2SPackets)
        {
            FACEBinaryReader Wire(Pair.Value.Payload);Wire.Skip(16);const uint32 Opcode=Wire.ReadUInt32();
            LocalSay |= Opcode==ACEOpcode::GameAction;
            if (Opcode!=ACEOpcode::TurbineChat) continue;
            GeneralPacket=true;Wire.Skip(48);
            TestEqual(TEXT("General uses the server-provided Turbine room"),Wire.ReadUInt32(),0x4321u);
            TestEqual(TEXT("General sends raw message text without local-speech markup"),Wire.ReadPackedUnicode(),GeneralText);
        }
        TestTrue(TEXT("General is serialized as F7DE"),GeneralPacket);
        TestFalse(TEXT("General does not send a local speech action"),LocalSay);
        Session.CharacterOptions2=SavedOptions;Session.TurbineGeneralChannel=SavedGeneral;
        Gameplay->GlobalChatTypeFilter=~0ull;Gameplay->ChatFilterMode=0;
        Gameplay->HandleChatMessage(TEXT("[General] Remote Friend says, \"hey\""),TEXT("Remote Friend"),ACEChatMessageType::General);
        FString LastDisplayed;
        TFunction<void(UWidget*)> ReadText=[&](UWidget* Widget)
        {
            if (auto* Label=Cast<UTextBlock>(Widget)) LastDisplayed+=Label->GetText().ToString();
            if (auto* Panel=Cast<UPanelWidget>(Widget)) for (auto* Child:Panel->GetAllChildren()) ReadText(Child);
        };
        if (Gameplay->ChatLog && Gameplay->ChatLog->GetChildrenCount()) ReadText(Gameplay->ChatLog->GetChildAt(Gameplay->ChatLog->GetChildrenCount()-1));
        TestTrue(TEXT("General displays one formatted server message"),LastDisplayed.Contains(TEXT("[General] Remote Friend says, \"hey\"")));
        TestFalse(TEXT("General is not nested in a local say message"),LastDisplayed.Contains(TEXT("says, \"[General]")));
        Gameplay->CancelPendingUseWith(); Gameplay->ShowExamination(false);
        Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field")); Gameplay->bShowPaperdollSlots=false;
        FACEWorldObject Shirt; Shirt.Guid=9901; Shirt.Name=TEXT("Undershirt"); Shirt.ItemType=ACEItemType::Clothing;
        Shirt.WielderId=Player.Guid; Shirt.CurrentWieldedLocation=2; Shirt.ValidLocations=2; Shirt.IconId=0x060011CF;
        FACEWorldObject Armor=Shirt; Armor.Guid=9902; Armor.Name=TEXT("Chest armor"); Armor.ItemType=ACEItemType::Armor;
        Armor.CurrentWieldedLocation=0x200; Armor.ValidLocations=0x200;
        Session.WorldObjects.Add(Shirt.Guid,Shirt); Session.WorldObjects.Add(Armor.Guid,Armor);
        Session.ClearPendingUse();Client->SelectObject(Shirt.Guid);
        Session.CachedC2SPackets.Reset();Controller->InteractWithSelectedObject();
        TestEqual(TEXT("F on worn clothing sends exactly one inventory action"),Session.CachedC2SPackets.Num(),1);
        for (const auto& Packet:Session.CachedC2SPackets)
        {
            if (!TestTrue(TEXT("Clothing pickup has a complete inventory payload"),Packet.Value.Payload.Num()>=40)) continue;
            FACEBinaryReader Wire(Packet.Value.Payload);Wire.Skip(24);
            TestEqual(TEXT("Removing clothing uses PutItemInContainer rather than ActOnUse"),Wire.ReadUInt32(),ACEGameAction::PutItemInContainer);
            TestEqual(TEXT("Removal retains selected clothing"),Wire.ReadUInt32(),uint32(Shirt.Guid));
            TestEqual(TEXT("Removal puts clothing into the main pack"),Wire.ReadUInt32(),uint32(Player.Guid));
            TestEqual(TEXT("Removal uses the first inventory position"),Wire.ReadUInt32(),0u);
        }
        Gameplay->SelectInventoryGuid(Armor.Guid);
        Gameplay->RefreshSelectionOverlay();
        Gameplay->bShowPaperdollSlots=true; Gameplay->RefreshInventoryOverlays();
        TestTrue(TEXT("Equipment checkbox draws the retail checked glyph above the model"),Gameplay->PaperdollSlotsCheckboxIcon && Gameplay->PaperdollSlotsCheckboxIcon->GetVisibility()!=ESlateVisibility::Collapsed && Gameplay->PaperdollSlotsCheckboxIcon->Background.GetResourceObject()==Resources->ResolveIconTexture(0x06004D17));
        CaptureScreen(TEXT("GameplayEquipmentSelected"));
        bool bSelectedArtwork=false;
        for (const auto& Pair:Gameplay->PaperDollSelectedIcons)
            if (Pair.Value && Pair.Value->GetVisibility()!=ESlateVisibility::Collapsed)
                bSelectedArtwork |= Pair.Value->Background.GetResourceObject()==Resources->ResolveIconTexture(0x06004D09);
        TestTrue(TEXT("Selected equipment uses the authored selection frame"),bSelectedArtwork);
        Gameplay->bShowPaperdollSlots=false;
        Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayEquipmentModelTargets"));
        TestTrue(TEXT("Model mode draws the retail unchecked glyph"),Gameplay->PaperdollSlotsCheckboxIcon && Gameplay->PaperdollSlotsCheckboxIcon->Background.GetResourceObject()==Resources->ResolveIconTexture(0x06004D15));
        TestEqual(TEXT("Model chest selects armor above the undershirt"),Gameplay->FindUpperEquippedItem(514),Armor.Guid);
        auto MaskElement=Manager->FindElementUnder(TEXT("InventoryPanel_Field"),TEXT("PaperDollDragMask"));
        if (TestTrue(TEXT("Authored body silhouette target exists"),MaskElement.IsValid()))
        {
            FIntPoint Pixel(-1,-1);
            for (int32 Y=0;Y<MaskElement->Height && Pixel.X<0;++Y)
                for (int32 X=0;X<MaskElement->Width;++X)
                    if (Resources->ResolvePaperDollSelectionMask(FIntPoint(X,Y))==514) { Pixel=FIntPoint(X,Y);break; }
            TestTrue(TEXT("The authored click map has a chest target"),Pixel.X>=0);
            const FVector2D Absolute=Canvas->GetElementLayer()->GetCachedGeometry().LocalToAbsolute(FVector2D(MaskElement->GetScreenOrigin()+Pixel)+FVector2D(.5,.5));
            FString SlotName; int64 Mask=0;
            TestTrue(TEXT("Live model hit testing reaches the DAT silhouette"),Gameplay->HitTestDollSlot(Absolute,SlotName,Mask));
            TestEqual(TEXT("Hidden slot boxes cannot intercept model selection"),SlotName,FString(TEXT("PaperDoll")));
            TestEqual(TEXT("Body click selects both chest layers"),Mask,int64(514));
            const FVector2D Local=Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute);
            Gameplay->CancelInventoryDrag();
            TestTrue(TEXT("Chest armor can be dragged from the 3D model"),Gameplay->TryBeginInventoryDrag(Local));
            TestEqual(TEXT("Drag chooses the outermost equipped item"),Gameplay->InvDragGuid,Armor.Guid);
            Gameplay->CancelInventoryDrag();

            // Keep this fixture out of the already-large screen test's optimized body.
            TFunction<void()> CheckManaStoneTargets = [&]()
            {
            FACEWorldObject ManaStone; ManaStone.Guid=9903; ManaStone.Name=TEXT("Charged mana stone");
            ManaStone.ItemType=ACEItemType::ManaStone; ManaStone.ItemUseable=0x0A0008;
            ManaStone.TargetType=ACEItemType::Creature|ACEItemType::Armor|ACEItemType::Clothing;
            ManaStone.ContainerId=Player.Guid; ManaStone.UiEffects=1;
            Session.WorldObjects.Add(ManaStone.Guid,ManaStone);
            auto UseManaAt = [&](const FVector2D& Point, int32 ExpectedTarget, bool bDrop)
            {
                Session.ClearPendingUse(); Session.CachedC2SPackets.Reset();
                Gameplay->CancelPendingUseWith(); Gameplay->CancelInventoryDrag();
                const FVector2D DropLocal=Canvas->GetCachedGeometry().AbsoluteToLocal(Point);
                if (bDrop)
                {
                    Gameplay->InvDragGuid=ManaStone.Guid;
                    Gameplay->bInvDragPending=true; Gameplay->bInvDragActive=true;
                    Gameplay->UpdateInventoryDrag(DropLocal);
                    if (Gameplay->bShowPaperdollSlots)
                    {
                        TestNotNull(TEXT("Mana-stone slot drag shows accept/reject feedback"),Gameplay->PaperDollDragTargetIcon.Get());
                        if (Gameplay->PaperDollDragTargetIcon)
                            TestTrue(TEXT("Drop artwork reflects targeted use rather than trying to equip the stone"),
                                Gameplay->PaperDollDragTargetIcon->Background.GetResourceObject()==Resources->ResolveIconTexture(ExpectedTarget?0x060011F9:0x060011F8));
                    }
                    TestTrue(TEXT("Paperdoll consumes the mana-stone drop"),Gameplay->TryFinishInventoryDrag(DropLocal));
                }
                else
                {
                    Gameplay->UseInventoryItem(ManaStone.Guid);
                    TestEqual(TEXT("Target cursor agrees with the actual paperdoll use target"),Gameplay->GetInventoryTargetAt(Point),ExpectedTarget);
                    FPointerEvent Down(0,Point,Point,TSet<FKey>{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState());
                    FPointerEvent Up(0,Point,Point,TSet<FKey>{},EKeys::LeftMouseButton,0,FModifierKeysState());
                    Canvas->NativeOnMouseButtonDown(Canvas->GetCachedGeometry(),Down);
                    Canvas->NativeOnMouseButtonUp(Canvas->GetCachedGeometry(),Up);
                }
                int32 Uses=0;
                for (const auto& Pair:Session.CachedC2SPackets)
                {
                    FACEBinaryReader Wire(Pair.Value.Payload); Wire.Skip(16);
                    if (Wire.ReadUInt32()!=ACEOpcode::GameAction) continue;
                    Wire.ReadUInt32(); if (Wire.ReadUInt32()!=ACEGameAction::UseWithTarget) continue;
                    ++Uses;
                    TestEqual(TEXT("Use packet contains the mana stone"),Wire.ReadUInt32(),uint32(ManaStone.Guid));
                    TestEqual(TEXT("Model targets self for server distribution; slots target exactly their item"),Wire.ReadUInt32(),uint32(ExpectedTarget));
                }
                TestEqual(TEXT("An empty slot cannot accidentally discharge the stone across all gear"),Uses,ExpectedTarget?1:0);
            };
            UseManaAt(Absolute,Player.Guid,false);
            UseManaAt(Absolute,Player.Guid,true);
            // An empty stone on the model must never pick worn armor to destroy.
            Session.ClearPendingUse(); Session.CachedC2SPackets.Reset();
            ManaStone.UiEffects=0; Session.WorldObjects.Add(ManaStone.Guid,ManaStone);
            Gameplay->UseInventoryItem(ManaStone.Guid);
            TestTrue(TEXT("Empty stone model click is handled"),Gameplay->TryHandleOverlayClick(
                Canvas->GetElementLayer()->GetCachedGeometry().AbsoluteToLocal(Absolute),false));
            TestEqual(TEXT("Empty stone model click never opens armor destruction confirmation"),Gameplay->ManaStoneConfirmSource,0);
            TestEqual(TEXT("Empty stone model click sends no use request"),Session.CachedC2SPackets.Num(),0);
            ManaStone.UiEffects=1; Session.WorldObjects.Add(ManaStone.Guid,ManaStone);

            Gameplay->bShowPaperdollSlots=true; Gameplay->RefreshInventoryOverlays();
            CaptureScreen(TEXT("GameplayManaStoneSlotTargets"));
            for (const auto& TargetSlot : {TPair<FString,int32>(TEXT("Inv_ChestSlot"),Armor.Guid),
                {TEXT("Inv_ClothesShirtSlot"),Shirt.Guid},{TEXT("Inv_AbdomenSlot"),0}})
            {
                const auto SlotElement=Manager->FindElementUnder(TEXT("InventoryPanel_Field"),TargetSlot.Key);
                if (!TestTrue(TEXT("Equipment slot has authored geometry"),SlotElement.IsValid())) continue;
                const FVector2D SlotAbsolute=Canvas->GetElementLayer()->GetCachedGeometry().LocalToAbsolute(
                    FVector2D(SlotElement->GetScreenOrigin())+FVector2D(SlotElement->Width,SlotElement->Height)*.5);
                UseManaAt(SlotAbsolute,TargetSlot.Value,false);
                UseManaAt(SlotAbsolute,TargetSlot.Value,true);
            }
            Session.ClearPendingUse(); Gameplay->CancelPendingUseWith();
            Session.WorldObjects.Remove(ManaStone.Guid);
            Gameplay->bShowPaperdollSlots=false; Gameplay->RefreshInventoryOverlays();
            CaptureScreen(TEXT("GameplayEquipmentModelAfterMana"));
            };
            CheckManaStoneTargets();
            Armor.WielderId=0; Armor.ContainerId=Player.Guid; Armor.CurrentWieldedLocation=0;
            Session.WorldObjects.Add(Armor.Guid,Armor); Session.CachedC2SPackets.Reset();
            Gameplay->InvDragGuid=Armor.Guid; Gameplay->bInvDragPending=true; Gameplay->bInvDragActive=true;
            Gameplay->TryFinishInventoryDrag(Local);
			bool bSentWield = false;
			for (const auto& Pair : Session.CachedC2SPackets)
			{
				FACEBinaryReader Wire(Pair.Value.Payload); Wire.Skip(16);
				if (Wire.ReadUInt32() != ACEOpcode::GameAction) continue;
				Wire.ReadUInt32();
				if (Wire.ReadUInt32() == ACEGameAction::GetAndWieldItem)
					bSentWield |= Wire.ReadUInt32() == uint32(Armor.Guid) && Wire.ReadUInt32() == 0x200u;
			}
			TestTrue(TEXT("Dropping armor on the model sends the item and full wield mask"), bSentWield);
        }
		Session.SocketC2S->Close(); Sockets->DestroySocket(Session.SocketC2S); Session.SocketC2S = nullptr;
		EquipmentReceiver->Close(); Sockets->DestroySocket(EquipmentReceiver);
        Gameplay->ReleasePaperDollPreview(); Gameplay->PlayerController=nullptr; Controller->Client=nullptr;
        PreviewGI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
    }

    // Match the user's magic -> peace -> inventory/critter death -> GC sequence.
    // Unplaced widgets are not owned by WidgetTree simply because it is their Outer.
    {
        TArray<TStrongObjectPtr<UObject>> Roots;
        for (UObject* Object : TArray<UObject*>{GI,Dat,Resources,Manager,Layout,Canvas,Binder,Client,Gameplay}) Roots.Emplace(Object);
        TArray<TWeakObjectPtr<UBorder>> Icons;
        for (UBorder* Icon : Gameplay->BuiltInSpellIconBorders) { Icon->RemoveFromParent(); Icons.Add(Icon); }
        Gameplay->CombatMode=1;
        auto* Video=Cast<UACEVideoSettingsWidget>(Gameplay->VideoSettings);
        UWidget* Generated=Video->GenerateOption(TEXT("High"));
        TWeakObjectPtr<UWidget> OptionLifetime=Generated;
        auto OptionSlate=Generated->TakeWidget();
        CollectGarbage(RF_NoFlags);
        TestTrue(TEXT("Dropdown text retained by an open Slate menu survives GC"),OptionLifetime.IsValid());
        if (auto* OptionRow=Cast<UACERetailOptionWidget>(OptionLifetime.Get()))
            TestEqual(TEXT("Dropdown keeps the chosen value after GC"),Cast<UTextBlock>(OptionRow->WidgetTree->RootWidget)->GetText().ToString(),FString(TEXT("High")));
        bool Alive=true;
        for (const auto& Icon : Icons) Alive &= Icon.IsValid();
        TestTrue(TEXT("Innate spell widget cache survives garbage collection when unplaced"),Alive);
        if (Alive) Gameplay->RefreshSpellHotbarOverlays();
    }
    {
        constexpr int32 Frames=120;
        const double Start=FPlatformTime::Seconds();
        for (int32 I=0; I<Frames; ++I) Gameplay->TickRefresh();
        const double BindEnd=FPlatformTime::Seconds();
        for (int32 I=0; I<Frames; ++I) { ++GFrameCounter; Canvas->NativeTick(Canvas->GetCachedGeometry(),0.f); }
        AddInfo(FString::Printf(TEXT("HUD CPU fixture: binder %.3f ms/frame; total HUD tick %.3f ms/frame"),
            (BindEnd-Start)*1000/Frames,(FPlatformTime::Seconds()-BindEnd)*1000/Frames));
    }
    Gameplay->Shutdown(); Layout->Shutdown(); Manager->Shutdown();
    return true;
}
#endif

