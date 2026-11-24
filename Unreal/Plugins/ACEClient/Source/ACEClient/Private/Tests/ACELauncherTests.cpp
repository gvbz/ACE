#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Protocol/ACELoginErrors.h"
#include "ACELoginProfile.h"
#include "ACELoginSettings.h"
#include "ACELoginWidget.h"
#include "ACEUpdateSubsystem.h"
#include "ACEClientBuild.h"
#include "ACEPlayerController.h"
#include "ACEDatSubsystem.h"
#include "UI/ACERetailTextBlock.h"
#include "TimerManager.h"
#include "Blueprint/GameViewportSubsystem.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/GameViewportClient.h"
#include "Components/EditableTextBox.h"
#include "Components/WidgetSwitcher.h"
#include "Components/TextBlock.h"
#include "Components/SizeBox.h"
#include "Components/ScrollBox.h"
#include "Components/Border.h"
#include "Components/CheckBox.h"
#include "Components/PanelWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "Slate/WidgetRenderer.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "Input/DragAndDrop.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELauncherDirectoryTest,"ACE.Launcher.DirectoryAndProfiles",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACELauncherDirectoryTest::RunTest(const FString&)
{
    const FString Xml=TEXT("<ArrayOfServerItem><ServerItem><id>one</id><name>One &amp; Two</name><emu>ACE</emu><server_host>one.invalid</server_host><server_port>9047</server_port><website_url>https://one.invalid</website_url><discord_url>https://discord.gg/example</discord_url></ServerItem>")
        TEXT("<ServerItem><id>two</id><name>GDLE fixture</name><emu>GDL</emu><connect_string>two.invalid:9010</connect_string><DiscordUrl>https://discord.gg/oldschema</DiscordUrl></ServerItem>")
        TEXT("<ServerItem><name>Duplicate</name><emu>ACE</emu><connect_string>ONE.invalid:9047</connect_string></ServerItem>")
        TEXT("<ServerItem><name>Hidden</name><emu>ACE</emu><visibility>Hidden</visibility><connect_string>hidden.invalid:9000</connect_string></ServerItem>")
        TEXT("<ServerItem><name>Unsupported</name><emu>DF</emu><connect_string>df.invalid:9000</connect_string></ServerItem>")
        TEXT("<ServerItem><name>Bad port</name><emu>ACE</emu><connect_string>bad.invalid:65535</connect_string></ServerItem></ArrayOfServerItem>");
    TArray<FACELoginServer> Servers; FString Error;
    TestTrue(TEXT("Current and legacy public directory schemas parse"),ACELoginProfile::ParseDirectory(Xml,Servers,Error));
    TestEqual(TEXT("Duplicates, hidden, unsupported and invalid entries excluded"),Servers.Num(),2);
    if (Servers.Num()!=2) return false;
    TestTrue(TEXT("GDL alias selects GDLE authentication"),Servers[0].bGDLE);
    TestEqual(TEXT("Legacy server imports the exact port in connect_string"),Servers[0].Port,9010);
    TestEqual(TEXT("Current server imports the exact server_port field"),Servers[1].Port,9047);
    TestEqual(TEXT("XML entities decode for display"),Servers[1].Name,FString(TEXT("One & Two")));
    TestTrue(TEXT("Both Discord spellings supported"),!Servers[0].Discord.IsEmpty() && !Servers[1].Discord.IsEmpty());
    TestFalse(TEXT("Non-web external links blocked"),ACELoginProfile::IsWebLink(TEXT("file:///C:/Windows/System32/cmd.exe")));
    TestFalse(TEXT("DTD input rejected"),ACELoginProfile::ParseDirectory(TEXT("<!DOCTYPE bad><ArrayOfServerItem/>"),Servers,Error));
    TestEqual(TEXT("Failed refresh preserves usable directory"),Servers.Num(),2);

    FACELoginSettings P; P.Host=TEXT("old.invalid"); P.Account=TEXT("legacy"); P.Password=TEXT("fixture-only");
    P.MigrateLegacy(); P.MigrateLegacy();
    TestEqual(TEXT("Migration creates one saved server"),P.Servers.Num(),1);
    TestEqual(TEXT("Migration preserves one account without duplication"),P.Accounts.Num(),1);
    TestTrue(TEXT("Legacy accounts need no alias"),P.Accounts[0].Alias.IsEmpty());
    TestEqual(TEXT("Accounts without aliases display their username"),P.Accounts[0].DisplayName(),P.Account);
    FACELoginSettings AccountOnly; AccountOnly.Account=TEXT("saved without server"); AccountOnly.Password=TEXT("fixture"); AccountOnly.MigrateLegacy();
    TestTrue(TEXT("An old account survives even if no host was saved"),AccountOnly.Servers.IsEmpty() && AccountOnly.Accounts.Num()==1);
    P.DatDirectory=TEXT("C:/Fixture game data"); P.Servers.Append(Servers);
    P.Accounts[0].Alias=TEXT("Conquest main");
    P.Accounts.Add({TEXT("alternate"),P.Account,TEXT("other-secret"),TEXT("Coldeve crafter")});
    const FString Path=FPaths::ProjectSavedDir()/TEXT("Automation/Launcher/Profile-")+FGuid::NewGuid().ToString()+TEXT(".dat");
    TestTrue(TEXT("Multiple profiles save"),ACELoginSettings::Save(P,Path));
    FACELoginSettings Restored; TestTrue(TEXT("Multiple profiles load"),ACELoginSettings::Load(Restored,Path));
    TestEqual(TEXT("Server list persists"),Restored.Servers.Num(),3);
    TestTrue(TEXT("Published ports survive profile save and reload"),Restored.Servers[1].Port==9010 && Restored.Servers[2].Port==9047);
    TestEqual(TEXT("Alternate accounts persist"),Restored.Accounts.Num(),2);
    TestTrue(TEXT("Same username keeps separate saved identities"),Restored.Accounts[0].Username==Restored.Accounts[1].Username
        && Restored.Accounts[0].Id!=Restored.Accounts[1].Id);
    TestEqual(TEXT("Primary alias survives restart"),Restored.Accounts[0].Alias,FString(TEXT("Conquest main")));
    TestEqual(TEXT("Alternate alias survives restart"),Restored.Accounts[1].Alias,FString(TEXT("Coldeve crafter")));
    TestEqual(TEXT("Alias and actual username appear together"),Restored.Accounts[1].DisplayName(),FString(TEXT("Coldeve crafter (legacy)")));
    TestEqual(TEXT("DAT folder persists"),Restored.DatDirectory,P.DatDirectory);
#if PLATFORM_LINUX || PLATFORM_MAC
    TestTrue(TEXT("Local retains account selection without saving passwords"),Restored.SelectedAccountId==P.SelectedAccountId && Restored.Password.IsEmpty() && Restored.Accounts[0].Password.IsEmpty() && Restored.Accounts[1].Password.IsEmpty());
    FString SavedProfile; FFileHelper::LoadFileToString(SavedProfile,*Path);
    TestFalse(TEXT("Local profile contains no primary password"),SavedProfile.Contains(TEXT("fixture-only")));
    TestFalse(TEXT("Local profile contains no alternate password"),SavedProfile.Contains(TEXT("other-secret")));
#else
    TestTrue(TEXT("Active account and password survive restart"),Restored.SelectedAccountId==P.SelectedAccountId && Restored.Accounts[0].Password==P.Password);
    TestEqual(TEXT("Same username retains its alternate password"),Restored.Accounts[1].Password,FString(TEXT("other-secret")));
#endif
    TestFalse(TEXT("New launcher profiles do not opt into automatic updates"),Restored.bAutoUpdate);
    P.bAutoUpdate=true;
    TestTrue(TEXT("Automatic update preference saves with the device profile"),ACELoginSettings::Save(P,Path));
    TestTrue(TEXT("Automatic update preference reloads"),ACELoginSettings::Load(Restored,Path) && Restored.bAutoUpdate);
    P.bAutoUpdate=false; ACELoginSettings::Save(P,Path);
    TestTrue(TEXT("Opting out survives restart too"),ACELoginSettings::Load(Restored,Path) && !Restored.bAutoUpdate);
    FACELoginSettings Legacy; Legacy.bAutoUpdate=true; // Legacy format has no autoUpdate field.
    ACELoginSettings::Save(Legacy,Path);
    FACELoginSettings Migrated; Migrated.bAutoUpdate=true;
    TestTrue(TEXT("Existing profiles without the new field always default off"),ACELoginSettings::Load(Migrated,Path) && !Migrated.bAutoUpdate);
    const FString Removed=Restored.SelectedServerId; Restored.RemoveServer(Removed);
    TestEqual(TEXT("Removing a server retains shared accounts"),Restored.Accounts.Num(),2);
    TestTrue(TEXT("Removing a server preserves selected login"),Restored.SelectedAccountId==P.SelectedAccountId && ((PLATFORM_LINUX || PLATFORM_MAC) ? Restored.Password.IsEmpty() : Restored.Password==P.Password));
    IFileManager::Get().Delete(*Path,false,true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELauncherWidgetTest,"ACE.Launcher.ResponsiveUI",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACELauncherWidgetTest::RunTest(const FString&)
{
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto* GI=NewObject<UGameInstance>(GEngine); GI->InitializeStandalone();
    GI->GetWorldContext()->SetCurrentWorld(World);World->SetGameInstance(GI);
    TStrongObjectPtr<UACELoginWidget> Owner(CreateWidget<UACELoginWidget>(GI,UACELoginWidget::StaticClass())); auto* W=Owner.Get();
    W->ProfilePath=FPaths::ProjectSavedDir()/TEXT("Automation/Launcher/Widget-")+FGuid::NewGuid().ToString()+TEXT(".dat");
    auto Slate=W->TakeWidget();
    W->SetUseDatIntroBackdrop(true);
    TestEqual(TEXT("Launcher stays black when the intro/game world exists after logout"),W->BackdropBorder->GetBrushColor(),FLinearColor::Black);
    auto* RetailTitle=Cast<UACERetailTextBlock>(W->ServerTitle);
    TestNotNull(TEXT("Launcher titles use the retail font renderer"),RetailTitle);
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    TestTrue(TEXT("Retail launcher art loads after the form is constructed"),Dat->LoadDatDirectory(DatDirectory));
    W->NativeTick(W->GetCachedGeometry(),.3f);
    TestTrue(TEXT("Already constructed labels acquire their DAT font without rebuilding"),RetailTitle&&RetailTitle->GetBitmapFont());
    TestTrue(TEXT("Fresh installation has no developer server or account supplied"),W->Profile.Servers.IsEmpty() && W->Profile.Accounts.IsEmpty());
    W->RunAction(TEXT("newserver")); W->NameBox->SetText(FText::FromString(TEXT("Dereth · Home")));
    W->HostBox->SetText(FText::FromString(TEXT("home.example.invalid"))); W->RunAction(TEXT("saveserver"));
    TestEqual(TEXT("Custom server saved from form"),W->Profile.Servers.Num(),1);
    const FString HomeId=W->Profile.SelectedServerId;
    W->AccountBox->SetText(FText::FromString(TEXT("Adventurer"))); W->PasswordBox->SetText(FText::FromString(TEXT("fixture-password")));
    W->AccountAliasBox->SetText(FText::FromString(TEXT("Conquest main"))); W->RunAction(TEXT("saveaccount"));
    const FString FirstAccountId=W->Profile.SelectedAccountId;
    W->RunAction(TEXT("newaccount"));
    TestTrue(TEXT("New account clears the alias as well as credentials"),W->AccountAliasBox->GetText().IsEmpty() && W->AccountBox->GetText().IsEmpty() && W->PasswordBox->GetText().IsEmpty());
    W->AccountBox->SetText(FText::FromString(TEXT("Adventurer"))); W->PasswordBox->SetText(FText::FromString(TEXT("fixture-two")));
    W->AccountAliasBox->SetText(FText::FromString(TEXT("Coldeve crafter"))); W->RunAction(TEXT("saveaccount"));
    const FString SecondAccountId=W->Profile.SelectedAccountId;
    TestEqual(TEXT("Second identity does not overwrite first"),W->Profile.Accounts.Num(),2);
    TestTrue(TEXT("Matching usernames still have distinct identity IDs"),FirstAccountId!=SecondAccountId);
    W->RunAction(TEXT("selectaccount"),FirstAccountId);
    TestEqual(TEXT("Selecting first duplicate restores its password"),W->PasswordBox->GetText().ToString(),FString(TEXT("fixture-password")));
    TestEqual(TEXT("Selecting first duplicate restores its alias"),W->AccountAliasBox->GetText().ToString(),FString(TEXT("Conquest main")));
    W->AccountAliasBox->SetText(FText::FromString(TEXT(" Conquest adventurer "))); W->RunAction(TEXT("saveaccount"));
    TestEqual(TEXT("Alias edit preserves account identity"),W->Profile.SelectedAccountId,FirstAccountId);
    TestEqual(TEXT("Alias edit trims surrounding spaces"),W->Profile.Accounts[0].Alias,FString(TEXT("Conquest adventurer")));
    TestEqual(TEXT("Alias edit leaves other duplicate untouched"),W->Profile.Accounts[1].Alias,FString(TEXT("Coldeve crafter")));
    W->RunAction(TEXT("newaccount"));
    W->AccountBox->SetText(FText::FromString(TEXT("Adventurer"))); W->PasswordBox->SetText(FText::FromString(TEXT("fixture-two")));
    W->AccountAliasBox->SetText(FText::FromString(TEXT("Same credentials, another label"))); W->RunAction(TEXT("saveaccount"));
    TestEqual(TEXT("New entry never merges by matching username and password"),W->Profile.Accounts.Num(),3);
    W->RunAction(TEXT("removeaccount")); W->RunAction(TEXT("confirmremove"));
    TestEqual(TEXT("Deleting an identical login removes only that entry"),W->Profile.Accounts.Num(),2);
    W->RunAction(TEXT("selectaccount"),SecondAccountId);
    TestEqual(TEXT("Selecting second duplicate restores its password"),W->PasswordBox->GetText().ToString(),FString(TEXT("fixture-two")));
    W->RunAction(TEXT("newserver")); W->NameBox->SetText(FText::FromString(TEXT("A second world"))); W->HostBox->SetText(FText::FromString(TEXT("second.invalid")));
    W->RunAction(TEXT("gdle")); W->RunAction(TEXT("saveserver"));
    TestEqual(TEXT("Accounts are shared when switching to a new server"),W->AccountBox->GetText().ToString(),FString(TEXT("Adventurer")));
    TestEqual(TEXT("Server switch retains selected alias"),W->AccountAliasBox->GetText().ToString(),FString(TEXT("Coldeve crafter")));
    TestEqual(TEXT("Every saved account is offered on each server"),W->AccountList->GetChildrenCount(),2);
    W->RunAction(TEXT("accounts"));
    TestEqual(TEXT("Account management has its own page"),W->Pages->GetActiveWidgetIndex(),6);
    TestEqual(TEXT("Account manager lists the same saved accounts"),W->ManagedAccountList->GetChildrenCount(),2);
    W->RunAction(TEXT("play"));
    TestTrue(TEXT("Selected GDLE type stored"),W->Profile.SelectedServer()->bGDLE);
    W->RunAction(TEXT("selectserver"),HomeId);
    TestEqual(TEXT("Switching servers preserves the chosen identity"),W->Profile.SelectedAccountId,SecondAccountId);
    W->RunAction(TEXT("selectaccount"),W->Profile.Accounts[1].Id);
    TestEqual(TEXT("Account selection keeps authentication username rather than alias"),W->Profile.Account,FString(TEXT("Adventurer")));
    for(auto* List:{W->AccountList.Get(),W->ManagedAccountList.Get()})
    {
        auto* Button=Cast<UACELoginActionButton>(List->GetChildAt(1));
        auto* Size=Button?Cast<USizeBox>(Button->GetContent()):nullptr;
        auto* Title=Size?Cast<UTextBlock>(Size->GetContent()):nullptr;
        TestTrue(TEXT("Play and Accounts rows display alias with username"),Title&&Title->GetText().ToString()==TEXT("Coldeve crafter (Adventurer)"));
        TestTrue(TEXT("Rows distinguish duplicate usernames by stable ID"),Button&&Button->Value==SecondAccountId);
    }
    W->RunAction(TEXT("accounts"));
    TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName)> FindTag=[&](TSharedRef<SWidget> Widget,FName Tag)->TSharedPtr<SWidget>
    {
        if (Widget->GetTag()==Tag) return Widget;
        auto* Children=Widget->GetChildren();
        for (int32 I=0;I<Children->Num();++I) if (auto Found=FindTag(Children->GetChildAt(I),Tag)) return Found;
        return nullptr;
    };
    const auto RowGeometry=FGeometry::MakeRoot(FVector2D(600,40),FSlateLayoutTransform());
    const FString EditedAlias=TEXT("Unsaved account label");
    W->AccountAliasBox->SetText(FText::FromString(EditedAlias));
    auto DragAccount=[&](const FString& Source,const FString& Target,bool After)
    {
        const FString PreviousFirst=W->Profile.Accounts[0].Id;
        auto Grip=FindTag(W->ManagedAccountList->TakeWidget(),FName(*(TEXT("AccountOrderGrip.")+Source)));
        auto Row=FindTag(W->ManagedAccountList->TakeWidget(),FName(*(TEXT("AccountOrderRow.")+Target)));
        if (!TestTrue(TEXT("Account rows expose a grip and full-width drop target"),Grip&&Row)) return;
        const FVector2D Position(10,After?35:5);
        const FPointerEvent Mouse(0,Position,Position,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState());
        const auto Drag=Grip->OnDragDetected(RowGeometry,Mouse).GetDragDropContent();
        if (!TestTrue(TEXT("Account grip starts a drag"),Drag.IsValid())) return;
        TestTrue(TEXT("Drag decorator works without a native popup"),Drag->IsWindowlessOperation()&&Drag->GetDefaultDecorator().IsValid());
        TestFalse(TEXT("Unrelated payload cannot reorder accounts"),Row->OnDrop(RowGeometry,FDragDropEvent(Mouse,MakeShared<FDragDropOperation>())).IsEventHandled());
        TestTrue(TEXT("Hover accepts account drag and shows insertion marker"),Row->OnDragOver(RowGeometry,FDragDropEvent(Mouse,Drag)).IsEventHandled());
        Row->OnDragLeave(FDragDropEvent(Mouse,Drag));
        TestEqual(TEXT("Dragging without dropping leaves the order unchanged"),W->Profile.Accounts[0].Id,PreviousFirst);
        TestTrue(TEXT("Dropping account onto another row is handled"),Row->OnDrop(RowGeometry,FDragDropEvent(Mouse,Drag)).IsEventHandled());
        W->NativeTick(W->GetCachedGeometry(),0);
    };
    // Move down to the end, then back up, exercising both insertion halves.
    DragAccount(FirstAccountId,SecondAccountId,true);
    TestEqual(TEXT("Lower-half drop puts the account last"),W->Profile.Accounts.Last().Id,FirstAccountId);
    TestEqual(TEXT("Dragging does not change selected identity"),W->Profile.SelectedAccountId,SecondAccountId);
    TestEqual(TEXT("Dragging preserves unsaved account form edits"),W->AccountAliasBox->GetText().ToString(),EditedAlias);
    TestEqual(TEXT("Dragging preserves the selected password"),W->PasswordBox->GetText().ToString(),FString(TEXT("fixture-two")));
    TestFalse(TEXT("Dropping in the same position does nothing"),W->MoveAccountInOrder(FirstAccountId,SecondAccountId,true));
    TestFalse(TEXT("A removed drag identity cannot change the list"),W->MoveAccountInOrder(TEXT("missing"),SecondAccountId,false));
    TestFalse(TEXT("Self-drop does nothing"),W->MoveAccountInOrder(FirstAccountId,FirstAccountId,true));
    // Verify the saved order before restoring the original sequence below.
    FACELoginSettings Ordered;
    TestTrue(TEXT("Drag order is saved immediately"),ACELoginSettings::Load(Ordered,W->ProfilePath));
    TestTrue(TEXT("Saved order retains both duplicate identities and selection"),Ordered.Accounts.Num()==2
        && Ordered.Accounts[0].Id==SecondAccountId && Ordered.Accounts[1].Id==FirstAccountId && Ordered.SelectedAccountId==SecondAccountId);
    TestTrue(TEXT("Drag does not save unfinished alias edits"),Ordered.Accounts[0].Alias==TEXT("Coldeve crafter"));
    for (auto* List:{W->AccountList.Get(),W->ManagedAccountList.Get()})
        TestEqual(TEXT("Both account lists show the saved order"),CastChecked<UACELoginActionButton>(List->GetChildAt(0))->Value,SecondAccountId);
    W->AccountAliasBox->SetText(FText::FromString(TEXT("Coldeve crafter")));
    // Restore upward without disturbing selection or any credentials.
    DragAccount(FirstAccountId,SecondAccountId,false);
    TestEqual(TEXT("Moving up restores the first account"),W->Profile.Accounts[0].Id,FirstAccountId);
    W->RunAction(TEXT("play"));
    TestFalse(TEXT("An old drag cannot reorder from another page"),W->MoveAccountInOrder(FirstAccountId,SecondAccountId,true));
    W->RunAction(TEXT("removeaccount")); W->RunAction(TEXT("cancelremove"));
    TestEqual(TEXT("Cancelled removal keeps accounts"),W->Profile.Accounts.Num(),2);
    W->Profile.Servers[0].Description.Reset(); W->SelectServer(HomeId);
    W->Directory=W->Profile.Servers;
    W->Updater=GI->GetSubsystem<UACEUpdateSubsystem>();
    if (!TestNotNull(TEXT("Game instance owns the updater"),W->Updater.Get())) return false;
    TestNotNull(TEXT("Updates page provides a real checkbox"),W->AutoUpdateCheckBox.Get());
    TestFalse(TEXT("Checkbox starts unchecked"),W->AutoUpdateCheckBox->IsChecked());
#if PLATFORM_LINUX || PLATFORM_MAC
    if(TestNotNull(TEXT("Local accounts provide a password-saving checkbox"),W->RememberPasswordsCheckBox.Get()))
    {
        TestFalse(TEXT("Password saving starts unchecked"),W->RememberPasswordsCheckBox->IsChecked());
        W->RememberPasswordsCheckBox->SetIsChecked(true);
        W->RememberPasswordsCheckBox->OnCheckStateChanged.Broadcast(true);
        {
            TStrongObjectPtr<UACELoginWidget> Reopened(CreateWidget<UACELoginWidget>(GI,UACELoginWidget::StaticClass()));
            Reopened->ProfilePath=W->ProfilePath;
            auto ReopenedSlate=Reopened->TakeWidget();
            TestTrue(TEXT("Reopened Local launcher restores the password preference"),Reopened->RememberPasswordsCheckBox->IsChecked());
            TestEqual(TEXT("Reopened Local launcher fills the selected account password"),Reopened->PasswordBox->GetText().ToString(),FString(TEXT("fixture-two")));
            Reopened->NativeDestruct();
        }
        W->RememberPasswordsCheckBox->SetIsChecked(false);
        W->RememberPasswordsCheckBox->OnCheckStateChanged.Broadcast(false);
        FACELoginSettings Cleared;
        TestTrue(TEXT("Unchecking removes passwords from every saved account"),ACELoginSettings::Load(Cleared,W->ProfilePath)
            && !Cleared.bSavePasswordsLocally && Cleared.Password.IsEmpty()
            && !Cleared.Accounts.ContainsByPredicate([](const FACELoginAccount& A){return !A.Password.IsEmpty();}));
        TestFalse(TEXT("Unchecking retains the current session password"),W->PasswordBox->GetText().IsEmpty());
    }
    TestTrue(TEXT("Local exposes supported automatic installation"),W->AutoUpdateCheckBox->IsVisible());
    TestFalse(TEXT("Local automatic updates are opt-in"),W->Updater->IsAutoUpdateEnabled());
    W->AutoUpdateCheckBox->SetIsChecked(true);
    W->AutoUpdateCheckBox->OnCheckStateChanged.Broadcast(true);
    FACELoginSettings UpdatePreference;
    TestTrue(TEXT("Local checking enables and persists automatic updates"),W->Updater->IsAutoUpdateEnabled()
        && ACELoginSettings::Load(UpdatePreference,W->ProfilePath) && UpdatePreference.bAutoUpdate);
    W->AutoUpdateCheckBox->SetIsChecked(false);
    W->AutoUpdateCheckBox->OnCheckStateChanged.Broadcast(false);
    TestTrue(TEXT("Local unchecking disables and persists automatic updates"),!W->Updater->IsAutoUpdateEnabled()
        && ACELoginSettings::Load(UpdatePreference,W->ProfilePath) && !UpdatePreference.bAutoUpdate);
#else
    W->AutoUpdateCheckBox->SetIsChecked(true);
    W->AutoUpdateCheckBox->OnCheckStateChanged.Broadcast(true);
    FACELoginSettings UpdatePreference;
    TestTrue(TEXT("Checking enables and persists automatic updates"),W->Updater->IsAutoUpdateEnabled()
        && ACELoginSettings::Load(UpdatePreference,W->ProfilePath) && UpdatePreference.bAutoUpdate);
    {
        TStrongObjectPtr<UACELoginWidget> Reloaded(CreateWidget<UACELoginWidget>(GI,UACELoginWidget::StaticClass()));
        Reloaded->ProfilePath=W->ProfilePath;
        auto ReloadedSlate=Reloaded->TakeWidget();
        TestTrue(TEXT("Reopened launcher restores the checked checkbox"),Reloaded->AutoUpdateCheckBox->IsChecked());
        TestEqual(TEXT("Reopened launcher selects the same duplicate account"),Reloaded->Profile.SelectedAccountId,SecondAccountId);
        TestEqual(TEXT("Reopened launcher restores alias"),Reloaded->AccountAliasBox->GetText().ToString(),FString(TEXT("Coldeve crafter")));
        TestEqual(TEXT("Reopened launcher restores the correct password"),Reloaded->PasswordBox->GetText().ToString(),FString(TEXT("fixture-two")));
        Reloaded->NativeDestruct();
    }
    W->AutoUpdateCheckBox->SetIsChecked(false);
    W->AutoUpdateCheckBox->OnCheckStateChanged.Broadcast(false);
    TestTrue(TEXT("Unchecking disables and persists automatic updates"),!W->Updater->IsAutoUpdateEnabled()
        && ACELoginSettings::Load(UpdatePreference,W->ProfilePath) && !UpdatePreference.bAutoUpdate);
#endif
    if (FApp::CanEverRender())
    {
        FWidgetRenderer Renderer(false,true);
        auto Capture=[&](const TCHAR* Name,FIntPoint Size)
        {
            TStrongObjectPtr<UTextureRenderTarget2D> Target(FWidgetRenderer::CreateTargetFor(FVector2D(Size),TF_Bilinear,false));
            for (int Pass=0;Pass<5;++Pass) { Renderer.DrawWidget(Target.Get(),Slate,FVector2D(Size),0.f); FlushRenderingCommands(); W->NativeTick(W->GetCachedGeometry(),0); }
            TestTrue(TEXT("Responsive dashboard fits horizontal viewport"),W->DashboardSize->GetCachedGeometry().GetAbsoluteSize().X<=Size.X+1);
            // List rows may scroll; navigation, form fields and actions may not.
            // Check the actual rendered allocation, including long errors below it.
            TFunction<void(UWidget*,float)> CheckActions=[&](UWidget* Widget,float Bottom)
            {
                if(!Widget || !Widget->IsVisible() || Cast<UScrollBox>(Widget)) return;
                if(Cast<UButton>(Widget) || Cast<UEditableTextBox>(Widget))
                {
                    const auto& G=Widget->GetCachedGeometry();
                    const FVector2D P=G.GetAbsolutePosition(), S=G.GetAbsoluteSize();
                    TestTrue(FString::Printf(TEXT("%s: %s stays fully visible without scrolling"),Name,*Widget->GetName()),
                        S.X>10 && S.Y>10 && P.X>=0 && P.Y>=0 && P.X+S.X<=Size.X+1 && P.Y+S.Y<=Bottom+1);
                }
                if(auto* Panel=Cast<UPanelWidget>(Widget)) for(auto* Child:Panel->GetAllChildren()) CheckActions(Child,Bottom);
            };
            const auto& Dashboard=W->DashboardSize->GetCachedGeometry();
            CheckActions(W->Pages->GetActiveWidget(),Dashboard.GetAbsolutePosition().Y+Dashboard.GetAbsoluteSize().Y);
            for(const auto& Button:W->NavigationButtons) CheckActions(Button.Get(),Size.Y);
            CheckActions(W->FooterSize,Size.Y);
            if(W->Pages->GetActiveWidgetIndex()==0)
            {
                TestTrue(TEXT("Saved server list retains room for a row"),W->ServerList->GetCachedGeometry().GetAbsoluteSize().Y>=25);
                TestTrue(TEXT("Saved account list retains room for a row"),W->AccountList->GetCachedGeometry().GetAbsoluteSize().Y>=25);
            }
            TArray<FColor> Pixels; FReadSurfaceDataFlags ReadFlags; ReadFlags.SetLinearToGamma(true); Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,ReadFlags);
            TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/Launcher")/(FString(Name)+TEXT(".png"))));
        };
        W->SetStatus(FString()); Capture(TEXT("Desktop"),FIntPoint(1600,1000));
        W->RunAction(TEXT("accounts"));Capture(TEXT("Accounts"),FIntPoint(1100,900));
        Capture(TEXT("Accounts720p"),FIntPoint(1280,720));Capture(TEXT("AccountsSmallWindow"),FIntPoint(800,600));
        Capture(TEXT("AccountsNarrow"),FIntPoint(650,1000));W->RunAction(TEXT("play"));
        Capture(TEXT("VR"),FIntPoint(1100,900));
        TestEqual(TEXT("Idle status leaves room for the login panel"),W->StatusText->GetParent()->GetVisibility(),ESlateVisibility::Collapsed);
        TestTrue(TEXT("Launch has a visible laid-out button"),W->LoginButton->GetCachedGeometry().GetLocalSize().Y>20);
        TestTrue(TEXT("Launch fits inside headset panel without scrolling"),W->LoginButton->GetCachedGeometry().GetAbsolutePosition().Y+W->LoginButton->GetCachedGeometry().GetLocalSize().Y<900);
        Capture(TEXT("Window720p"),FIntPoint(1280,720));
        Capture(TEXT("Desktop4K"),FIntPoint(3840,2160));
        Capture(TEXT("Desktop32x9"),FIntPoint(3840,1080));
        Capture(TEXT("Narrow"),FIntPoint(650,1000));
        Capture(TEXT("SmallWindow"),FIntPoint(800,600));
        const auto SavedAccounts=W->Profile.Accounts;
        for(int32 I=0;I<40;++I) W->Profile.Accounts.Add({FString::FromInt(I),FString::Printf(TEXT("Saved adventurer %d"),I),TEXT("fixture")});
        W->RefreshAccounts(); Capture(TEXT("ManyAccounts720p"),FIntPoint(1280,720));
        W->Profile.Accounts=SavedAccounts; W->RefreshAccounts();
        W->SetStatus(ACELoginErrors::Character(1));
        Capture(TEXT("LoginRejectedVR"),FIntPoint(1100,900));
        TestTrue(TEXT("Account-in-use guidance wraps within the VR panel"),W->StatusText->GetCachedGeometry().GetAbsoluteSize().X<1100);
        TestTrue(TEXT("Full error guidance remains inside the headset panel"),W->StatusText->GetCachedGeometry().GetAbsolutePosition().Y+W->StatusText->GetCachedGeometry().GetAbsoluteSize().Y<900);
        Capture(TEXT("LoginRejected720p"),FIntPoint(1280,720));
        Capture(TEXT("LoginRejectedNarrow"),FIntPoint(650,1000));
        W->RunAction(TEXT("removeaccount")); Capture(TEXT("ConfirmationNarrow"),FIntPoint(650,1000)); W->RunAction(TEXT("cancelremove"));
        W->RunAction(TEXT("browser")); W->RunAction(TEXT("directoryselect"),HomeId); Capture(TEXT("Browser"),FIntPoint(1100,900));
        Capture(TEXT("BrowserNarrow"),FIntPoint(650,1000));
        W->RunAction(TEXT("editserver")); Capture(TEXT("Editor"),FIntPoint(1100,900));
        Capture(TEXT("Editor720p"),FIntPoint(1280,720)); Capture(TEXT("EditorNarrow"),FIntPoint(650,1000));
        W->RunAction(TEXT("files")); Capture(TEXT("Files"),FIntPoint(1100,900));
        W->FileProblem->SetText(FText::FromString(TEXT("Missing client_portal.dat and client_cell_1.dat. Choose the folder containing your Asheron's Call game data to continue.")));
        W->FileProblem->SetVisibility(ESlateVisibility::HitTestInvisible);
        Capture(TEXT("MissingFiles720p"),FIntPoint(1280,720)); Capture(TEXT("MissingFilesNarrow"),FIntPoint(650,1000));
        W->RunAction(TEXT("plugins")); Capture(TEXT("Plugins"),FIntPoint(1100,900));
        // This offscreen fixture does not mount the widget in a player screen.
        W->Updater=GI->GetSubsystem<UACEUpdateSubsystem>();
        if (!TestNotNull(TEXT("Game instance owns the updater"),W->Updater.Get())) return false;
        W->Updater->Release.Number=ACEClientBuild::ReleaseNumber+1;
        W->Updater->State=EACEUpdateState::Available;
        W->Updater->Message=TEXT("A new version is available (320 MB).");
        W->RunAction(TEXT("updates")); Capture(TEXT("UpdatesVR"),FIntPoint(1100,900));
        TestTrue(TEXT("Auto-update checkbox has a usable headset hit target"),W->AutoUpdateCheckBox->GetCachedGeometry().GetLocalSize().Y>=26);
        W->AutoUpdateCheckBox->SetIsChecked(true);
        W->AutoUpdateCheckBox->OnCheckStateChanged.Broadcast(true);
        Capture(TEXT("AutoUpdateEnabledVR"),FIntPoint(1100,900));
        W->AutoUpdateCheckBox->SetIsChecked(false);
        W->AutoUpdateCheckBox->OnCheckStateChanged.Broadcast(false);
        Capture(TEXT("Updates720p"),FIntPoint(1280,720));
        TestTrue(TEXT("Download update is available in the shared VR/desktop lobby"),W->DownloadUpdateButton->IsVisible());
        TestFalse(TEXT("Install remains hidden until verification"),W->InstallUpdateButton->IsVisible());
        W->Updater->State=EACEUpdateState::Downloading; W->RefreshUpdateControls();
        TestFalse(TEXT("Cannot log in while replacing client files"),W->LoginButton->GetIsEnabled());
        W->Updater->State=EACEUpdateState::Ready; W->Updater->Message=FString(TEXT("Update ready. "))+ACEUpdates::InstallationNotice(PLATFORM_ANDROID);
        W->RefreshUpdateControls(); Capture(TEXT("UpdateReadyVR"),FIntPoint(1100,900));
        TestTrue(TEXT("Verified update presents install action"),W->InstallUpdateButton->IsVisible());
        W->RunAction(TEXT("installupdate")); W->RefreshUpdateControls();
        TestEqual(TEXT("Manual installation opens the closure notice"),W->Updater->State,EACEUpdateState::InstallNotice);
        TestTrue(TEXT("Notice offers cancellation before the game closes"),W->CancelUpdateButton->IsVisible());
        TestFalse(TEXT("Notice prevents launching into a server"),W->LoginButton->GetIsEnabled());
        Capture(TEXT("UpdateClosureNoticeVR"),FIntPoint(1100,900));
        Capture(TEXT("UpdateClosureNotice720p"),FIntPoint(1280,720));
        Capture(TEXT("UpdateClosureNoticeNarrow"),FIntPoint(650,1000));
        W->RunAction(TEXT("cancelupdate"));
        TestEqual(TEXT("Cancel button dismisses the scheduled install"),W->Updater->State,EACEUpdateState::Ready);
        // Render the longer Quest instructions on the shared headset surface.
        W->Updater->State=EACEUpdateState::Installing;
        W->Updater->Message=ACEUpdates::InstallationNotice(true);
        W->RefreshUpdateControls(); Capture(TEXT("QuestUpdateClosureNotice"),FIntPoint(1100,900));
        W->Updater->Cancel(); W->RefreshUpdateControls();

    }
    W->RunAction(TEXT("play")); W->RunAction(TEXT("removeaccount")); W->RunAction(TEXT("confirmremove"));
    TestEqual(TEXT("Confirmed removal deletes only selected account"),W->Profile.Accounts.Num(),1);
    W->RunAction(TEXT("removeserver")); W->RunAction(TEXT("confirmremove"));
    TestEqual(TEXT("Removing a server keeps shared accounts"),W->Profile.Accounts.Num(),1);
    FACELoginSettings Saved; TestTrue(TEXT("Actions survive restart"),ACELoginSettings::Load(Saved,W->ProfilePath));
    TestTrue(TEXT("Server deletion preserves the remaining shared identity after restart"),Saved.Accounts.Num()==1 && Saved.Account==TEXT("Adventurer"));
    TestFalse(TEXT("Removed alternate cannot reappear"),Saved.Accounts.ContainsByPredicate([&](const auto& A){return A.Id==SecondAccountId;}));
    TestTrue(TEXT("Same-username survivor keeps its identity and alias"),Saved.Accounts[0].Id==FirstAccountId && Saved.Accounts[0].Alias==TEXT("Conquest adventurer"));
    W->NativeDestruct(); IFileManager::Get().Delete(*W->ProfilePath,false,true);
    Owner.Reset(); GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
    return true;
}
// Exercise the actual AddToPlayerScreen host. Rendering TakeWidget() directly
// bypasses the viewport slot and cannot detect accidentally cleared anchors.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELauncherViewportTest,"ACE.Launcher.ViewportPlacement",
 EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACELauncherViewportTest::RunTest(const FString&)
{
    UWorld* World=GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr;
    auto* PC=World ? Cast<AACEPlayerController>(World->GetFirstPlayerController()) : nullptr;
    if (!TestNotNull(TEXT("Run in the launched client with its real player viewport"),PC)) return false;
    auto* W=PC->LoginWidget.Get();
    if (!TestNotNull(TEXT("Login lobby exists in the launched client"),W)) return false;
    TestTrue(TEXT("Login is attached to the player screen"),W->IsInViewport());
    TestNotNull(TEXT("Live lobby is connected to updater"),W->Updater.Get());
    auto* Viewport=UGameViewportSubsystem::Get();
    if (!TestNotNull(TEXT("Viewport subsystem owns the login slot"),Viewport)) return false;
    auto CheckSlot=[&]()
    {
        const auto Slot=Viewport->GetWidgetSlot(W);
        TestTrue(TEXT("Login fills both viewport axes"),Slot.Anchors==FAnchors(0,0,1,1));
        TestTrue(TEXT("Login has no stale position or size margins"),Slot.Offsets==FMargin(0));
    };
    CheckSlot();
    PC->ApplyViewportSizeToLoginWidget(); // Re-show/retry must preserve stretch too.
    CheckSlot();
    const FGeometry Screen=UWidgetLayoutLibrary::GetPlayerScreenWidgetGeometry(PC);
    const FGeometry Lobby=W->GetCachedGeometry();
    TestTrue(TEXT("Lobby receives the full screen allocation"),Lobby.GetAbsoluteSize().Equals(Screen.GetAbsoluteSize(),2));
    for (UWidget* Control : TArray<UWidget*>{W->ServerTitle,W->AccountList,W->LoginButton,W->StatusText})
    {
        if(Control==W->StatusText && W->StatusText->GetText().IsEmpty())
        {
            TestEqual(TEXT("Idle status does not reserve a blank footer"),W->StatusText->GetParent()->GetVisibility(),ESlateVisibility::Collapsed);
            continue;
        }
        const FGeometry Geometry=Control->GetCachedGeometry();
        const FVector2D Size=Geometry.GetLocalSize();
        const FVector2D First=Screen.AbsoluteToLocal(Geometry.LocalToAbsolute(FVector2D::ZeroVector));
        const FVector2D Last=Screen.AbsoluteToLocal(Geometry.LocalToAbsolute(Size));
        TestTrue(TEXT("Login controls have usable dimensions"),Size.X>50 && Size.Y>15);
        TestTrue(TEXT("Login controls are fully within the live viewport"),First.X>=0 && First.Y>=0 && Last.X<=Screen.GetLocalSize().X+1 && Last.Y<=Screen.GetLocalSize().Y+1);
    }
    auto* Dat=World->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    // The launched client's background loader owns these resources. Reloading
    // them synchronously here invalidates the live UI's font/texture references.
    if (!TestTrue(TEXT("Character-list handoff has retail UI data"),Dat && Dat->IsDatReady())) return false;
    PC->ShowCharacterSelectUI({},TEXT("Regression fixture"));
    TestNotNull(TEXT("Successful login creates character selection"),PC->DatCharSelectBinder.Get());
    TestNull(TEXT("Successful login removes the account launcher"),PC->LoginWidget.Get());
    TestFalse(TEXT("Character selection clears the startup retry timer"),World->GetTimerManager().IsTimerActive(PC->ShowLoginUIRetryTimerHandle));
    PC->ShowLoginUI(); // A next-tick callback can already have been queued.
    TestNull(TEXT("A late startup callback cannot cover character selection"),PC->LoginWidget.Get());
    return true;
}
// This opt-in process test presses the actual lobby button. It cannot run in
// PIE or a normal automation batch, because success terminates its own client.
static FAutoConsoleCommandWithWorld GACEQuitButtonTest(TEXT("ace.TestQuitButton"),
 TEXT("With -ACEQuitProcessTest, press the lobby Quit button in an isolated -game process."),
 FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
 {
    if (!World || !World->IsGameWorld() || World->IsPlayInEditor()
        || !FParse::Param(FCommandLine::Get(),TEXT("ACEQuitProcessTest"))) return;
    FTimerHandle Handle;
    World->GetTimerManager().SetTimer(Handle, [Weak=TWeakObjectPtr<UWorld>(World)]()
    {
        for (TObjectIterator<UACELoginActionButton> It; It; ++It)
            if (It->Action==TEXT("quit") && It->Owner.IsValid() && It->Owner->IsInViewport()
                && It->Owner->GetWorld()==Weak.Get())
            {
                UE_LOG(LogTemp, Display, TEXT("ACE quit process test: pressing the lobby Quit button"));
                It->OnClicked.Broadcast();
                return;
            }
        UE_LOG(LogTemp, Error, TEXT("ACE quit process test: lobby Quit button was not found"));
        FPlatformMisc::RequestExitWithStatus(false, 1);
    }, 5.f, false);
 }));
#endif
