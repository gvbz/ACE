#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "ACEInputBindings.h"
#include "ACEPlayerController.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "RetailCustomKeymap.inl"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "UI/ACERetailTextEntry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEInputBindingsTest,"ACE.RetailParity.KeyboardBindings",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEInputBindingsTest::RunTest(const FString&)
{
 const FString Original=GGameUserSettingsIni;
 GGameUserSettingsIni=FPaths::ProjectSavedDir()/TEXT("Automation/KeyboardFixture.ini");
 FConfigFile Config;Config.NoSave=false;Config.bCanSaveAllSections=true;
 GConfig->SetFile(GGameUserSettingsIni,&Config);ACEInputBindings::Reload();
 const auto Values=UWorld::InitializationValues().CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
 auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
 auto* PC=World->SpawnActor<APlayerController>();PC->PlayerInput=NewObject<UPlayerInput>(PC);
 ON_SCOPE_EXIT{World->DestroyWorld(false);GGameUserSettingsIni=Original;ACEInputBindings::Reload();};
 auto* Movement=World->SpawnActor<AACEPlayerController>();
 auto Forward=[&](float Value,bool Toggle){Movement->UpdateKeyboardAutoRun(Value,Toggle);return Value;};
 Forward(1,false); Forward(1,true); Forward(1,true); Forward(1,false);
 TestTrue(TEXT("Autorun takes over while forward is already held"),Movement->bAutoRun);
 TestEqual(TEXT("Releasing forward keeps autorun moving"),Forward(0,false),1.f);
 Forward(1,false);TestFalse(TEXT("A new forward press cancels autorun"),Movement->bAutoRun);
 Forward(0,false);Forward(0,true);Forward(0,false);Forward(-1,false);
 TestFalse(TEXT("A new backward press cancels autorun"),Movement->bAutoRun);
 Forward(0,true);Forward(0,false);Forward(0,true);
 TestFalse(TEXT("Second autorun press toggles it off"),Movement->bAutoRun);
 auto Hold=[&](TArray<FKey> Keys)
 {
  PC->PlayerInput->FlushPressedKeys();
  for(FKey Key:Keys)PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key,IE_Pressed,1.f));
  PC->PlayerInput->ProcessInputStack({},.016f,false);
 };
 {
  auto& Slate=FSlateApplication::Get();const auto PreviousFocus=Slate.GetKeyboardFocusedWidget();
  TSharedPtr<SEditableTextBox> Entry;
  auto Window=SNew(SWindow).ClientSize(FVector2D(300,80))[SAssignNew(Entry,SEditableTextBox)];
  Slate.AddWindow(Window,false);Slate.SetKeyboardFocus(Entry,EFocusCause::SetDirectly);
  TestTrue(TEXT("Plugin edit field is recognized without native chat focus"),ACEInputBindings::IsTextEntryFocused());
  Hold({EKeys::W,EKeys::E});
  TestFalse(TEXT("Typing forward key cannot move the player"),ACEInputBindings::Down(PC,EKeys::W));
  TestFalse(TEXT("Typing inspect key cannot trigger an action"),ACEInputBindings::Pressed(PC,EKeys::F));
  Movement->PlayerInput=NewObject<UPlayerInput>(Movement);
  TestTrue(TEXT("Plugin text key-down is consumed before gameplay input"),Movement->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W,IE_Pressed,1.f)));
  TestFalse(TEXT("Text entry cannot leave forward key latched"),Movement->IsInputKeyDown(EKeys::W));
  Slate.ClearKeyboardFocus();Hold({EKeys::W});
  TestTrue(TEXT("Gameplay resumes when text entry loses focus"),ACEInputBindings::Down(PC,EKeys::W));
  Slate.RequestDestroyWindow(Window);if(PreviousFocus)Slate.SetKeyboardFocus(PreviousFocus);
 }
 {
  // Journal, Page List, Fellowship, Friends and Squelch share this DAT editor.
  auto& Slate=FSlateApplication::Get();const auto PreviousFocus=Slate.GetKeyboardFocusedWidget();
  auto* Entry=NewObject<UACERetailTextEntry>();const auto Widget=Entry->TakeWidget();
  auto Window=SNew(SWindow).ClientSize(FVector2D(300,80))[Widget];Slate.AddWindow(Window,false);
  for(bool Multiline:{false,true})
  {
   Entry->bMultiline=Multiline;Entry->SetText(FText::GetEmpty());Slate.SetKeyboardFocus(Widget);
   TestTrue(TEXT("Retail bitmap text editor blocks gameplay polling"),ACEInputBindings::IsTextEntryFocused());
   Hold({EKeys::W,EKeys::E});
   TestFalse(TEXT("Retail field suppresses held movement"),ACEInputBindings::Down(PC,EKeys::W));
   TestFalse(TEXT("Retail field suppresses examine"),ACEInputBindings::Pressed(PC,ACEInputBindings::Action(TEXT("Examine"))));
   for(FKey Key:{EKeys::W,EKeys::A,EKeys::S,EKeys::D,EKeys::R})
   {
    TestTrue(TEXT("Retail key down does not bubble to panel/viewport actions"),Widget->OnKeyDown(FGeometry(),FKeyEvent(Key,FModifierKeysState(),0,false,0,0)).IsEventHandled());
    Movement->InputKey(FInputKeyEventArgs::CreateSimulated(Key,IE_Pressed,1.f));
    Movement->PlayerInput->ProcessInputStack({},.016f,false);
    TestFalse(TEXT("Typing cannot latch a gameplay key"),Movement->IsInputKeyDown(Key));
   }
   for(TCHAR C:FString(TEXT("wasdr")))Slate.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
   TestEqual(TEXT("Letters still enter the retail field"),Entry->GetText().ToString(),FString(TEXT("wasdr")));
   // Releases must reach PlayerInput even when a field gained focus mid-hold.
   Movement->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W,IE_Pressed,1.f));
   Movement->PlayerInput->ProcessInputStack({},.016f,false);
   Movement->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W,IE_Released,0.f));
   Movement->PlayerInput->ProcessInputStack({},.016f,false);
   TestFalse(TEXT("Focus during a held key cannot strand movement after editing"),Movement->IsInputKeyDown(EKeys::W));
   Slate.ClearKeyboardFocus();
  }
  Hold({EKeys::W});TestTrue(TEXT("Gameplay resumes after retail editing"),ACEInputBindings::Down(PC,EKeys::W));
  Slate.RequestDestroyWindow(Window);if(PreviousFocus)Slate.SetKeyboardFocus(PreviousFocus);
 }
 const auto QuickShow=ACEInputBindings::Action(TEXT("QuickShowUI"));
 Hold({EKeys::LeftControl});TestTrue(TEXT("Left Control quick-shows pinned windows by default"),ACEInputBindings::Down(PC,QuickShow));
 Hold({});TestFalse(TEXT("Releasing hold ends quick-show"),ACEInputBindings::Down(PC,QuickShow));
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(QuickShow,0,FInputChord(EKeys::F11));ACEInputBindings::Commit();
 Hold({EKeys::LeftControl});TestFalse(TEXT("Rebinding removes the original quick-show key"),ACEInputBindings::Down(PC,QuickShow));
 Hold({EKeys::F11});TestTrue(TEXT("Rebound hold quick-shows pinned windows"),ACEInputBindings::Down(PC,QuickShow));
 ACEInputBindings::Reload();TestTrue(TEXT("Quick-show binding persists"),ACEInputBindings::Down(PC,QuickShow));
 // Shipped DAT gmDefaultMap (14000000) and DefaultMap (14000002),
 // rather than the later WASD bindings that were invented by this client.
 Hold({EKeys::X});TestTrue(TEXT("Retail X moves backward"),ACEInputBindings::Down(PC,EKeys::S));
 Hold({EKeys::Z});TestTrue(TEXT("Retail Z strafes left"),ACEInputBindings::Down(PC,EKeys::Q));
 const auto ToggleInterface=ACEInputBindings::Action(TEXT("ToggleInterface"));
 TestFalse(TEXT("Plain Z does not hide the interface"),ACEInputBindings::Down(PC,ToggleInterface));
 for(FKey Alt:{EKeys::LeftAlt,EKeys::RightAlt})
 {
  Hold({Alt,EKeys::Z});
  TestTrue(TEXT("Either Alt+Z toggles the interface"),ACEInputBindings::Pressed(PC,ToggleInterface));
  TestFalse(TEXT("Alt+Z does not also strafe"),ACEInputBindings::Down(PC,EKeys::Q));
 }
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(ToggleInterface,0,FInputChord(EKeys::F10,false,true,false,false));ACEInputBindings::Commit();
 ACEInputBindings::Reload();Hold({EKeys::LeftControl,EKeys::F10});
 TestTrue(TEXT("Interface toggle rebind persists across reload"),ACEInputBindings::Pressed(PC,ToggleInterface));
 Hold({EKeys::LeftAlt,EKeys::Z});
 TestFalse(TEXT("Old interface shortcut is released after rebinding"),ACEInputBindings::Down(PC,ToggleInterface));
 ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
 Hold({EKeys::C});TestTrue(TEXT("Retail C strafes right"),ACEInputBindings::Down(PC,EKeys::E));
 TestFalse(TEXT("Retail C no longer crouches"),ACEInputBindings::Down(PC,EKeys::C));
 Hold({EKeys::Q});TestTrue(TEXT("Retail Q autoruns"),ACEInputBindings::Down(PC,EKeys::NumLock));
 Hold({EKeys::LeftShift,EKeys::W});TestTrue(TEXT("Shift preserves forward movement"),ACEInputBindings::Down(PC,EKeys::W));
 TestTrue(TEXT("Shift supplies walk modifier"),ACEInputBindings::Down(PC,EKeys::LeftShift));
 Hold({EKeys::LeftAlt,EKeys::A});TestTrue(TEXT("Alt+A strafes left"),ACEInputBindings::Down(PC,EKeys::Q));
 TestFalse(TEXT("Alt+A cannot also turn left"),ACEInputBindings::Down(PC,EKeys::A));
 for(int32 I=0;I<9;++I)
 {
  const auto First=ACEInputBindings::Get(ACEInputBindings::Shortcut(I),0);
  Hold({EKeys::LeftAlt,First.Key});
  TestTrue(TEXT("Alt number reaches second shortcut row"),ACEInputBindings::Down(PC,ACEInputBindings::Shortcut(I+9)));
  TestFalse(TEXT("Alt number does not also fire first row"),ACEInputBindings::Down(PC,ACEInputBindings::Shortcut(I)));
 }
 TestEqual(TEXT("Retail use key"),ACEInputBindings::Get(EKeys::F,0).Key,EKeys::R);
 TestEqual(TEXT("Retail examine key"),ACEInputBindings::Get(ACEInputBindings::Action(TEXT("Examine")),0).Key,EKeys::E);
 TestEqual(TEXT("Retail inventory key"),ACEInputBindings::Get(EKeys::I,0).Key,EKeys::F12);
 TestEqual(TEXT("Retail zoom-in is numpad minus"),ACEInputBindings::Get(EKeys::Add,0).Key,EKeys::Subtract);
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::W,0,FInputChord(EKeys::F7));
 ACEInputBindings::Set(EKeys::W,1,FInputChord());ACEInputBindings::Commit();
 Hold({EKeys::W,EKeys::Up});TestFalse(TEXT("Rebinding removes old primary and secondary movement keys"),ACEInputBindings::Down(PC,EKeys::W));
 Hold({EKeys::F7});TestTrue(TEXT("New binding reaches actual input poll"),ACEInputBindings::Down(PC,EKeys::W));
 FConfigFile Saved;Saved.Read(GGameUserSettingsIni);GConfig->SetFile(GGameUserSettingsIni,&Saved);ACEInputBindings::Reload();
 TestTrue(TEXT("Reload retains the working rebind"),ACEInputBindings::Down(PC,EKeys::W));
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::S,2,FInputChord(EKeys::F7));
 TestFalse(TEXT("Duplicate chord removed from previous action"),ACEInputBindings::Get(EKeys::W,0).Key.IsValid());
 ACEInputBindings::Revert();TestEqual(TEXT("Revert restores active bindings"),ACEInputBindings::Get(EKeys::W,0).Key,EKeys::F7);
 ACEInputBindings::Defaults();TestEqual(TEXT("Defaults restores retail secondary arrow"),ACEInputBindings::Get(EKeys::W,1).Key,EKeys::Up);
 ACEInputBindings::Cancel();TestEqual(TEXT("Cancel leaves saved customization intact"),ACEInputBindings::Get(EKeys::W,0).Key,EKeys::F7);
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(ACEInputBindings::Action(TEXT("Chat")),0,FInputChord(EKeys::F6));ACEInputBindings::Commit();
 TestTrue(TEXT("Slate chat uses the same rebind"),ACEInputBindings::Matches(ACEInputBindings::Action(TEXT("Chat")),FInputChord(EKeys::F6)));
 TestFalse(TEXT("Old chat primary can be removed"),ACEInputBindings::Matches(ACEInputBindings::Action(TEXT("Chat")),FInputChord(EKeys::Enter)));
 ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();
 const FString Retail=TEXT(R"KEYMAP(
 # Representative retail CMasterInputMap export: device order is intentionally reversed.
 "User Defined Keymap" [ 00000000-0000-0000-0000-000000000000 ]
 Devices [ Mouse [ GUID_SysMouse ] Keyboard [ GUID_SysKeyboard ] Joystick [ Test ] ]
 MetaKeys [ 1 [ 1 DIK_LSHIFT ] 2 [ 1 DIK_LCONTROL ] 3 [ 1 DIK_LMENU ] 5 [ 0 DIMOFS_BUTTON3 ] ]
 Bindings [
 MovementCommands [
  MovementBackup [ "" [ 1 DIK_S ] ] MovementBackup [ "" [ 1 DIK_DOWNARROW ] ]
  MovementStrafeLeft [ "" [ 1 DIK_Q ] ] MovementStrafeLeft [ "" [ 1 DIK_A ] 0x00000004 ]
  MovementTurnLeft [ "" [ 1 DIK_A ] ] MovementTurnLeft [ "" [ 1 DIK_LEFT ] ]
  MovementRunLock [ "" [ 0 DIMOFS_BUTTON3 ] ]
  DoNothing [ "" [ 1 DIK_X ] ]
 ]
 CameraControls [ CameraRotateLeft [ "" [ 1 DIK_NUMPAD4 ] ] ]
 CameraAlternateControls [ CameraRotateLeft [ "" [ 1 DIK_LEFT ] ] ]
 MeleeCombat [ CombatLowAttack [ "" [ 1 DIK_F7 ] ] ]
 MagicCombat [ CombatCastCurrentSpell [ "" [ 1 DIK_F7 ] ] UseSpellSlot_1 [ "" [ 1 DIK_1 ] ] ]
 UICommands [ USE [ "" [ 1 DIK_R ] ] EscapeKey [ "" [ 1 DIK_ESCAPE ] ] ToggleHelp [ "" [ 1 DIK_F1 ] ] ]
 Emotes [ Wave [ "" [ 1 DIK_J ] 0x00000010 ] Cry [ "" [ 2 JOY_BUTTON1 ] ] ]
 EditControls [ EscapeKey [ "" [ 1 DIK_F10 ] ] ]
 ]
 )KEYMAP");
 const auto Imported=ACEInputBindings::ImportRetailKeymap(Retail);
 if(!Imported.bSuccess)AddError(Imported.Error);
 TestTrue(TEXT("Retail text keymap imports"),Imported.bSuccess);
 TestTrue(TEXT("Unsupported actions, devices and modifiers are reported"),Imported.Skipped.Num()>=3);
 TestEqual(TEXT("Separate native contexts are reported separately"),Imported.UnchangedContexts.Num(),2);
 TestEqual(TEXT("Import is a draft and keeps retail S backward"),ACEInputBindings::Get(EKeys::S,0).Key,EKeys::S);
 TestEqual(TEXT("Mouse autorun imports using the declared device index"),ACEInputBindings::Get(EKeys::NumLock,0).Key,EKeys::ThumbMouseButton);
 TestTrue(TEXT("Retail file mask bit 2 names MetaKeys ordinal 3, preserving Alt-A"),ACEInputBindings::Get(EKeys::Q,1)==FInputChord(EKeys::A,false,false,true,false));
 TestFalse(TEXT("Unlisted movement actions are unbound when their retail input map is replaced"),ACEInputBindings::Get(EKeys::W,0).Key.IsValid());
 TestFalse(TEXT("Imported backward binding removes default Stop conflict"),ACEInputBindings::Get(ACEInputBindings::Action(TEXT("Stop")),0).Key.IsValid());
 TestEqual(TEXT("Alternate camera context cannot steal Left from movement"),ACEInputBindings::Get(EKeys::A,1).Key,EKeys::Left);
 TestEqual(TEXT("Text-widget Escape does not override gameplay Escape"),ACEInputBindings::Get(EKeys::Escape,0).Key,EKeys::Escape);
 const auto Before=ACEInputBindings::Get(EKeys::S,0);
 TestFalse(TEXT("Truncated keymap rejected"),ACEInputBindings::ImportRetailKeymap(Retail.LeftChop(3)).bSuccess);
 TestTrue(TEXT("Malformed import leaves draft unchanged"),ACEInputBindings::Get(EKeys::S,0)==Before);
 ACEInputBindings::Commit();Hold({EKeys::F7});ACEInputBindings::SetCombatContext(2);
 TestTrue(TEXT("Melee context retains imported attack"),ACEInputBindings::Down(PC,ACEInputBindings::Action(TEXT("MeleeLow"))));
 TestFalse(TEXT("Magic binding inactive in melee"),ACEInputBindings::Down(PC,ACEInputBindings::Action(TEXT("SpellCast"))));
 ACEInputBindings::SetCombatContext(8);
 TestTrue(TEXT("Same physical key independently casts in magic"),ACEInputBindings::Down(PC,ACEInputBindings::Action(TEXT("SpellCast"))));
 Hold({EKeys::One});TestTrue(TEXT("Magic slot takes priority over ordinary shortcut"),ACEInputBindings::Down(PC,ACEInputBindings::SpellSlot(0)));
 TestFalse(TEXT("Spell slot cannot also use an inventory item"),ACEInputBindings::Down(PC,ACEInputBindings::Shortcut(0)));
 Hold({EKeys::LeftControl,EKeys::One});TestTrue(TEXT("Ctrl shortcut still works while in magic"),ACEInputBindings::Down(PC,ACEInputBindings::Shortcut(0)));
 TestFalse(TEXT("Ctrl shortcut cannot also cast"),ACEInputBindings::Down(PC,ACEInputBindings::SpellSlot(0)));
 ACEInputBindings::BeginEdit();ACEInputBindings::ImportRetailKeymap(Retail);ACEInputBindings::Cancel();
 TestEqual(TEXT("Cancel preserves prior live import"),ACEInputBindings::Get(EKeys::S,0).Key,EKeys::S);
 ACEInputBindings::BeginEdit();
 const FString ExportPath=FPaths::ProjectSavedDir()/TEXT("Automation/RoundTrip.keymap");FString ExportError;
 for(int32 Slot=9;Slot<12;++Slot)
 {
  TestTrue(TEXT("All twelve spell actions are listed for rebinding"),ACEInputBindings::Actions().ContainsByPredicate([&](const auto& A){return A.Key==ACEInputBindings::SpellSlot(Slot);}));
  ACEInputBindings::Set(ACEInputBindings::SpellSlot(Slot),0,FInputChord(Slot==9?EKeys::Zero:Slot==10?EKeys::Hyphen:EKeys::Equals));
 }
 ACEInputBindings::Set(ACEInputBindings::Action(TEXT("ToggleChat")),0,FInputChord(EKeys::F12));
 const FKey ToggleUCM=ACEInputBindings::Action(TEXT("ToggleUCM"));
 ACEInputBindings::Set(ToggleUCM,0,FInputChord(EKeys::F10));
 TestTrue(TEXT("UCM action appears in keybinding UI"),ACEInputBindings::Actions().ContainsByPredicate([&](const auto& A){return A.Key==ToggleUCM;}));
 TestTrue(TEXT("UCM start/stop accepts custom bindings"),ACEInputBindings::Get(ToggleUCM,0)==FInputChord(EKeys::F10));
 TestTrue(TEXT("Save As writes a retail-format keymap"),ACEInputBindings::ExportRetailKeymapFile(ExportPath,ExportError));
 ACEInputBindings::Defaults();
 const auto RoundTrip=ACEInputBindings::ImportRetailKeymapFile(ExportPath);
 TestTrue(TEXT("Exported file reloads"),RoundTrip.bSuccess);
 for(int32 Slot=9;Slot<12;++Slot)TestEqual(TEXT("Spell slots 10–12 survive keymap export/import"),ACEInputBindings::Get(ACEInputBindings::SpellSlot(Slot),0).Key,Slot==9?EKeys::Zero:Slot==10?EKeys::Hyphen:EKeys::Equals);
 TestEqual(TEXT("Toggle Chat Entry is rebindable and survives keymap export/import"),ACEInputBindings::Get(ACEInputBindings::Action(TEXT("ToggleChat")),0).Key,EKeys::F12);
 TestTrue(TEXT("Retail keymap import retains the local interface shortcut"),ACEInputBindings::Get(ToggleInterface,0)==FInputChord(EKeys::Z,false,false,true,false));
 TestTrue(TEXT("Retail keymap import retains local quick-show binding"),ACEInputBindings::Get(QuickShow,0)==FInputChord(EKeys::LeftControl));
 TestTrue(TEXT("Save and load preserve modifier bindings"),ACEInputBindings::Get(EKeys::Q,1)==FInputChord(EKeys::A,false,false,true,false));
 TestFalse(TEXT("Save and load preserve unbound actions without invalid empty control records"),ACEInputBindings::Get(EKeys::W,0).Key.IsValid());
 TestEqual(TEXT("Save and load preserve mouse bindings"),ACEInputBindings::Get(EKeys::NumLock,0).Key,EKeys::ThumbMouseButton);
 ACEInputBindings::Cancel();
 ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();
 const auto Custom=ACEInputBindings::ImportRetailKeymap(RetailCustomKeymap);
 TestTrue(TEXT("User retail keymap imports"),Custom.bSuccess);
 TestEqual(TEXT("All supported user-file action bindings import"),Custom.BindingCount,157);
 TestEqual(TEXT("Remaining unsupported controls are reported individually"),Custom.Skipped.Num(),18);
 TestEqual(TEXT("Retail map view imports its keypad Enter"),ACEInputBindings::Get(ACEInputBindings::Action(TEXT("CameraViewMapMode")),0).Key,ACEInputBindings::NumpadEnterKey());
 TestEqual(TEXT("All explicit supported DoNothing overrides import, including chat toggles"),ACEInputBindings::GetBlockedBindings().Num(),19);
 TestEqual(TEXT("Native input contexts are accounted for separately"),Custom.UnchangedContexts.Num(),8);
 AddInfo(FString::Printf(TEXT("Custom keymap: %d imported, %d diagnostics"),Custom.BindingCount,Custom.Skipped.Num()));
 for(const auto& Entry:Custom.Skipped)AddInfo(Entry);
 TestEqual(TEXT("Self binding from user file"),ACEInputBindings::Get(ACEInputBindings::Action(TEXT("SelectionSelf")),0).Key,EKeys::NumPadOne);
 TestEqual(TEXT("Windows applications key imports for contracts"),ACEInputBindings::Get(EKeys::U,0).Key,ACEInputBindings::ApplicationsKey());
 TestEqual(TEXT("Mouse wheel imports as directional zoom"),ACEInputBindings::Get(EKeys::Add,0).Key,EKeys::MouseScrollUp);
 TestEqual(TEXT("Custom friend panel does not become allegiance"),ACEInputBindings::Get(ACEInputBindings::Action(TEXT("ToggleFriendsPanel")),0).Key,EKeys::F3);
 ACEInputBindings::Commit();ACEInputBindings::SetCombatContext(1);
 Hold({EKeys::LeftControl,EKeys::Up});TestFalse(TEXT("DoNothing Ctrl+Up blocks forward fallback"),ACEInputBindings::Down(PC,EKeys::W));
 Hold({EKeys::LeftAlt,EKeys::A});TestFalse(TEXT("DoNothing Alt+A blocks turning"),ACEInputBindings::Down(PC,EKeys::A));
 TestFalse(TEXT("DoNothing Alt+A blocks old strafe default"),ACEInputBindings::Down(PC,EKeys::Q));
 Hold({EKeys::Up});TestTrue(TEXT("Plain Up still moves"),ACEInputBindings::Down(PC,EKeys::W));
 Hold({EKeys::LeftAlt,EKeys::Up});TestTrue(TEXT("Alt+Up still autoruns"),ACEInputBindings::Down(PC,EKeys::NumLock));
 TestFalse(TEXT("Alt+Up does not also move"),ACEInputBindings::Down(PC,EKeys::W));
 Hold({EKeys::Slash});TestTrue(TEXT("Custom slash selects a player"),ACEInputBindings::Down(PC,ACEInputBindings::Action(TEXT("SelectionClosestPlayer"))));
 TestFalse(TEXT("Custom slash no longer opens chat"),ACEInputBindings::Down(PC,ACEInputBindings::Action(TEXT("Chat"))));
 Hold({EKeys::LeftControl,EKeys::LeftShift,EKeys::One});TestTrue(TEXT("Custom second-row shortcut survives"),ACEInputBindings::Down(PC,ACEInputBindings::Shortcut(9)));
 Hold({EKeys::LeftAlt,EKeys::One});TestTrue(TEXT("Alt+1 opens floating chat from this file"),ACEInputBindings::Down(PC,ACEInputBindings::Action(TEXT("ToggleFloatingChatWindow1"))));
 TestFalse(TEXT("Floating chat binding does not also use a shortcut"),ACEInputBindings::Down(PC,ACEInputBindings::Shortcut(9)));
 ACEInputBindings::Reload();
 Hold({EKeys::LeftControl,EKeys::Up});TestFalse(TEXT("DoNothing survives settings reload"),ACEInputBindings::Down(PC,EKeys::W));
 ACEInputBindings::BeginEdit();
 TestTrue(TEXT("Custom bindings export"),ACEInputBindings::ExportRetailKeymapFile(ExportPath,ExportError));
 ACEInputBindings::Defaults();TestTrue(TEXT("Custom bindings reimport"),ACEInputBindings::ImportRetailKeymapFile(ExportPath).bSuccess);
 ACEInputBindings::Commit();Hold({EKeys::LeftControl,EKeys::Up});
 TestFalse(TEXT("DoNothing survives retail export/import"),ACEInputBindings::Down(PC,EKeys::W));
 TestTrue(TEXT("Mouse wheel survives retail export/import"),ACEInputBindings::Matches(EKeys::Add,FInputChord(EKeys::MouseScrollUp)));
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::W,2,FInputChord(EKeys::Up,false,true,false,false));ACEInputBindings::Commit();
 TestTrue(TEXT("Explicit rebind can replace DoNothing"),ACEInputBindings::Down(PC,EKeys::W));
 ACEInputBindings::Cancel();
 // The active file's identity is part of the same draft/commit transaction as
 // its chords, not transient keyboard-window state.
 ACEInputBindings::BeginEdit();
 TestTrue(TEXT("Import records a draft filename"),ACEInputBindings::ImportRetailKeymapFile(ExportPath).bSuccess);
 const FString ImportedName=FPaths::GetCleanFilename(ExportPath);
 TestEqual(TEXT("Imported filename excludes the host path"),ACEInputBindings::GetKeymapFileName(),ImportedName);
 ACEInputBindings::Commit();
 FConfigFile Reloaded;Reloaded.Read(GGameUserSettingsIni);GConfig->SetFile(GGameUserSettingsIni,&Reloaded);ACEInputBindings::Reload();
 TestEqual(TEXT("Loaded keymap name survives a process-style settings reload"),ACEInputBindings::GetKeymapFileName(),ImportedName);
 ACEInputBindings::BeginEdit();ACEInputBindings::SetKeymapFileName(TEXT("Cancelled.keymap"));ACEInputBindings::Cancel();
 TestEqual(TEXT("Cancel restores the applied filename"),ACEInputBindings::GetKeymapFileName(),ImportedName);
 ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();
 TestEqual(TEXT("Defaults names the default mapping"),ACEInputBindings::GetKeymapFileName(),FString(TEXT("acclient.keymap")));
 ACEInputBindings::Revert();TestEqual(TEXT("Revert restores the applied filename"),ACEInputBindings::GetKeymapFileName(),ImportedName);
 TestFalse(TEXT("Failed import cannot replace the filename"),ACEInputBindings::ImportRetailKeymapFile(ExportPath+TEXT(".missing")).bSuccess);
 TestEqual(TEXT("Failed import retains the filename"),ACEInputBindings::GetKeymapFileName(),ImportedName);
 ACEInputBindings::Defaults();
 const auto NextSpell=ACEInputBindings::Action(TEXT("SpellNext"));
 ACEInputBindings::Set(NextSpell,0,FInputChord(EKeys::MouseScrollDown));ACEInputBindings::Commit();ACEInputBindings::SetCombatContext(8);
 Hold({EKeys::MouseScrollDown});
 TestTrue(TEXT("Wheel can select the next spell in magic mode"),ACEInputBindings::Pressed(PC,NextSpell));
 TestFalse(TEXT("Combat wheel action overrides general zoom"),ACEInputBindings::Matches(EKeys::Subtract,FInputChord(EKeys::MouseScrollDown)));
 ACEInputBindings::SetCombatContext(1);
 TestTrue(TEXT("Outside magic mode the same wheel still uses its general binding"),ACEInputBindings::Matches(EKeys::Subtract,FInputChord(EKeys::MouseScrollDown)));
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::NumLock,0,FInputChord(EKeys::MouseScrollUp));ACEInputBindings::Commit();
 Hold({EKeys::MouseScrollUp});
 TestTrue(TEXT("Wheel pulses also reach held-input actions such as autorun"),ACEInputBindings::Down(PC,EKeys::NumLock));
 TestFalse(TEXT("Rebinding wheel removes default zoom"),ACEInputBindings::Matches(EKeys::Add,FInputChord(EKeys::MouseScrollUp)));
 PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseScrollUp,IE_Released,0.f));
 PC->PlayerInput->ProcessInputStack({},.016f,false);PC->PlayerInput->ProcessInputStack({},.016f,false);
 TestFalse(TEXT("Wheel action cannot remain held"),ACEInputBindings::Down(PC,EKeys::NumLock));
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::Add,0,FInputChord(EKeys::F6));ACEInputBindings::Commit();
 Hold({EKeys::F6});TestTrue(TEXT("Zoom can be bound to an ordinary key"),ACEInputBindings::Down(PC,EKeys::Add));
 ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
 TestEqual(TEXT("Controller default does not replace the third retail strafe chord"),ACEInputBindings::Get(EKeys::Q,2),FInputChord(EKeys::Left,false,false,true,false));
 TestEqual(TEXT("Controller has its own fourth binding slot"),ACEInputBindings::Get(EKeys::Q,3).Key,EKeys::Gamepad_LeftStick_Left);
 for(int32 Mode:{2,4,8})
 {
  ACEInputBindings::SetCombatContext(Mode);Hold({EKeys::Gamepad_RightTrigger});
  const auto Attack=ACEInputBindings::Action(Mode==2?TEXT("MeleeMedium"):Mode==4?TEXT("MissileMedium"):TEXT("SpellCast"));
  TestTrue(TEXT("Controller trigger uses the existing stance-specific attack action"),ACEInputBindings::Down(PC,Attack));
 }
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::SpaceBar,3,FInputChord(EKeys::Gamepad_FaceButton_Top));ACEInputBindings::Commit();
 ACEInputBindings::Reload();Hold({EKeys::Gamepad_FaceButton_Top});
 TestTrue(TEXT("Controller rebind persists through a settings reload"),ACEInputBindings::Down(PC,EKeys::SpaceBar));
 TestFalse(TEXT("Controller rebind resolves conflicting pickup action"),ACEInputBindings::Down(PC,ACEInputBindings::Action(TEXT("Pickup"))));
 Hold({EKeys::SpaceBar});TestTrue(TEXT("Controller rebind preserves the keyboard jump"),ACEInputBindings::Down(PC,EKeys::SpaceBar));
 Movement->PlayerInput=NewObject<UPlayerInput>(Movement);
 Movement->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Gamepad_LeftY,IE_Axis,.65f));
 Movement->PlayerInput->ProcessInputStack({},.016f,false);
 TestTrue(TEXT("Analog stick is translated to the rebindable forward action"),ACEInputBindings::Down(Movement,EKeys::W));
 const float Analog=ACEInputBindings::Value(Movement,EKeys::W);
 TestTrue(TEXT("Partial stick deflection retains an analog magnitude"),Analog>0.f && Analog<1.f);
 Movement->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Gamepad_LeftY,IE_Axis,.05f));
 Movement->PlayerInput->ProcessInputStack({},.016f,false);
 TestFalse(TEXT("Recenter/deadzone releases the direction instead of leaving movement held"),ACEInputBindings::Down(Movement,EKeys::W));
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::W,3,FInputChord(EKeys::Gamepad_LeftStick_Down));ACEInputBindings::Commit();
 Movement->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Gamepad_LeftY,IE_Axis,-1.f));
 Movement->PlayerInput->ProcessInputStack({},.016f,false);
 TestTrue(TEXT("Stick directions can be rebound just like buttons"),ACEInputBindings::Down(Movement,EKeys::W));
 // Replay a supplied report without changing the user's live bindings or file.
 FString ReportedKeymap;
 if(FParse::Value(FCommandLine::Get(),TEXT("RetailKeymapTestFile="),ReportedKeymap))
 {
  ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();
  const auto Report=ACEInputBindings::ImportRetailKeymapFile(ReportedKeymap);
  if(!TestTrue(TEXT("Reported keymap imports"),Report.bSuccess)){AddError(Report.Error);return false;}
  AddInfo(FString::Printf(TEXT("Reported keymap: %d bindings; %d unsupported records"),Report.BindingCount,Report.Skipped.Num()));
  ACEInputBindings::Commit();
  auto CheckReported=[&]()
  {
   for(int32 Mode:{1,2,4,8})
   {
    ACEInputBindings::SetCombatContext(Mode);Hold({EKeys::Semicolon});
    TestTrue(TEXT("Reported semicolon selects next monster in every stance"),ACEInputBindings::Pressed(PC,EKeys::Apostrophe));
    TestFalse(TEXT("Reported next-monster key cannot cast selected spell"),ACEInputBindings::Pressed(PC,ACEInputBindings::Action(TEXT("SpellCast"))));
    Hold({EKeys::W});TestTrue(TEXT("Reported W moves forward in every stance"),ACEInputBindings::Down(PC,EKeys::W));
    TestFalse(TEXT("Reported movement cannot cast selected spell"),ACEInputBindings::Pressed(PC,ACEInputBindings::Action(TEXT("SpellCast"))));
    Hold({EKeys::Two});
    for(int32 Slot=0;Slot<12;++Slot)TestEqual(TEXT("Reported key 2 activates only spell slot 2 in magic"),
      ACEInputBindings::Pressed(PC,ACEInputBindings::SpellSlot(Slot)),Mode==8&&Slot==1);
    TestEqual(TEXT("Reported key 2 uses the item shortcut only outside magic"),ACEInputBindings::Pressed(PC,ACEInputBindings::Shortcut(1)),Mode!=8);
    TestFalse(TEXT("Reported key 2 cannot cast a different selected spell"),ACEInputBindings::Pressed(PC,ACEInputBindings::Action(TEXT("SpellCast"))));
   }
  };
  CheckReported();ACEInputBindings::Reload();CheckReported();
 }
 return !HasAnyErrors();
}
#endif
