#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Mobile/ACEMobileSettings.h"
#include "Mobile/ACEMobileWidget.h"
#include "ACEPlayerController.h"
#include "ACELoginWidget.h"
#include "Components/Button.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ACEClientSubsystem.h"
#include "ACEUpdateSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "Mods/ACEPluginSubsystem.h"
#include "VR/ACEVRMenu.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIGameplayBinder.h"
#include "ACETestDatDirectory.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SVirtualWindow.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "ShaderCompiler.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEMobileLayoutTest,"ACE.Mobile.LayoutAndUpdates",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEMobileLayoutTest::RunTest(const FString&)
{
    auto* Settings=NewObject<UACEMobileSettings>();Settings->ResetLayout();
    for(FVector2D Pixels:{FVector2D(1280,720),FVector2D(2520,1080),FVector2D(2184,1968),FVector2D(2560,1600),FVector2D(1024,768),FVector2D(800,600)})
    {
        const auto Logical=ACEMobile::LogicalSize(Pixels);
        auto Controls=Settings->Controls;ACEMobile::FitLayout(Controls,Logical);
        for(int32 I=0;I<Controls.Num();++I)
        {
            TestTrue(TEXT("All default controls fit phone/tablet layouts"),Controls[I].bVisible);
            TestTrue(TEXT("Controls clear safe edges, menu rail, and each other"),ACEMobile::CanPlace(Controls,I,Controls[I].Position,Controls[I].Size,Logical));
        }
        const auto First=Controls;ACEMobile::FitLayout(Controls,Logical);
        for(int32 I=0;I<Controls.Num();++I)TestTrue(TEXT("Repeated layout is stable"),Controls[I].Position.Equals(First[I].Position));
        TestFalse(TEXT("Cannot place over another control"),ACEMobile::CanPlace(Controls,1,Controls[0].Position,Controls[1].Size,Logical));
        TestFalse(TEXT("Cannot cover permanent menu rail"),ACEMobile::CanPlace(Controls,1,{.5,.02},Controls[1].Size,Logical));
        TestFalse(TEXT("Cannot cover selected ability footer"),ACEMobile::CanPlace(Controls,1,{.5,.99},Controls[1].Size,Logical));
        TestFalse(TEXT("Cannot cover enabled-plugin rail"),ACEMobile::CanPlace(Controls,2,{.04,.5},Controls[2].Size,Logical));
        const auto& Stick=Controls[0];const auto Center=Stick.Position*Logical;
        TestTrue(TEXT("D-pad accepts its center"),ACEMobile::HitControl(Stick,Logical,Center));
        TestFalse(TEXT("Transparent D-pad corners remain available for camera gestures"),ACEMobile::HitControl(Stick,Logical,Center+FVector2D(Stick.Size*.45,Stick.Size*.45)));
        Controls=Settings->Controls;ACEMobile::FitLayout(Controls,Logical,312);
        for(int32 I=0;I<Controls.Num();++I)
        {
            TestTrue(TEXT("All default controls remain available with pinned panels"),Controls[I].bVisible);
            TestTrue(TEXT("Pinned panels cannot cover a movement/action button"),ACEMobile::CanPlace(Controls,I,Controls[I].Position,Controls[I].Size,Logical,312));
        }
    }
    Settings->Controls[2].Action="Shortcut 14";Settings->Controls[2].Position={.5,.7};Settings->Controls[2].Size=80;
    const FString Config=FPaths::ProjectSavedDir()/TEXT("Automation/MobileLayout.ini");
    Settings->SaveConfig(CPF_Config,*Config);
    auto* Restored=NewObject<UACEMobileSettings>();Restored->LoadConfig(nullptr,*Config);
    TestEqual(TEXT("Rebound action survives restart"),Restored->Controls[2].Action,FName("Shortcut 14"));
    TestTrue(TEXT("Control position survives restart"),Restored->Controls[2].Position.Equals({.5,.7}));
    TestEqual(TEXT("Control size survives restart"),Restored->Controls[2].Size,80.f);
    Restored->Controls[2].Size=-20;Restored->Controls[2].Action="Unsupported";Restored->LookSensitivity=100;Restored->Sanitize();
    TestEqual(TEXT("Bad saved size is repaired"),Restored->Controls[2].Size,56.f);
    TestEqual(TEXT("Unknown action is repaired"),Restored->Controls[2].Action,FName("Attack"));
    Restored->Controls.RemoveAll([](const auto& C){return C.Id=="Inspect";});Restored->Controls[0].Size=144;
    Restored->Sanitize();
    TestTrue(TEXT("Existing layouts gain Inspect without losing rebound controls"),Restored->Controls.ContainsByPredicate([](const auto& C){return C.Id=="Inspect" && C.Action=="Inspect";}));
    TestEqual(TEXT("Old stick size upgrades to finger-sized D-pad"),Restored->Controls[0].Size,168.f);
    TestTrue(TEXT("D-pad center is neutral"),ACEMobile::MovementAxes({4,-10},84).IsNearlyZero());
    TestTrue(TEXT("Barely entering forward key runs at retail speed"),ACEMobile::MovementAxes({1,-29},84).Equals({0,-1}));
    TestTrue(TEXT("Diagonal uses both retail movement commands"),ACEMobile::MovementAxes({60,-60},84).Equals({1,-1}));
    for(float Size:{56.f,64.f,80.f,120.f,160.f})
    {
        const auto Content=ACEMobile::ButtonContent({Size,Size});
        TestTrue(TEXT("Icon and label never overlap at supported button sizes"),Content.Icon.Bottom<Content.Label.Top);
        TestTrue(TEXT("Button padding scales uniformly"),FMath::IsNearlyEqual(Content.Icon.Left/Size,6.f/64));
        TestTrue(TEXT("Resizing a button also resizes its artwork area"),FMath::IsNearlyEqual(Content.Icon.GetSize().Y/Size,.5f));
        for(FVector2D Native:{FVector2D(22,31),FVector2D(55,58),FVector2D(63,58),FVector2D(32,32)})
        {
            const auto Image=ACEMobile::FitImage(Native,Content.Icon);
            TestTrue(TEXT("Retail icons retain their native aspect ratio"),FMath::IsNearlyEqual(Image.GetSize().X/Image.GetSize().Y,Native.X/Native.Y,1.e-5));
            TestTrue(TEXT("Aspect fit centers icons in their available space"),Image.GetCenter().Equals(Content.Icon.GetCenter(),.001f));
            TestTrue(TEXT("Icon stays inside scaled button padding"),Image.Left>=Content.Icon.Left-.001f && Image.Right<=Content.Icon.Right+.001f && Image.Bottom<=Content.Icon.Bottom+.001f);
        }
    }
    for(float Size:{160.f,192.f,224.f})
    {
        const auto Left=ACEMobile::TurnKeyRect(Size,-1),Right=ACEMobile::TurnKeyRect(Size,1);
        TestTrue(TEXT("Turn keys stay square when resized"),FMath::IsNearlyEqual(Left.GetSize().X,Left.GetSize().Y,1.e-4f));
        TestTrue(TEXT("Turn gap scales with the keys"),FMath::IsNearlyEqual(Right.Left-Left.Right,Size/5,1.e-4f));
        TestTrue(TEXT("Turn keys retain finger-sized targets with a wide neutral gap"),Left.GetSize().X>=64 && Right.Left-Left.Right>=32);
    }
    for(float Size:{168.f,192.f,224.f})
    {
        const auto Center=ACEMobile::DPadKeyRect(Size,{0,0}),Up=ACEMobile::DPadKeyRect(Size,{0,-1}),Right=ACEMobile::DPadKeyRect(Size,{1,0});
        TestTrue(TEXT("D-pad center and arms use equal frames"),Center.GetSize().Equals(Up.GetSize()));
        TestTrue(TEXT("D-pad border gaps match horizontally and vertically"),FMath::IsNearlyEqual(Center.Top-Up.Bottom,Right.Left-Center.Right));
    }
    TestTrue(TEXT("Joystick dead zone is stable"),ACEMobile::StickVector({1,1},60).IsNearlyZero());
    TestTrue(TEXT("Joystick clamps diagonals"),FMath::IsNearlyEqual(ACEMobile::StickVector({100,100},60).Size(),1.));
    TestEqual(TEXT("Wheel center cancels selection"),ACEMobile::WheelIndex({0,0},8),INDEX_NONE);
    TestEqual(TEXT("Wheel top selects first spell"),ACEMobile::WheelIndex({0,-120},8),0);
    TestEqual(TEXT("Wheel right selects third spell"),ACEMobile::WheelIndex({120,0},8),2);
    TestEqual(TEXT("Outside wheel cannot accidentally cast"),ACEMobile::WheelIndex({1000,0},8),INDEX_NONE);
    TestEqual(TEXT("Fold7 memory budget is bounded to one GiB"),ACEMobile::TexturePoolMB(11102ull*1024*1024),1024);
    for(uint64 MB:{2048ull,4096ull,6144ull,8192ull,16384ull})
        TestTrue(TEXT("Mobile texture pool remains bounded by device RAM"),ACEMobile::TexturePoolMB(MB<<20)<=int32(MB/8) && ACEMobile::TexturePoolMB(MB<<20)<=1024);
    const FString Hash=FString::ChrN(64,TEXT('A'));
    const FString Manifest=FString::Printf(TEXT("{\"version\":142,\"androidVersion\":\"2026.10.27.142\",\"updates\":{\"schema\":1,\"android\":{\"file\":\"AC-Unreal-Android-v142.apk\",\"bytes\":42,\"sha256\":\"%s\"}}}"),*Hash);
    FACEUpdateRelease Release;FString Error;
    TestTrue(TEXT("Phone uses a separate verified update artifact"),ACEUpdates::ParseManifest(Manifest,false,Release,Error,false,true));
    TestFalse(TEXT("Quest cannot consume a phone release"),ACEUpdates::ParseManifest(Manifest,true,Release,Error));
    TestFalse(TEXT("Phone cannot consume a mislabeled Quest APK"),ACEUpdates::ParseManifest(Manifest.Replace(TEXT("AC-Unreal-Android-v142.apk"),TEXT("AC-VR-Quest-v142.apk")),false,Release,Error,false,true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEMobileTouchTest,"ACE.Mobile.TouchAndMenus",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEMobileTouchTest::RunTest(const FString&)
{
    auto* FixtureDefaults=GetMutableDefault<UACEMobileSettings>();const bool FixtureWasEnabled=FixtureDefaults->bEnabled;FixtureDefaults->bEnabled=true;
    ON_SCOPE_EXIT {FixtureDefaults->bEnabled=FixtureWasEnabled;};
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto* GI=NewObject<UGameInstance>(GEngine);GI->InitializeStandalone();auto* Dummy=GI->GetWorld();
    GI->GetWorldContext()->SetCurrentWorld(World);World->SetGameInstance(GI);Dummy->DestroyWorld(false);
    auto* PC=World->SpawnActor<AACEPlayerController>();auto* LP=NewObject<ULocalPlayer>(GEngine);
    LP->PlayerController=PC;GI->AddLocalPlayer(LP,FPlatformUserId::CreateFromInternalId(0));PC->SetPlayer(LP);World->AddController(PC);
    auto* Client=GI->GetSubsystem<UACEClientSubsystem>();PC->Client=Client;
    auto& Session=*Client->GetSession();Session.State=EACESessionState::InWorld;Session.PlayerGuid=100;
    Session.PlayerVitals.Health=210;Session.PlayerVitals.MaxHealth=300;Session.PlayerVitals.Stamina=350;Session.PlayerVitals.MaxStamina=400;Session.PlayerVitals.Mana=400;Session.PlayerVitals.MaxMana=500;
    auto* Touch=CreateWidget<UACEMobileWidget>(PC);Touch->InitializeMobile(PC);PC->MobileWidget=Touch;
    Touch->Settings=NewObject<UACEMobileSettings>();Touch->Settings->ResetLayout();Touch->Layout=Touch->Settings->Controls;ACEMobile::FitLayout(Touch->Layout,Touch->ViewSize);
    Touch->Settings->bPinnedChat=false;Touch->Settings->ExtraHUDPlugins.Reset();
    for(const auto& P:GI->GetSubsystem<UACEPluginSubsystem>()->Plugins)Touch->Settings->HiddenHUDPlugins.AddUnique(P->Id);
    ON_SCOPE_EXIT {Session.State=EACESessionState::Disconnected;Touch->NativeDestruct();Touch->RemoveFromParent();GI->Shutdown();World->DestroyWorld(false);};
    const auto Slate=Touch->TakeWidget();Touch->UpdateMobile();
    auto Location=[&](FName Id){const auto* C=Touch->Layout.FindByPredicate([Id](const auto& V){return V.Id==Id;});return C?C->Position*Touch->ViewSize:FVector2D::ZeroVector;};
    const auto Move=Location("Move"),Turn=Location("Turn"),Jump=Location("Jump");
    Touch->TouchBegin(0,Move+FVector2D(0,-29),Move+FVector2D(0,-29));
    TestTrue(TEXT("Touch-down on forward key moves at full speed without drag or jump"),Touch->Movement().Equals({0,-1}) && !Touch->IsJumpHeld());
    Touch->TouchMove(0,Move+FVector2D(0,-70));TestTrue(TEXT("Moving farther cannot change gait rate"),Touch->Movement().Equals({0,-1}));
    Touch->TouchEnd(0,Move,Move);
    Touch->TouchBegin(1,Turn+FVector2D(-30,0),Turn);
    TestEqual(TEXT("Left turn key turns immediately without dragging"),Touch->Turning(),-1.f);
    Touch->TouchMove(1,Turn+FVector2D(30,0));TestEqual(TEXT("Right turn key switches direction"),Touch->Turning(),1.f);
    Touch->TouchEnd(1,Turn,Turn);
    Touch->TouchBegin(1,Turn+FVector2D(12,0),Turn);
    TestEqual(TEXT("Wide gap between turn keys does not accidentally turn"),Touch->Turning(),0.f);
    Touch->TouchEnd(1,Turn,Turn);
    Touch->TouchBegin(0,Move,Move);Touch->TouchMove(0,Move+FVector2D(0,-60));
    TestTrue(TEXT("First finger walks forward"),Touch->Movement().Y<-.9);
    Touch->TouchBegin(1,Turn,Turn);Touch->TouchMove(1,Turn+FVector2D(45,0));
    TestTrue(TEXT("Second finger turns without stealing movement"),Touch->Turning()>.8 && Touch->Movement().Y<-.9);
    Touch->TouchBegin(2,Jump,Jump);TestTrue(TEXT("Third finger charges jump while moving/turning"),Touch->IsJumpHeld());
    Touch->TouchEnd(1,Turn,Turn);Touch->CancelFinger(1);
    TestTrue(TEXT("Releasing one pointer retains the other fingers"),Touch->Turning()==0 && Touch->Movement().Y<-.9 && Touch->IsJumpHeld());
    PC->bJumpCharging=true;Touch->CancelFinger(2);
    TestFalse(TEXT("Capture loss cancels jump instead of launching it"),PC->bJumpCharging);
    PC->bJumpCharging=true;PC->JumpChargeExtent=.65f;
    TestEqual(TEXT("Jump meter exposes actual movement charge"),PC->GetJumpChargeFraction(),.65f);
    PC->bJumpCharging=false;TestEqual(TEXT("Released jump meter is cleared"),PC->GetJumpChargeFraction(),0.f);
    Touch->OpenPanel("Menu");TestTrue(TEXT("Modal menu releases all held controls"),Touch->Movement().IsNearlyZero() && Touch->Fingers.IsEmpty());
    Touch->ClosePanel();TestTrue(TEXT("Closing menu does not resurrect held input"),Touch->Movement().IsNearlyZero());
    PC->bServerMoveToActive=true;Touch->CancelTouches();
    TestFalse(TEXT("Backgrounding cancels server-directed approach even after fingers lift"),PC->bServerMoveToActive);
    Touch->TouchBegin(0,Move,Move);Touch->TouchMove(0,Move+FVector2D(20,-40));Touch->CancelTouches();
    TestTrue(TEXT("Background/focus loss clears axes"),Touch->Movement().IsNearlyZero());
    Touch->TouchBegin(0,Move,Move);Touch->TouchMove(0,Move+FVector2D(0,-60));
    Touch->SurfaceResized(ACEMobile::LogicalSize({2184,1968}));
    TestTrue(TEXT("Unfolding releases movement before controls move"),Touch->Movement().IsNearlyZero() && Touch->Fingers.IsEmpty());
    Touch->SurfaceResized({960,540});
    Touch->bEditing=true;Touch->TouchBegin(0,Move,Move);Touch->TouchMove(0,{50,20});
    TestTrue(TEXT("Editing never moves the player"),Touch->Movement().IsNearlyZero());
    TestTrue(TEXT("Dragging into the menu rail is rejected"),Touch->Layout[0].Position.Equals(Move/Touch->ViewSize));
    Touch->CancelTouches();Touch->bEditing=false;

    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();FString Dats=TEXT("C:/Turbine/Asheron's Call");FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Dats);
    if(!TestTrue(TEXT("Mobile uses retail DAT resources"),Dat->LoadDatDirectory(Dats)))return false;
    auto* Manager=NewObject<UACEUIElementManager>(PC);Manager->Initialize();
    auto* Resources=NewObject<UACEUIResourceResolver>(PC);Resources->Initialize(Dat);
    auto* Layout=NewObject<UACEUILayoutResolver>(PC);Layout->Initialize(Dat,Manager);TestTrue(TEXT("Shared action layout loads"),Layout->LoadLayout(0x21000005));
    auto* Canvas=CreateWidget<UACEUICanvasWidget>(PC);Canvas->InitializeCanvas(Manager);Canvas->SetResourceResolver(Resources);Canvas->TakeWidget();
    auto* Binder=NewObject<UACEUIGameplayBinder>(PC);Binder->Initialize(Client,Manager,Canvas,PC);Canvas->SetGameplayBinder(Binder);
    PC->DatCanvasWidget=Canvas;PC->DatGameplayBinder=Binder;
    FACEWorldObject Self;Self.Guid=100;Self.Name=TEXT("Touch tester");Self.ItemsCapacity=102;Self.bIsPlayer=true;Session.WorldObjects.Add(100,Self);
    for(int32 I=0;I<50;++I)
    {FACEWorldObject Item;Item.Guid=1000+I;Item.ContainerId=100;Item.Name=FString::Printf(TEXT("Fixture item %d"),I+1);Item.IconId=0x06001036;Item.PlacementPosition=I;Item.StackSize=5;Item.MaxStackSize=100;Session.WorldObjects.Add(Item.Guid,Item);}
    Session.KnownSpells={1,2,3,4,5,6,7,8,9,10};Session.SpellBars.SetNum(8);Session.SpellBars[0]={1,2,3,4,5,6,7,8,9,10};
    Session.ShortcutObjects.SetNum(18);Session.ShortcutObjects[0]=1000;
    Binder->CombatMode=8;Binder->SelectedCombatSpellSlot=2;Touch->RefreshPresentation();
    TestEqual(TEXT("Attack artwork follows the actual selected spell"),Touch->ActionVisuals["Attack"].SpellId,3);
    TestNotNull(TEXT("Attack has composited retail spell artwork"),Touch->ActionVisuals["Attack"].Icon.Get());
    TestEqual(TEXT("Magic mode uses retail mode artwork"),Touch->ActionVisuals["Combat"].ResourceId,uint32(0x06004CF2));
    TestEqual(TEXT("Shortcut artwork resolves the exact bound item"),Touch->ActionVisuals["Shortcut 1"].ObjectId,1000);
    TestEqual(TEXT("Shortcut shows its current stack count"),Touch->ActionVisuals["Shortcut 1"].Badge,FString("5"));
    Session.ShortcutObjects[0]=1001;Binder->SelectedCombatSpellSlot=4;Touch->RefreshPresentation();
    TestEqual(TEXT("Changing a shortcut replaces its artwork and identity"),Touch->ActionVisuals["Shortcut 1"].ObjectId,1001);
    TestEqual(TEXT("External spell selection updates the touch attack button"),Touch->ActionVisuals["Attack"].SpellId,5);
    Binder->CombatMode=1;Touch->RefreshPresentation();
    TestEqual(TEXT("Peace mode uses the retail dove"),Touch->ActionVisuals["Combat"].ResourceId,uint32(0x06004CEC));
    TestEqual(TEXT("All retail vital cap, background and fill textures resolve"),Touch->VitalTextures.Num(),18);
    TestNotNull(TEXT("Health uses the end-of-retail floaty vital fill"),Touch->VitalTextures.FindRef(0x06007482).Get());
    TestEqual(TEXT("Inspect uses retail magnifying glass artwork"),Touch->ActionVisuals["Inspect"].ResourceId,uint32(0x06001127));
    Binder->CombatMode=8;
    Touch->Action("Spell wheel");TestTrue(TEXT("Touch wheel uses existing spell bank"),Touch->bWheel && Touch->WheelSpells.Num()==10);
    Touch->TouchBegin(1,{480,182},{480,182});Touch->TouchEnd(1,{480,182},{480,182});
    TestEqual(TEXT("Wheel chooses the requested spell"),Touch->SelectedSpell,1);
    Touch->RefreshPresentation();TestEqual(TEXT("Wheel selection is also the displayed attack spell"),Touch->ActionVisuals["Attack"].SpellId,1);
    TestFalse(TEXT("Selecting a spell dismisses wheel"),Touch->bWheel);
    Touch->Action("Spell wheel");Touch->TouchBegin(1,{605,505},{605,505});Touch->TouchEnd(1,{605,505},{605,505});
    TestEqual(TEXT("Entire next-page button is tappable"),Touch->WheelPage,1);
    Touch->bWheel=false;
    TestEqual(TEXT("Selecting in wheel does not fire a cast"),Session.CachedC2SPackets.Num(),0);
    Touch->OpenPanel("Inventory");TestTrue(TEXT("Inventory uses touch-sized menu"),Touch->Menu && Touch->Menu->bTouchMode);
    auto* Menu=Touch->Menu.Get();const int32 Builds=Menu->GetRefreshCount();for(int32 I=0;I<120;++I)Menu->RefreshIfDirty();
    TestEqual(TEXT("Unchanged inventory is not rebuilt every frame"),Menu->GetRefreshCount(),Builds);
    bool IconReady=false;int32 Attempts=0;
    auto Delayed=Menu->DeferredIcon([&]()->UTexture2D*{++Attempts;return IconReady?Touch->ActionVisuals["Inspect"].Icon.Get():nullptr;},64);
    const auto DelayedBrush=Menu->IconBrushes.Last();
    TestEqual(TEXT("Missing artwork is queued instead of permanently blank"),DelayedBrush->DrawAs,ESlateBrushDrawType::NoDrawType);
    IconReady=true;Menu->RefreshIfDirty();
    TestNotNull(TEXT("Icon appears once ready without an inventory update"),DelayedBrush->GetResourceObject());
    TestEqual(TEXT("Asset readiness does not rebuild the inventory"),Menu->GetRefreshCount(),Builds);
    Menu->RefreshIfDirty();TestEqual(TEXT("Resolved icons are not retried each frame"),Attempts,2);
    Client->SelectObject(1001);Touch->Action("Inspect");
    TestTrue(TEXT("World Inspect opens assessment of the current selection"),Menu->IsInspectionOpen() && Menu->InspectItem==1001);
    TestEqual(TEXT("Inspect keeps the selected target"),Client->GetSelectedObject().Guid,1001);
    TestEqual(TEXT("Inspect requests server appraisal"),Client->GetIdentifyRequestGuid(),1001);
    Touch->ClosePanel();
    Session.Book.bOpen=true;Session.Book.BookGuid=1000;Session.Book.NumPages=2;
    Session.Book.Pages={TEXT("A readable first page."),TEXT("The second page.")};
    Session.Book.PageTextIncluded={true,true};
    Client->OnBookChanged.Broadcast();Touch->UpdateMobile();
    TestEqual(TEXT("Books open a touch reader instead of a hidden desktop window"),Touch->Panel,FName("Book"));
    TestTrue(TEXT("Reading a book blocks movement"),Touch->IsBlockingMovement());
    Touch->ClosePanel();TestFalse(TEXT("Closing the reader clears book state"),Session.Book.bOpen);
    Session.Confirmations.Add({1,456,TEXT("Accept this fellowship invitation?")});
    ++GFrameCounter;Canvas->TickGameplayState(false);Touch->UpdateMobile();
    TestEqual(TEXT("Server confirmations remain accessible with the desktop UI hidden"),Touch->Panel,FName("Confirmation"));
    Touch->ResolveConfirmation(false);TestTrue(TEXT("Touch No resolves the pending server question"),Session.Confirmations.IsEmpty());
    {
        FACEBinaryWriter Message;Message.WriteUInt32(100);Message.WriteUInt32(2);Message.WriteUInt32(ACEGameEvent::PopupString);
        Message.WriteString16L(TEXT("Bring the sleeping pet back to its cage."));
        FACEBinaryReader R(Message.GetData());Session.HandleGameEvent(R);
        ++GFrameCounter;Canvas->TickGameplayState(false);Touch->UpdateMobile();
        TestTrue(TEXT("Server instructions open a touch message with local OK instead of Yes/No"),Touch->Panel=="Confirmation"
            && Touch->ConfirmationPopupId!=0 && Touch->ConfirmationText==TEXT("Bring the sleeping pet back to its cage."));
        const uint32 Sequence=Session.NextGameActionSequence;
        Touch->ResolveConfirmation(true);
        TestTrue(TEXT("Touch OK dismisses without sending confirmation or gameplay actions"),Session.GetServerPopups().IsEmpty()
            && Session.NextGameActionSequence==Sequence && Touch->ConfirmationPopupId==0);
    }
    {
        auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
        const auto Plugin=Host->Plugins.FindByPredicate([](const auto& P){return P->Id==TEXT("ucm");});
        if(Plugin){const bool Enabled=(*Plugin)->Enabled;(*Plugin)->Enabled=true;Touch->RefreshPluginRail();
            TestTrue(TEXT("Enabled plugins appear in the left rail"),Touch->RailPluginIds.Contains(TEXT("ucm")));(*Plugin)->Enabled=false;Touch->RefreshPluginRail();
            TestFalse(TEXT("Disabled plugins leave the left rail"),Touch->RailPluginIds.Contains(TEXT("ucm")));(*Plugin)->Enabled=Enabled;}
        Touch->ClosePanel();Host->TogglePanel();
        TestEqual(TEXT("Plugin manager opens inside the touch modal"),Touch->Panel,FName("Plugins"));
        Host->TogglePluginWindow(TEXT("ucm"));
        TestEqual(TEXT("Plugin window requests open visible mobile controls"),Touch->Panel,FName("Plugin:ucm"));
        TestTrue(TEXT("Mobile plugin window state reports its active panel"),Host->IsPluginWindowOpen(TEXT("ucm")));
        TestTrue(TEXT("Plugin controls block movement behind the panel"),Touch->IsBlockingMovement());
        Host->TogglePluginWindow(TEXT("ucm"));TestTrue(TEXT("Plugin close returns to the game"),Touch->Panel.IsNone());
    }
    auto* RailHost=GI->GetSubsystem<UACEPluginSubsystem>();TArray<bool> SavedEnabled;
    for(const auto& Plugin:RailHost->Plugins){SavedEnabled.Add(Plugin->Enabled);Plugin->Enabled=SavedEnabled.Num()<=3 || Plugin->Id==TEXT("ucm");}
    ON_SCOPE_EXIT {for(int32 I=0;I<FMath::Min(SavedEnabled.Num(),RailHost->Plugins.Num());++I)RailHost->Plugins[I]->Enabled=SavedEnabled[I];};
    Touch->RefreshPluginRail();
    // Persistent plugin readouts and chat reserve their own area but never make
    // gameplay modal. Each plugin still owns its live data and widgets.
    for(const FString Id:{TEXT("waypoint"),TEXT("huntledger"),TEXT("supplyledger")})
    {
        Touch->Settings->HiddenHUDPlugins.Remove(Id);
        for(const auto& P:RailHost->Plugins)if(P->Id==Id)P->Enabled=true;
    }
    RailHost->SetWaypointOption(TEXT("arrow"),true);
    Touch->ClosePanel();Touch->RefreshHUDPanels();
    TestTrue(TEXT("Enabled ledgers remain on screen outside a modal"),Touch->IsHUDPluginVisible(TEXT("huntledger")) && Touch->IsHUDPluginVisible(TEXT("supplyledger")) && !Touch->IsBlockingMovement());
    TestTrue(TEXT("Waypoint arrow is hosted on the mobile HUD"),Touch->IsHUDPluginVisible(TEXT("waypoint.arrow")));
    RailHost->SetWaypointOption(TEXT("arrow"),false);Touch->RefreshHUDPanels();
    TestFalse(TEXT("Refreshing the mobile HUD removes the disabled arrow"),Touch->IsHUDPluginVisible(TEXT("waypoint.arrow")));
    RailHost->SetWaypointOption(TEXT("arrow"),true);Touch->RefreshHUDPanels();
    TestTrue(TEXT("Re-enabling the mobile arrow restores it"),Touch->IsHUDPluginVisible(TEXT("waypoint.arrow")));
    TestTrue(TEXT("Plugin host recognizes a visible mobile readout"),RailHost->IsPluginWindowOpen(TEXT("huntledger")));
    RailHost->TogglePluginWindow(TEXT("huntledger"));
    TestTrue(TEXT("Hiding a visible readout does not open a modal"),!Touch->IsHUDPluginVisible(TEXT("huntledger")) && Touch->Panel.IsNone());
    Touch->ToggleHUDPlugin(TEXT("huntledger"));
    Touch->OpenPanel("Inventory");
    TestFalse(TEXT("Plugin HUD is not reported visible behind a modal"),RailHost->IsPluginWindowOpen(TEXT("huntledger")));
    Touch->ClosePanel();
    const auto StoredControls=Touch->Settings->Controls;
    Touch->SetPinnedChat(true);
    Client->ReceiveChatMessage(TEXT("Pinned chat fixture"),TEXT(""),2);Touch->RefreshChat();
    TestTrue(TEXT("Pinned chat receives live history without blocking movement"),Touch->PinnedChatLines.IsValid() && Touch->PinnedChatLines->NumSlots()>0 && !Touch->IsBlockingMovement());
    Touch->OpenPanel("Chat");Touch->ClosePanel();
    TestTrue(TEXT("Closing chat composer keeps the pinned transcript"),Touch->PinnedChatLines.IsValid());
    Touch->PinnedChatEntry->SetText(FText::FromString(TEXT("Mobile chat fixture")));Touch->SendPinnedChat();
    TestTrue(TEXT("Pinned chat sends through the shared chat pipeline and clears the entry"),Touch->PinnedChatDraft.IsEmpty() && Touch->PinnedChatEntry->GetText().IsEmpty());
    Touch->SetPinnedChat(false);TestFalse(TEXT("Full-screen chat option removes pinned transcript"),Touch->PinnedChatLines.IsValid());
    for(int32 I=0;I<StoredControls.Num();++I)TestTrue(TEXT("Showing readouts does not overwrite saved controls"),StoredControls[I].Position.Equals(Touch->Settings->Controls[I].Position));
    for(const auto& P:RailHost->Plugins)Touch->Settings->HiddenHUDPlugins.AddUnique(P->Id);
    Touch->RefreshHUDPanels();
    if(FApp::CanEverRender())
    {
        // Real Slate routing catches pointer-capture and hit-test failures that
        // direct handler calls cannot establish on a multitouch surface.
        Touch->ClosePanel();Touch->SurfaceResized({960,540});
        auto& App=FSlateApplication::Get();auto Window=SNew(SVirtualWindow).Size(FVector2D(960,540));Window->SetContent(Slate);App.RegisterVirtualWindow(Window);
        FWidgetRenderer RoutedRenderer(false,true);auto* RoutedTarget=FWidgetRenderer::CreateTargetFor({960,540},TF_Bilinear,false);
        ON_SCOPE_EXIT {App.ReleaseAllPointerCapture();App.UnregisterVirtualWindow(Window);RoutedTarget->ReleaseResource();};
        auto Paint=[&](){++GFrameCounter;RoutedRenderer.DrawWindow(RoutedTarget,Window->GetHittestGrid(),Window,1.f,{960,540},.016f);FlushRenderingCommands();};
        auto Path=[&](FVector2D P){return App.LocateWindowUnderMouse(P,{Window},false,0);};
        auto Down=[&](int32 Id,FVector2D P){App.RoutePointerDownEvent(Path(P),FPointerEvent(0,Id,P,P,1.f,true));};
        auto MoveFinger=[&](int32 Id,FVector2D P,FVector2D From){App.RoutePointerMoveEvent(Path(P),FPointerEvent(0,Id,P,From,1.f,true),false);};
        auto Up=[&](int32 Id,FVector2D P){App.RoutePointerUpEvent(Path(P),FPointerEvent(0,Id,P,P,0.f,false));};
        Paint();const auto MoveP=Location("Move"),TurnP=Location("Turn"),JumpP=Location("Jump");
        Down(0,MoveP);MoveFinger(0,MoveP+FVector2D(0,-60),MoveP);Down(1,TurnP);MoveFinger(1,TurnP+FVector2D(50,0),TurnP);Down(2,JumpP);
        TestTrue(TEXT("Real Slate touches move, turn, and hold jump simultaneously"),Touch->Movement().Y<-.9 && Touch->Turning()>.8 && Touch->IsJumpHeld());
        Up(1,TurnP+FVector2D(50,0));TestTrue(TEXT("Slate releases only the ending finger"),Touch->Movement().Y<-.9 && Touch->Turning()==0 && Touch->IsJumpHeld());
        Up(0,MoveP);Up(2,JumpP);TestTrue(TEXT("All real touch captures release"),Touch->Fingers.IsEmpty());
        Touch->OpenPanel("Inventory");Paint();Paint();
        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName)> Find=[&](TSharedRef<SWidget> W,FName Tag)->TSharedPtr<SWidget>
        {if(W->GetTag()==Tag)return W;auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Tag))return Found;return nullptr;};
        Touch->ClosePanel();Touch->RefreshPluginRail();Paint();Paint();
        if(auto RailButton=Find(Slate,"MobilePlugin:ucm"))
        {
            const auto P=RailButton->GetCachedGeometry().LocalToAbsolute(RailButton->GetCachedGeometry().GetLocalSize()*.5);
            Down(0,P);Up(0,P);
            TestEqual(TEXT("Plugin rail tap opens the plugin through Slate hit testing"),Touch->Panel,FName("Plugin:ucm"));
            TestTrue(TEXT("Opening plugin from rail releases movement"),Touch->Fingers.IsEmpty() && Touch->IsBlockingMovement());
        }
        else AddError(TEXT("Enabled UCM rail button is missing"));
        Touch->OpenPanel("Inventory");Paint();Paint();
        const auto ItemWidget=Find(Slate,"PackSlot_100_0");
        if(TestTrue(TEXT("Paged inventory exposes its first slot"),ItemWidget.IsValid()))
        {
            const auto P=ItemWidget->GetCachedGeometry().LocalToAbsolute(ItemWidget->GetCachedGeometry().GetLocalSize()*.5);
            Down(0,P);Up(0,P);Menu->RefreshIfDirty();Paint();
            TestEqual(TEXT("Touch tap selects an inventory item through its real hit path"),Client->GetSelectedObject().Guid,1000);
            TestTrue(TEXT("Inventory tapping never starts world movement"),Touch->Movement().IsNearlyZero());
        }
        Touch->ClosePanel();Touch->SetPinnedChat(true);Paint();Paint();
        if(auto Entry=Find(Slate,"MobilePinnedChatEntry"))
        {
            const auto P=Entry->GetCachedGeometry().LocalToAbsolute(Entry->GetCachedGeometry().GetLocalSize()*.5);
            Down(0,P);Up(0,P);Paint();
            TestTrue(TEXT("Touching pinned chat focuses typing and blocks movement"),Touch->PinnedChatEntry->HasKeyboardFocus() && Touch->IsBlockingMovement());
            TestTrue(TEXT("Typing never leaves held movement fingers"),Touch->Fingers.IsEmpty());
        }
        else AddError(TEXT("Pinned chat input missing from real Slate tree"));
        Touch->SetPinnedChat(false);TestFalse(TEXT("Unpinning chat releases input focus"),Touch->IsBlockingMovement());
    }
    if(FApp::CanEverRender())
    {
        FWidgetRenderer Renderer(false,true);const FString Directory=FPaths::ProjectSavedDir()/TEXT("Automation/Mobile");IFileManager::Get().MakeDirectory(*Directory,true);
        for(const FIntPoint Size:{FIntPoint(1280,720),FIntPoint(2520,1080),FIntPoint(2184,1968),FIntPoint(1024,768)})
        {
            Touch->SurfaceResized(ACEMobile::LogicalSize(FVector2D(Size)));
            Touch->RefreshPluginRail();
            for(const FName Page:{FName(),FName("Wheel"),FName("Menu"),FName("Inspect"),FName("Plugins"),FName("Plugin:ucm"),FName("Inventory"),FName("Spellbook"),FName("Rebind"),FName("Combat settings"),FName("Scaled controls"),FName("Pinned panels"),FName("Pinned chat"),FName("Jump charge")})
            {
                const auto SavedLayout=Touch->Layout;
                if(Page=="Pinned panels" || Page=="Pinned chat")
                {
                    Touch->ClosePanel();
                    FACEPosition Position;Position.CellId=0x7D64000D;Position.Location=FVector(60,80,0);Session.SetLocalPosition(Position);
                    for(const FString Id:{TEXT("waypoint"),TEXT("huntledger"),TEXT("supplyledger")})Touch->Settings->HiddenHUDPlugins.Remove(Id);
                    Touch->SetPinnedChat(Page=="Pinned chat");Touch->RefreshHUDPanels();
                }
                else if(Page=="Jump charge")
                {Touch->ClosePanel();PC->bJumpCharging=true;PC->JumpChargeExtent=.65f;}
                else if(Page=="Scaled controls")
                {
                    Touch->ClosePanel();Touch->Layout.Reset();
                    auto Add=[&](FName Id,FName Action,float X,float Y,float Width){FACETouchControl C;C.Id=Id;C.Action=Action;C.Position={X,Y};C.Size=Width;Touch->Layout.Add(C);};
                    Add("Move","Move",.2f,.67f,224);Add("Turn","Turn",.86f,.34f,160);
                    Add("Inspect","Inspect",.46f,.34f,56);Add("Mode","Combat",.58f,.34f,64);Add("Use","Use",.46f,.72f,80);
                    Add("Self","Self",.64f,.72f,120);Add("Ability","Attack",.85f,.72f,160);
                    ACEMobile::FitLayout(Touch->Layout,Touch->ViewSize);Touch->RefreshPresentation();
                }
                else if(Page.IsNone())Touch->ClosePanel();else if(Page=="Wheel"){Touch->ClosePanel();Touch->Action("Spell wheel");}else {Touch->SelectedControl=2;Touch->OpenPanel(Page);}
                auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(Size),TF_Bilinear,false);
                for(int32 Pass=0;Pass<3;++Pass){Renderer.DrawWidget(Target,Slate,FVector2D(Size),.016f);FlushRenderingCommands();}
                TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(true);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
                TestTrue(TEXT("Touch presentation draws readable foreground"),Pixels.ContainsByPredicate([](FColor P){return P.R>150 && P.G>150 && P.B>100;}));
                TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
                FFileHelper::SaveArrayToFile(PNG,*(Directory/FString::Printf(TEXT("%s-%dx%d.png"),Page.IsNone()?TEXT("Controls"):*Page.ToString().Replace(TEXT(":"),TEXT("-")),Size.X,Size.Y)));
                Target->ReleaseResource();
                PC->bJumpCharging=false;
                for(const auto& P:RailHost->Plugins)Touch->Settings->HiddenHUDPlugins.AddUnique(P->Id);
                Touch->SetPinnedChat(false);
                Touch->Layout=SavedLayout;Touch->RefreshPresentation();
            }
        }
    }
    Touch->ClosePanel();Session.State=EACESessionState::CharacterSelect;Touch->UpdateMobile();
    TestEqual(TEXT("Logout replaces gameplay with touch roster"),Touch->Panel,FName("Characters"));
    TestTrue(TEXT("Logout clears old inventory binder"),Touch->Menu==nullptr);
    TestTrue(TEXT("Lobby cannot move the character"),Touch->IsBlockingMovement());
    Touch->StartCharacterCreation();
    TestTrue(TEXT("New accounts can create characters using retail data"),Touch->Creation.IsValid());
    Touch->Creation->Selection.Name=TEXT("Touch Tester");FString CreationError;
    TestTrue(TEXT("Touch creator retains a valid retail character draft"),Touch->Creation->Validate(CreationError));
    if(FApp::CanEverRender())
    {
        const FVector2D Size(1280,720);FWidgetRenderer Renderer(false,true);
        auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,false);
        auto Capture=[&](TSharedRef<SWidget> Widget,const TCHAR* Name)
        {
            // A synchronous fixture must finish the editor's asynchronous shaders
            // and advance scene frames, just as the running game does.
            if(GShaderCompilingManager)GShaderCompilingManager->FinishAllCompilation();
            for(int32 Pass=0;Pass<5;++Pass){++GFrameCounter;World->SendAllEndOfFrameUpdates();if(Touch->CreationCapture)Touch->CreationCapture->CaptureScene();Renderer.DrawWidget(Target,Widget,Size,.016f);FlushRenderingCommands();}
            TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(true);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(1280,720,Pixels,PNG);FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/Mobile")/(FString(Name)+TEXT("-1280x720.png"))));
        };
        Capture(Slate,TEXT("Create"));Touch->CreationTab=4;Touch->BuildPanel();Capture(Slate,TEXT("Appearance"));
        TestNotNull(TEXT("Mobile creator renders a DAT character preview"),Touch->CreationActor.Get());
        TArray<FColor> PreviewPixels;Touch->CreationTarget->GameThread_GetRenderTargetResource()->ReadPixels(PreviewPixels);
        int32 LitPixels=0;for(const auto P:PreviewPixels)if(FMath::Max3(P.R,P.G,P.B)>40)++LitPixels;
        TestTrue(TEXT("Character preview contains colored geometry, not a black silhouette"),LitPixels>500);
        auto* Defaults=GetMutableDefault<UACEMobileSettings>();const bool OldEnabled=Defaults->bEnabled;Defaults->bEnabled=true;
        ON_SCOPE_EXIT {Defaults->bEnabled=OldEnabled;};
        auto* Login=CreateWidget<UACELoginWidget>(PC);Login->ProfilePath=FPaths::ProjectSavedDir()/TEXT("Automation/MobileLogin.dat");
        auto LoginSlate=Login->TakeWidget();Login->RunAction(TEXT("play"));
        for(int32 Pass=0;Pass<6;++Pass){Renderer.DrawWidget(Target,LoginSlate,Size,.016f);FlushRenderingCommands();Login->NativeTick(Login->GetCachedGeometry(),.016f);}
        Capture(LoginSlate,TEXT("Login"));
        TestTrue(TEXT("Launcher keeps readable mobile scale"),Login->LoginButton->GetCachedGeometry().GetAbsoluteSize().Y>=40);
        Login->NativeDestruct();Target->ReleaseResource();
    }
    return true;
}
#endif
