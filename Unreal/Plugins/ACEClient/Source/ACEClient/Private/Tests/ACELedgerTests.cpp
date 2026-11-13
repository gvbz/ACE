#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Mods/ACELedger.h"
#include "Mods/ACEWaypoint.h"
#include "Misc/ScopeExit.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginDesktop.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginIcon.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUIElementManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Engine/GameInstance.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELedgerTest,"ACE.Plugins.Ledgers",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACELedgerTest::RunTest(const FString&)
{
    FACEHuntLedger L;L.Reset(1000);
    FACEPlayerVitals V;V.bValid=true;V.QualityInt64s.Add(1,1000000);V.QualityInt64s.Add(6,500);
    V.TotalExperience=1000000;V.AvailableLuminance=500;L.Observe(V,1000);
    TestEqual(TEXT("Login totals establish a baseline"),L.XP,int64(0));TestEqual(TEXT("Existing luminance is not income"),L.Lum,int64(0));
    TestEqual(TEXT("No XP gain cannot predict a level"),L.SecondsToLevel(1200,1060),-1.);
    V.TotalExperience+=600;V.AvailableLuminance+=100;L.Observe(V,1060);
    TestEqual(TEXT("Hourly rate uses elapsed run time"),L.PerHour(L.XP,1060),int64(36000));
    TestEqual(TEXT("600 XP in a minute estimates two minutes for 1200 XP"),L.SecondsToLevel(1200,1060),120.);
    TestEqual(TEXT("Estimate updates as remaining XP changes"),L.SecondsToLevel(300,1060),30.);
    TestEqual(TEXT("Idle time reduces the run rate"),L.SecondsToLevel(1200,1120),240.);
    TestEqual(TEXT("A completed level has no remaining time"),L.SecondsToLevel(0,1060),0.);
    V.AvailableExperience=0;V.AvailableLuminance-=500;L.Observe(V,1061);
    V.AvailableLuminance+=20;L.Observe(V,1062);
    TestEqual(TEXT("Spending does not remove earned luminance"),L.Lum,int64(120));
    TestEqual(TEXT("XP spending does not alter income"),L.XP,int64(600));
    TestEqual(TEXT("Recent XP expires after five minutes"),L.LastFiveMinutes(1360).XP,int64(0));
    TestEqual(TEXT("Recent luminance counts each gain independently"),L.LastFiveMinutes(1360).Lum,int64(20));
    TestEqual(TEXT("All recent gains expire"),L.LastFiveMinutes(1362).Lum,int64(0));
    TestEqual(TEXT("Lifetime run total survives rolling expiry"),L.XP,int64(600));
    L.Reset(1400);L.Observe(V,1400);TestEqual(TEXT("Reset does not recount historical XP"),L.XP,int64(0));
    TestEqual(TEXT("Reset waits for new XP before estimating"),L.SecondsToLevel(1200,1460),-1.);
    for(int32 I=1;I<=100000;++I){++V.TotalExperience;L.Observe(V,1400+I);}
    TestEqual(TEXT("Long runs have bounded rolling history"),L.LastFiveMinutes(101400).XP,int64(300));
    TestEqual(TEXT("Long run totals retain all gains"),L.XP,int64(100000));

    FACEHuntLedger Pending;Pending.Reset(0);FACEPlayerVitals Partial;Partial.bValid=true;
    Pending.Observe(Partial,1);
    TestFalse(TEXT("Early attribute updates do not invent a zero luminance baseline"),Pending.HasLum);
    Partial.QualityInt64s.Add(6,500);Partial.AvailableLuminance=500;Pending.Observe(Partial,2);
    TestEqual(TEXT("A first explicit balance without a login snapshot is a baseline"),Pending.Lum,int64(0));

    TMap<int32,FACEWorldObject> Objects;
    // ACE's Prismatic Taper (20631): use its real item icon, not toolbar artwork.
    auto Item=[&](int32 Id,int32 Parent,int32 Count,int32 Class=20631){FACEWorldObject O;O.Guid=Id;O.ContainerId=Parent;O.WeenieClassId=Class;
        O.Name=TEXT("Prismatic Taper");O.IconId=0x0600262A;O.ItemType=4096;O.StackSize=Count;Objects.Add(Id,O);};
    Item(2,1,1,90);Item(3,2,1,91);Item(10,1,20);Item(11,2,30);Item(12,3,40);Item(13,888,99);
    Item(14,0,5);Objects[14].WielderId=1;Item(15,1,0);Item(16,17,7);Item(17,16,7);
    TArray<FACESupplyEntry> Entries;FACESupplyEntry E;E.ClassId=20631;E.Name=TEXT("Prismatic Taper");E.CopyAppearance(Objects[10]);Entries.Add(E);
    ACELedger::Count(Entries,1,Objects);TestEqual(TEXT("All owned packs and equipped items count; corpse items and cycles do not"),Entries[0].Count,int64(96));
    Objects.Remove(10);Objects[11].StackSize=3;ACELedger::Count(Entries,1,Objects);TestEqual(TEXT("Consumed stacks and transfers update counts"),Entries[0].Count,int64(49));
    ACELedger::Count(Entries,100,Objects);TestEqual(TEXT("Other character does not inherit counts"),Entries[0].Count,int64(0));
    TestEqual(TEXT("Out-of-stock item remains tracked"),Entries.Num(),1);

    auto* GI=NewObject<UGameInstance>();GI->Init();
    TestTrue(TEXT("Retail DAT loads for visual verification"),GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(ACETestDatDirectory()));
    auto* H=GI->GetSubsystem<UACEPluginSubsystem>();
    auto* Manager=GI->GetSubsystem<UACEClientSubsystem>()->GetUIElementManager();Manager->SetUiLocked(false);
    H->StorageRoot=FPaths::ProjectSavedDir()/TEXT("Automation/Ledgers");H->Settings=MakeShared<FJsonObject>();
    const auto Session=GI->GetSubsystem<UACEClientSubsystem>()->GetSession();
    {
        auto P=H->Find(TEXT("ucm"));const bool WasEnabled=P->Enabled;P->Enabled=true;
        ON_SCOPE_EXIT{P->Enabled=WasEnabled;};
        auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> Result;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Result);return Result;};
        const auto OldState=Session->State;const auto OldProfile=P->Profile;const auto OldRuntime=H->RuntimeRoute;const int32 OldPoint=H->RoutePoint;
        ON_SCOPE_EXIT{Session->State=OldState;P->Profile=OldProfile;H->RuntimeRoute=OldRuntime;H->RoutePoint=OldPoint;};
        P->Profile=Json(TEXT(R"({"route_preview":false,"route":[{"cell":21168385,"x":10,"y":10,"z":0}]})"));
        TestTrue(TEXT("Map route is hidden while logged out"),H->GetWaypointMapRoute(true).IsEmpty());
        Session->State=EACESessionState::InWorld;FACEPosition Position;Position.CellId=0x01430171;Session->SetLocalPosition(Position);
        TestEqual(TEXT("Map route is independent of world route preview"),H->GetWaypointMapRoute(true).Num(),1);
        H->RuntimeRoute=Json(TEXT(R"({"route":[{"cell":21168385,"x":20,"y":10,"z":0},{"cell":21168386,"x":30,"y":10,"z":0}]})"));H->RoutePoint=2;
        const auto MapRoute=H->GetWaypointMapRoute(true);
        TestTrue(TEXT("Map uses active runtime route and current waypoint instead of the saved profile"),MapRoute.Num()==2&&MapRoute[0].Position.Location.X==20&&MapRoute[1].Current);
    }
    // Exercise the real packet -> session -> subsystem -> ledger path, including
    // luminance-only updates (no XP notification and no UI/ticker refresh).
    auto LoginLedger=[&](int32 Player,TOptional<int64> Balance,int64 Capacity=1500)
    {
        Session->State=EACESessionState::EnteringWorld;Session->PlayerGuid=Player;
        Session->PlayerVitals=FACEPlayerVitals();H->LedgerCharacter.Empty();H->UpdateLedgers();
        FACEBinaryWriter Login;Login.WriteUInt32(0x80);Login.WriteUInt32(1);
        Login.WriteUInt16(Balance.IsSet()?3:2);Login.WriteUInt16(64);
        Login.WriteUInt32(1);Login.WriteUInt64(1000000);
        if(Balance.IsSet()){Login.WriteUInt32(6);Login.WriteUInt64(Balance.GetValue());}
        Login.WriteUInt32(7);Login.WriteUInt64(Capacity);
        for(int32 I=0;I<8;++I)Login.WriteUInt32(0);
        FACEBinaryReader R(Login.GetData());Session->HandlePlayerDescription(R);
        Session->State=EACESessionState::InWorld;
    };
    uint8 Sequence=0;
    auto Quad=[&](int32 Key,int64 Value,int32 PublicPlayer=0)
    {
        FACEBinaryWriter W;W.WriteUInt32(PublicPlayer?ACEOpcode::PublicUpdatePropertyInt64:ACEOpcode::PrivateUpdatePropertyInt64);
        W.WriteUInt8(++Sequence);if(PublicPlayer)W.WriteUInt32(PublicPlayer);
        W.WriteUInt32(Key);W.WriteUInt64(Value);Session->HandleGameMessage(W.GetData());
    };
    LoginLedger(1,500);
    TestEqual(TEXT("Packet login balance is not earned luminance"),H->GetHuntLedger().Lum,int64(0));
    Quad(6,650);
    TestEqual(TEXT("Luminance-only packet immediately reaches Hunt Ledger"),H->GetHuntLedger().Lum,int64(150));
    Quad(7,2000);Quad(6,100);Quad(6,175);
    TestEqual(TEXT("Spend and gain in one frame preserve actual income"),H->GetHuntLedger().Lum,int64(225));
    Quad(6,175);Quad(6,999,2);Quad(6,200,1);
    TestEqual(TEXT("Duplicate and other-player updates do not inflate earnings"),H->GetHuntLedger().Lum,int64(250));
    Quad(6,2000);Quad(6,2000);Quad(1,1000600);
    TestEqual(TEXT("Cap credits only the balance increase, independent of XP"),H->GetHuntLedger().Lum,int64(2050));
    TestEqual(TEXT("Packet XP tracking is unchanged"),H->GetHuntLedger().XP,int64(600));
    H->ResetHuntLedger();Quad(6,1800);Quad(6,1840);
    TestEqual(TEXT("Reset and subsequent spending preserve new gains only"),H->GetHuntLedger().Lum,int64(40));
    LoginLedger(2,5000000000LL,6000000000LL);Quad(6,5000000123LL);
    TestEqual(TEXT("Character switch rebases large custom-server balances without truncation"),H->GetHuntLedger().Lum,int64(123));
    LoginLedger(1,TOptional<int64>());Quad(6,60);
    TestEqual(TEXT("First reward from an omitted zero login balance is income"),H->GetHuntLedger().Lum,int64(60));
    LoginLedger(1,TOptional<int64>(),0);Quad(7,1500);Quad(6,50);
    TestEqual(TEXT("Unlocking luminance during the session does not lose the first reward"),H->GetHuntLedger().Lum,int64(50));
    H->LedgerCharacter.Empty();
    Session->State=EACESessionState::InWorld;Session->PlayerGuid=1;Session->PlayerVitals=V;
    H->UpdateLedgers();const FString FirstCharacter=H->LedgerCharacter;
    Session->WorldObjects=Objects;++Session->InventoryDataRevision;H->UpdateLedgers();
    GI->GetSubsystem<UACEClientSubsystem>()->SelectObject(11);H->EditSupplySelection(true);
    TestEqual(TEXT("Adding the selected item retains its actual inventory icon"),H->GetSupplies()[0].Icon,0x0600262A);
    TestEqual(TEXT("Native ledger refreshes from inventory revisions"),H->GetSupplies()[0].Count,int64(49));
    H->ToggleSupplyName(0);
    Session->PlayerGuid=2;H->UpdateLedgers();TestEqual(TEXT("Second character has a separate list"),H->GetSupplies().Num(),0);
    Session->PlayerGuid=1;H->UpdateLedgers();TestEqual(TEXT("Returning to a character restores its list"),H->GetSupplies().Num(),1);
    FString Saved;TestTrue(TEXT("Supply selection persists to disk"),FFileHelper::LoadFileToString(Saved,*(H->StorageRoot/TEXT("settings.json"))));
    TestTrue(TEXT("Supplies are keyed by character"),Saved.Contains(TEXT("supplies.")+FirstCharacter)&&Saved.Contains(TEXT("Prismatic Taper")));
    TestEqual(TEXT("Item artwork survives character reload"),H->GetSupplies()[0].Icon,0x0600262A);
    TestFalse(TEXT("Icon-only preference survives character reload"),H->AreSupplyNamesVisible());
    H->ToggleSupplyName(0);
    TestEqual(TEXT("Item background type survives character reload"),H->GetSupplies()[0].ItemType,4096);
    H->SetWaypointOption(TEXT("arrow"),false);H->SetWaypointOption(TEXT("dungeon_overlay"),false);
    for(const TCHAR* Id:{TEXT("supplyledger"),TEXT("huntledger")})
    {
        auto P=H->Find(Id);if(!TestTrue(TEXT("HUD is discoverable"),P.IsValid()))continue;P->Enabled=true;
        H->SetLedgerOption(Id,TEXT("visible"),true);H->SavePluginWindowPosition(Id,FVector2D(80,80));
    }
    H->SavePluginWindowPosition(TEXT("huntledger.size"),FVector2D(290,180));
    auto Dock=SNew(SACEPluginDesktop).Host(H);Dock->Tick(FGeometry::MakeRoot(FVector2D(1400,900),FSlateLayoutTransform()),1,.016f);
    TestEqual(TEXT("Legacy fixed Hunt Ledger height adapts to current rows without duplicate editor chrome"),Dock->DisplaySize(Dock->Windows[TEXT("huntledger")]).Y,H->LedgerSize(TEXT("huntledger")).Y);
    TestTrue(TEXT("Saved HUD visibility restores without UCM running"),Dock->IsOpen(TEXT("supplyledger"))&&Dock->IsOpen(TEXT("huntledger")));
    TestEqual(TEXT("Supply HUD defaults to a compact width"),Dock->DisplaySize(Dock->Windows[TEXT("supplyledger")]).X,200.);
    const auto InitialSize=Dock->DisplaySize(Dock->Windows[TEXT("supplyledger")]);
    Dock->Resize(TEXT("supplyledger"),FVector2D(-35,90),true);
    const auto Resized=Dock->DisplaySize(Dock->Windows[TEXT("supplyledger")]);
    TestEqual(TEXT("Supply HUD can be made narrower"),Resized.X,165.);
    TestEqual(TEXT("Supply HUD can be made taller"),Resized.Y,InitialSize.Y+90.);
    Dock->Move(TEXT("supplyledger"),FVector2D(10,20),true);const auto At=Dock->Windows[TEXT("supplyledger")].Position;
    Manager->SetUiLocked(true);Dock->Move(TEXT("supplyledger"),FVector2D(50,50),true);
    TestEqual(TEXT("Pinned HUD cannot move"),Dock->Windows[TEXT("supplyledger")].Position,At);
    auto Reloaded=SNew(SACEPluginDesktop).Host(H);TestEqual(TEXT("HUD restores saved position"),Reloaded->Windows[TEXT("supplyledger")].Position,At);
    TestTrue(TEXT("Pin state survives window recreation"),Manager->IsUiLocked());
    TestEqual(TEXT("Supply width persists through recreation and pinning"),Reloaded->DisplaySize(Reloaded->Windows[TEXT("supplyledger")]).X,Resized.X);
    TestEqual(TEXT("Pinning removes only editor chrome from a resized ledger"),Reloaded->DisplaySize(Reloaded->Windows[TEXT("supplyledger")]).Y,Resized.Y-68.);
    const auto PinnedSize=Dock->Windows[TEXT("supplyledger")].Size;
    Dock->Resize(TEXT("supplyledger"),FVector2D(50,50),true);
    TestEqual(TEXT("Pinned ledger cannot be resized"),Dock->Windows[TEXT("supplyledger")].Size,PinnedSize);
    Dock->Toggle(TEXT("huntledger"));TestFalse(TEXT("Closing HUD persists visibility"),H->LedgerOption(TEXT("huntledger"),TEXT("visible")));
    auto Style=FACEPluginStyle::Use(H);TestTrue(TEXT("Retail glyph atlas resolves"),Style->Atlas!=nullptr);
    TestTrue(TEXT("Retail checkbox artwork resolves"),Style->Check.CheckedImage.GetResourceObject()!=nullptr);
    auto* Resources=GI->GetSubsystem<UACEClientSubsystem>()->GetUIResourceResolver();
    TestTrue(TEXT("Supply artwork resolves"),Style->ItemImage(E,true)->GetResourceObject()!=nullptr);
    TestTrue(TEXT("Supply foreground uses exactly the inventory renderer"),Style->ItemImage(E,true)->GetResourceObject()==Resources->ResolveItemForeground(E.Icon,E.Overlay,E.Effects));
    TestTrue(TEXT("Supply background uses exactly the inventory renderer"),Style->ItemImage(E,false)->GetResourceObject()==Resources->ResolveItemBackground(E.ItemType,E.Underlay));
    const auto Wide=SACEPluginIcon::Fit(FVector2D(90,30));const auto Tall=SACEPluginIcon::Fit(FVector2D(30,90));
    TestTrue(TEXT("Wide toolbar cells preserve square icon proportions"),FMath::IsNearlyEqual(Wide.GetScale(),30.f/32.f)&&FVector2D(Wide.GetTranslation()).Equals(FVector2D(30,0)));
    TestTrue(TEXT("Tall toolbar cells center instead of stretching icons"),FMath::IsNearlyEqual(Tall.GetScale(),30.f/32.f)&&FVector2D(Tall.GetTranslation()).Equals(FVector2D(0,30)));
    if(FApp::CanEverRender())
    {
        H->LedgerSupplies=Entries;H->LedgerSupplies[0].Count=56;++H->SupplyRevision;
        H->HuntLedger=L;H->HuntLedger.Started=FPlatformTime::Seconds()-3600;
        H->HuntLedger.Lum=75;
        Session->PlayerVitals.AvailableLuminance=100;Session->PlayerVitals.MaximumLuminance=100;
        int64 NextThreshold=0;GI->GetSubsystem<UACEDatSubsystem>()->TryGetXpToNextLevel(0,100,NextThreshold);
        Session->PlayerVitals.Level=100;Session->PlayerVitals.TotalExperience=NextThreshold-100000;
        Manager->SetUiLocked(false);H->Find(TEXT("ucm"))->Enabled=true;H->SavePluginWindowPosition(TEXT("ucm"),FVector2D(430,40));Dock->Toggle(TEXT("ucm"));
        H->SavePluginWindowPosition(TEXT("huntledger"),FVector2D(80,380));Dock->Windows[TEXT("huntledger")].Position=FVector2D(80,380);Dock->Toggle(TEXT("huntledger"));
        Dock->Refresh();
        const FVector2D Size(1280,800);FWidgetRenderer Renderer(false,true);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
        for(int32 Pass=0;Pass<6;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();}
        TFunction<void(TSharedRef<SWidget>)> InspectText=[&](TSharedRef<SWidget> W)
        {
            if(W->GetType()==FName(TEXT("SACEPluginText")))
            {
                const auto T=StaticCastSharedRef<SACEPluginText>(W);
                if(T->GetText().ToString().Contains(TEXT("Choose activities")))
                    TestTrue(TEXT("Wrapped plugin help reserves multiple lines"),T->GetDesiredSize().Y>25);
            }
            auto Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)InspectText(Children->GetChildAt(I));
        };InspectText(Dock);
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,[&]{TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);return Pixels;}(),PNG);
        FFileHelper::SaveArrayToFile(PNG,*(H->StorageRoot/TEXT("plugin-windows.png")));
        TFunction<int32(TSharedRef<SWidget>,FName)> CountControls=[&](TSharedRef<SWidget> W,FName Tag)
        {if(!W->GetVisibility().IsVisible())return 0;int32 N=W->GetTag()==Tag?1:0;auto Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)N+=CountControls(Children->GetChildAt(I),Tag);return N;};
        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName)> FindTagged=[&](TSharedRef<SWidget> W,FName Tag)->TSharedPtr<SWidget>
        {if(W->GetTag()==Tag)return W;auto Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=FindTagged(Children->GetChildAt(I),Tag))return Found;return nullptr;};
        auto LumTotal=StaticCastSharedPtr<SACEPluginText>(FindTagged(Dock,TEXT("HuntLuminance2")));
        auto LumAvailable=StaticCastSharedPtr<SACEPluginText>(FindTagged(Dock,TEXT("HuntLuminance3")));
        if(!TestTrue(TEXT("Luminance earned and available cells exist"),LumTotal.IsValid()&&LumAvailable.IsValid()))return false;
        TestEqual(TEXT("Visible luminance total shows earned income"),LumTotal->GetText().ToString(),FString(TEXT("75")));
        TestEqual(TEXT("Available balance explains the luminance cap"),LumAvailable->GetText().ToString(),FString(TEXT("100 (cap)")));
        Session->PlayerVitals.AvailableLuminance=20;
        ++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();
        TestEqual(TEXT("Spending immediately refreshes available luminance without changing earned totals"),LumAvailable->GetText().ToString(),FString(TEXT("20")));
        TestEqual(TEXT("Spending leaves displayed earnings intact"),LumTotal->GetText().ToString(),FString(TEXT("75")));
        auto ResetControl=FindTagged(Dock,TEXT("LedgerReset"));
        const auto HuntBounds=Dock->Windows[TEXT("huntledger")].Widget->GetCachedGeometry();
        if(!TestTrue(TEXT("Hunt Ledger has a reset control"),ResetControl.IsValid()))return false;
        const auto ResetBounds=ResetControl->GetCachedGeometry();
        TestTrue(TEXT("Reset stays above the window resize handle"),ResetBounds.GetAbsolutePosition().Y+ResetBounds.GetAbsoluteSize().Y
            <=HuntBounds.GetAbsolutePosition().Y+HuntBounds.GetAbsoluteSize().Y-20.);
        const auto SupplyContent=FindTagged(Dock->Windows[TEXT("supplyledger")].Widget.ToSharedRef(),TEXT("LedgerContent"));
        const auto HuntContent=FindTagged(Dock->Windows[TEXT("huntledger")].Widget.ToSharedRef(),TEXT("LedgerContent"));
        if(!TestTrue(TEXT("Both ledger frames expose their content"),SupplyContent.IsValid()&&HuntContent.IsValid()))return false;
        const auto SupplyAnchor=SupplyContent->GetCachedGeometry().GetAbsolutePosition();
        const auto HuntAnchor=HuntContent->GetCachedGeometry().GetAbsolutePosition();
        auto Estimate=StaticCastSharedPtr<SACEPluginText>(FindTagged(Dock,TEXT("HuntNextLevel")));
        if(!TestTrue(TEXT("Hunt ledger shows an estimate below totals"),Estimate.IsValid()))return false;
        TestTrue(TEXT("Live estimate uses remaining level XP and earned run rate"),Estimate->GetText().ToString().StartsWith(TEXT("1h")));
        Session->PlayerVitals.Level=100000;
        ++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();
        TestEqual(TEXT("Level cap is reported instead of a fabricated time"),Estimate->GetText().ToString(),FString(TEXT("Max level")));
        Session->PlayerVitals.Level=100;
        const auto BeforeReset=H->HuntLedger;H->ResetHuntLedger();
        ++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();
        TestEqual(TEXT("UI waits for fresh gains after Reset run"),Estimate->GetText().ToString(),FString(TEXT("Waiting for XP")));
        H->HuntLedger=BeforeReset;
        TestTrue(TEXT("Unlocked ledger offers add/remove"),CountControls(Dock,TEXT("LedgerEdit"))>0);
        TestEqual(TEXT("Only one add control plus one remove per supply row"),CountControls(Dock,TEXT("LedgerEdit")),H->GetSupplies().Num()+1);
        TestEqual(TEXT("Unlocked ledgers show both title bars"),CountControls(Dock,TEXT("LedgerHeader")),2);
        Manager->SetUiLocked(true);
        for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();}
        TestEqual(TEXT("Pinned ledger hides add/remove controls"),CountControls(Dock,TEXT("LedgerEdit")),0);
        TestEqual(TEXT("Pinned hunt ledger hides reset"),CountControls(Dock,TEXT("LedgerReset")),0);
        TestEqual(TEXT("Pinned ledgers hide both title bars"),CountControls(Dock,TEXT("LedgerHeader")),0);
        TestTrue(TEXT("Pinning supply ledger keeps rows at the same screen position"),SupplyContent->GetCachedGeometry().GetAbsolutePosition().Equals(SupplyAnchor,.01f));
        TestTrue(TEXT("Pinning hunt ledger keeps totals at the same screen position"),HuntContent->GetCachedGeometry().GetAbsolutePosition().Equals(HuntAnchor,.01f));
        TestEqual(TEXT("The level estimate remains visible when pinned"),CountControls(Dock,TEXT("HuntNextLevel")),1);
        PNG.Reset();FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,[&]{TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);return Pixels;}(),PNG);
        FFileHelper::SaveArrayToFile(PNG,*(H->StorageRoot/TEXT("plugin-windows-pinned.png")));
        Manager->SetQuickShowUi(true);
        for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();}
        TestEqual(TEXT("Quick-show restores both title bars"),CountControls(Dock,TEXT("LedgerHeader")),2);
        TestTrue(TEXT("Quick-show restores inventory controls"),CountControls(Dock,TEXT("LedgerEdit"))>0);
        TestFalse(TEXT("Quick-show temporarily unlocks shared UI"),Manager->IsUiLocked());
        TestTrue(TEXT("Quick-show does not shift supply rows"),SupplyContent->GetCachedGeometry().GetAbsolutePosition().Equals(SupplyAnchor,.01f));
        TestTrue(TEXT("Quick-show does not shift hunt totals"),HuntContent->GetCachedGeometry().GetAbsolutePosition().Equals(HuntAnchor,.01f));
        Dock->Move(TEXT("supplyledger"),FVector2D(10,20),true);
        const auto Moved=Dock->Windows[TEXT("supplyledger")].Position;
        TestFalse(TEXT("Quick-show permits moving a pinned HUD"),Moved.Equals(At));
        Manager->SetQuickShowUi(false);
        for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();}
        TestTrue(TEXT("Releasing quick-show restores permanent lock"),Manager->IsUiLocked());
        TestEqual(TEXT("Releasing quick-show hides header again"),CountControls(Dock,TEXT("LedgerHeader")),0);
        TestEqual(TEXT("Releasing quick-show preserves new position"),Dock->Windows[TEXT("supplyledger")].Position,Moved);
        Manager->ToggleUiLocked();
        for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();}
        TestEqual(TEXT("Radar lock toggle unlocks both ledgers"),CountControls(Dock,TEXT("LedgerHeader")),2);
        Manager->SetQuickShowUi(true);Manager->SetQuickShowUi(false);
        TestFalse(TEXT("Quick-show release does not lock permanently unlocked UI"),Manager->IsUiLocked());
        for(const TCHAR* Id:{TEXT("supplyledger"),TEXT("huntledger")})Dock->Windows[Id].Position.Y=790;
        for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();}
        const auto BottomSupply=SupplyContent->GetCachedGeometry().GetAbsolutePosition(),BottomHunt=HuntContent->GetCachedGeometry().GetAbsolutePosition();
        Manager->SetUiLocked(true);
        for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();}
        TestTrue(TEXT("Bottom-edge clamping keeps supply content fixed when pinning"),SupplyContent->GetCachedGeometry().GetAbsolutePosition().Equals(BottomSupply,.01f));
        TestTrue(TEXT("Bottom-edge clamping keeps hunt content fixed when pinning"),HuntContent->GetCachedGeometry().GetAbsolutePosition().Equals(BottomHunt,.01f));
        // Independent pinning and opacity work without the global interface lock.
        Manager->SetUiLocked(false);
        auto Draw=[&]{for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Dock,Size,0);FlushRenderingCommands();}};
        auto Click=[&](TSharedPtr<SWidget> Button)
        {
            // Exercise the button's activation delegate without inventing mouse capture
            // for this offscreen widget tree.
            const FKeyEvent Accept(EKeys::Enter,FModifierKeysState(),0,false,0,0);
            Button->OnKeyDown(Button->GetCachedGeometry(),Accept);
            Button->OnKeyUp(Button->GetCachedGeometry(),Accept);
        };
        Draw();
        const auto IndependentAnchor=SupplyContent->GetCachedGeometry().GetAbsolutePosition();
        H->SetPluginWindowPinned(TEXT("supplyledger"),true);Draw();
        TestTrue(TEXT("Local pin keeps supply content anchored"),SupplyContent->GetCachedGeometry().GetAbsolutePosition().Equals(IndependentAnchor,.01f));
        TestFalse(TEXT("Local pin locks manipulation even with interface unlocked"),H->CanManipulatePluginWindow(TEXT("supplyledger")));
        TestTrue(TEXT("Other plugin remains movable"),H->CanManipulatePluginWindow(TEXT("huntledger")));
        auto SupplyStyle=FACEPluginStyle::Use(H,TEXT("supplyledger"));auto HuntStyle=FACEPluginStyle::Use(H,TEXT("huntledger"));
        TestEqual(TEXT("Pinned window retains its background"),SupplyStyle->Frame.TintColor.GetSpecifiedColor().A,.96f);
        TestEqual(TEXT("Pinned frame outline is transparent"),SupplyStyle->Frame.OutlineSettings.Color.GetSpecifiedColor().A,0.f);
        FACESupplyEntry Second=E;Second.ClassId=20632;Second.Name=TEXT("Second supply");H->LedgerSupplies.Add(Second);H->SaveSupplies();Draw();
        const auto Name=FindTagged(Dock,TEXT("SupplyName.0")),Icon=FindTagged(Dock,TEXT("SupplyIcon.1")),Count=FindTagged(Dock,TEXT("SupplyCount.0"));
        if(!TestTrue(TEXT("Supply row exposes name/icon/count"),Name&&Icon&&Count))return false;
        const auto CountAnchor=Count->GetCachedGeometry().GetAbsolutePosition();
        const auto NameToggleSize=Dock->DisplaySize(Dock->Windows[TEXT("supplyledger")]);
        const uint32 Revision=H->GetSupplyRevision();Click(Name);Draw();
        TestFalse(TEXT("Clicking the item name hides its label"),Name->GetVisibility().IsVisible());
        TestFalse(TEXT("Clicking one name also hides every other name"),FindTagged(Dock,TEXT("SupplyName.1"))->GetVisibility().IsVisible());
        TestEqual(TEXT("Name toggle does not rebuild the ledger"),H->GetSupplyRevision(),Revision);
        TestTrue(TEXT("Count retains its widget and stays visible"),Count==FindTagged(Dock,TEXT("SupplyCount.0"))&&Count->GetVisibility().IsVisible());
        TestTrue(TEXT("Hiding names leaves the count in exactly the same place"),Count->GetCachedGeometry().GetAbsolutePosition().Equals(CountAnchor,.01f));
        TestEqual(TEXT("Name toggle never resizes the window"),Dock->DisplaySize(Dock->Windows[TEXT("supplyledger")]),NameToggleSize);
        Click(Icon);Draw();TestTrue(TEXT("Clicking a different icon restores all names"),Name->GetVisibility().IsVisible()&&FindTagged(Dock,TEXT("SupplyName.1"))->GetVisibility().IsVisible());
        TestTrue(TEXT("Restoring names also preserves the count position"),Count->GetCachedGeometry().GetAbsolutePosition().Equals(CountAnchor,.01f));
        H->SetPluginWindowOpacity(TEXT("huntledger"),.25f);Draw();
        TestTrue(TEXT("Unrelated window has its own opacity"),FMath::IsNearlyEqual(HuntStyle->Frame.TintColor.GetSpecifiedColor().A,.24f));
        H->SetPluginWindowOpacity(TEXT("supplyledger"),.5f);Draw();
        const float PinnedBackground=SupplyStyle->Background.TintColor.GetSpecifiedColor().A;
        TestTrue(TEXT("Pinned window honors its own opacity"),FMath::IsNearlyEqual(SupplyStyle->Frame.TintColor.GetSpecifiedColor().A,.48f));
        H->SetPluginWindowOpacity(TEXT(""),.75f);H->SetPluginOpacityGlobal(true);Draw();
        TestEqual(TEXT("Global opacity applies to pinned backgrounds"),SupplyStyle->Frame.TintColor.GetSpecifiedColor().A,.72f,.0001f);
        TestEqual(TEXT("Global opacity applies to unpinned backgrounds"),HuntStyle->Frame.TintColor.GetSpecifiedColor().A,.72f,.0001f);
        TestEqual(TEXT("Global opacity does not restore a pinned border"),SupplyStyle->Frame.OutlineSettings.Color.GetSpecifiedColor().A,0.f);
        H->SetPluginOpacityGlobal(false);H->SetPluginWindowOpacity(TEXT(""),1.f);Draw();
        TestTrue(TEXT("Disabling global override restores both window opacities"),
            FMath::IsNearlyEqual(SupplyStyle->Frame.TintColor.GetSpecifiedColor().A,.48f)&&
            FMath::IsNearlyEqual(HuntStyle->Frame.TintColor.GetSpecifiedColor().A,.24f));
        TestEqual(TEXT("Opacity leaves foreground widgets opaque"),Count->GetRenderOpacity(),1.f);
        TestEqual(TEXT("Opacity leaves item artwork opaque"),SupplyStyle->ItemImage(E,true)->TintColor.GetSpecifiedColor().A,1.f);
        TestTrue(TEXT("Opacity leaves existing widget tree intact"),Count==FindTagged(Dock,TEXT("SupplyCount.0")));
        H->SetPluginWindowOpacity(TEXT("ucm"),.4f);
        TestEqual(TEXT("Related UCM log inherits the chosen opacity"),H->GetPluginWindowOpacity(TEXT("ucm.log")),.4f);
        Manager->SetQuickShowUi(true);Draw();
        TestTrue(TEXT("Quick-show restores a locally pinned window's controls"),H->CanManipulatePluginWindow(TEXT("supplyledger")));
        TestTrue(TEXT("Quick-show restores the local frame outline"),SupplyStyle->Frame.OutlineSettings.Color.GetSpecifiedColor().A>0);
        TestTrue(TEXT("Quick-show preserves background opacity"),FMath::IsNearlyEqual(SupplyStyle->Frame.TintColor.GetSpecifiedColor().A,.48f));
        TestEqual(TEXT("Quick-show preserves inner background opacity"),SupplyStyle->Background.TintColor.GetSpecifiedColor().A,PinnedBackground);
        H->SetPluginWindowPinned(TEXT("supplyledger"),false);Manager->SetQuickShowUi(false);Draw();
        TestTrue(TEXT("Unpinning preserves background opacity"),FMath::IsNearlyEqual(SupplyStyle->Frame.TintColor.GetSpecifiedColor().A,.48f));
        TestEqual(TEXT("Unpinning preserves inner background opacity"),SupplyStyle->Background.TintColor.GetSpecifiedColor().A,PinnedBackground);
        H->SetPluginWindowPinned(TEXT("supplyledger"),true);Manager->SetQuickShowUi(true);Draw();
        Dock->Resize(TEXT("supplyledger"),FVector2D(-10000,-10000),true);
        TestEqual(TEXT("Supply supports a narrow icon counter"),Dock->Windows[TEXT("supplyledger")].Size.X,72.);
        Dock->Windows[TEXT("supplyledger")].Position=FVector2D(80,80);H->ToggleSupplyName(0);H->LedgerSupplies[0].Count=8544;
        Manager->SetQuickShowUi(false);Draw();
        auto CompactCount=FindTagged(Dock,TEXT("SupplyCount.0"));
        const auto CounterBounds=CompactCount->GetCachedGeometry();const auto FrameBounds=Dock->Windows[TEXT("supplyledger")].Widget->GetCachedGeometry();
        TestTrue(TEXT("Four-digit count fits inside compact frame"),CounterBounds.GetAbsolutePosition().X+CounterBounds.GetAbsoluteSize().X<=FrameBounds.GetAbsolutePosition().X+FrameBounds.GetAbsoluteSize().X);
        TestTrue(TEXT("Compact count is fully allocated"),CounterBounds.GetLocalSize().X>=CompactCount->GetDesiredSize().X);
        const auto ListBounds=FindTagged(Dock,TEXT("SupplyRows"))->GetCachedGeometry();
        TestTrue(TEXT("Scrollbar never covers digits in the narrow ledger"),CounterBounds.GetAbsolutePosition().X+CounterBounds.GetAbsoluteSize().X<=ListBounds.GetAbsolutePosition().X+ListBounds.GetAbsoluteSize().X);
        // Sample empty space inside the actual pinned window, away from item artwork
        // and controls, so a visible backdrop is verified beyond brush properties.
        auto BackgroundAlpha=[&](float Opacity)
        {
            H->SetPluginWindowOpacity(TEXT("supplyledger"),Opacity);Draw();
            TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            const FVector2D Sample=FrameBounds.GetAbsolutePosition()+FVector2D(6,FrameBounds.GetAbsoluteSize().Y-6);
            return Pixels[FMath::FloorToInt(Sample.Y)*Target->SizeX+FMath::FloorToInt(Sample.X)].A;
        };
        const uint8 ClearAlpha=BackgroundAlpha(0.f),HalfAlpha=BackgroundAlpha(.5f),FullAlpha=BackgroundAlpha(1.f);
        TestTrue(TEXT("Pinned background renders from transparent through half to full opacity"),ClearAlpha<5&&HalfAlpha>100&&HalfAlpha<150&&FullAlpha>230);
        PNG.Reset();FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,[&]{TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);return Pixels;}(),PNG);
        FFileHelper::SaveArrayToFile(PNG,*(H->StorageRoot/TEXT("plugin-windows-compact.png")));
        while(!H->GetSupplies().IsEmpty())H->RemoveSupply(0);
        Draw();
        const auto SupplyWindow=Dock->Windows[TEXT("supplyledger")].Widget.ToSharedRef();
        TestEqual(TEXT("Empty pinned ledger keeps only the add control"),CountControls(SupplyWindow,TEXT("LedgerEdit")),1);
        TestEqual(TEXT("Empty pinned ledger still hides the header"),CountControls(SupplyWindow,TEXT("LedgerHeader")),0);
        auto Add=FindTagged(SupplyWindow,TEXT("LedgerEdit"));
        if(!TestTrue(TEXT("Empty pinned ledger exposes add"),Add.IsValid()))return false;
        const auto AddBounds=Add->GetCachedGeometry(),EmptyBounds=SupplyWindow->GetCachedGeometry();
        const auto AddCaption=StaticCastSharedPtr<SButton>(Add)->GetContent();
        const auto CaptionBounds=AddCaption->GetCachedGeometry();
        const auto CaptionInset=AddBounds.AbsoluteToLocal(CaptionBounds.GetAbsolutePosition());
        TestTrue(TEXT("Supply add button reserves a readable interior inside the retail bevel"),
            AddBounds.GetLocalSize().X>=28&&AddBounds.GetLocalSize().Y>=28&&CaptionInset.X>=5.9&&CaptionInset.Y>=5.9);
        TestTrue(TEXT("Supply add symbol is fully allocated and centered"),
            CaptionBounds.GetLocalSize().X>=AddCaption->GetDesiredSize().X&&CaptionBounds.GetLocalSize().Y>=AddCaption->GetDesiredSize().Y&&
            AddBounds.AbsoluteToLocal(CaptionBounds.LocalToAbsolute(CaptionBounds.GetLocalSize()*.5)).Equals(AddBounds.GetLocalSize()*.5,.1f));
        TestTrue(TEXT("Add control fits the minimum-size pinned ledger"),AddBounds.GetAbsolutePosition().Y>=EmptyBounds.GetAbsolutePosition().Y&&
            AddBounds.GetAbsolutePosition().Y+AddBounds.GetAbsoluteSize().Y<=EmptyBounds.GetAbsolutePosition().Y+EmptyBounds.GetAbsoluteSize().Y);
        PNG.Reset();FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,[&]{TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);return Pixels;}(),PNG);
        FFileHelper::SaveArrayToFile(PNG,*(H->StorageRoot/TEXT("plugin-windows-empty.png")));
        GI->GetSubsystem<UACEClientSubsystem>()->SelectObject(11);Click(Add);Draw();
        TestEqual(TEXT("Pinned add button tracks the selected item"),H->GetSupplies().Num(),1);
        TestEqual(TEXT("Populated pinned ledger hides the add interface again"),CountControls(SupplyWindow,TEXT("LedgerEdit")),0);
        TestFalse(TEXT("Removing and adding supplies retains global names preference"),H->AreSupplyNamesVisible());
        H->SetWaypointMapZoom(true,false,12.);H->SetWaypointMapZoom(true,true,4.5);
        H->SetWaypointMapZoom(false,false,2.);H->SetWaypointMapZoom(false,true,6.);
        H->SetWaypointOption(TEXT("fill_dungeon"),true);H->SetWaypointOption(TEXT("show_route"),false);
        const auto Waypoint=H->Find(TEXT("waypoint"));const FString WaypointProfile=Waypoint->ProfileName;
        Waypoint->Profile=MakeShared<FJsonObject>();
        TestTrue(TEXT("Map profile reload succeeds"),H->LoadProfile(TEXT("waypoint"),WaypointProfile));
        TestTrue(TEXT("Walkable fill survives profile reload"),H->WaypointOption(TEXT("fill_dungeon"),false));
        TestFalse(TEXT("Route visibility survives profile reload"),H->WaypointOption(TEXT("show_route"),true));
        Dock->SaveOverlayLayout();const auto SavedPosition=Dock->Windows[TEXT("supplyledger")].Position;
        FString AppearanceSaved;FFileHelper::LoadFileToString(AppearanceSaved,*(H->StorageRoot/TEXT("settings.json")));
        TSharedPtr<FJsonObject> Restored;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(AppearanceSaved),Restored);H->Settings=Restored;
        TestTrue(TEXT("Pin survives settings reload"),H->IsPluginWindowPinned(TEXT("supplyledger")));
        TestEqual(TEXT("Opacity survives settings reload"),H->GetPluginWindowOpacity(TEXT("huntledger")),.25f);
        H->LedgerCharacter.Empty();H->UpdateLedgers();
        TestEqual(TEXT("Disk reload restores the tracked supply list"),H->GetSupplies().Num(),1);
        TestFalse(TEXT("Disk reload restores the ledger-wide name toggle"),H->AreSupplyNamesVisible());
        TestEqual(TEXT("Disk reload retains actual item artwork"),H->GetSupplies()[0].Icon,0x0600262A);
        TestEqual(TEXT("Disk reload recounts the current inventory"),H->GetSupplies()[0].Count,int64(49));
        TestEqual(TEXT("Pinned outdoor zoom survives disk reload"),H->WaypointMapZoom(true,false),12.);
        TestEqual(TEXT("Pinned indoor zoom survives disk reload"),H->WaypointMapZoom(true,true),4.5);
        TestEqual(TEXT("Window outdoor zoom survives disk reload"),H->WaypointMapZoom(false,false),2.);
        TestEqual(TEXT("Window indoor zoom survives disk reload"),H->WaypointMapZoom(false,true),6.);
        auto DiskDock=SNew(SACEPluginDesktop).Host(H);
        TestEqual(TEXT("Disk reload restores window position"),DiskDock->Windows[TEXT("supplyledger")].Position,SavedPosition);
        TestEqual(TEXT("Disk reload restores compact width"),DiskDock->DisplaySize(DiskDock->Windows[TEXT("supplyledger")]).X,72.);
        TestTrue(TEXT("Plugin bar has an options gear"),FindTagged(DiskDock,TEXT("PluginOptions")).IsValid());
        auto Icons=SNew(SHorizontalBox);
        for(const TCHAR* Id:{TEXT("plugin.options"),TEXT("ucm"),TEXT("ucmmicro"),TEXT("looteditor"),TEXT("itemtools"),TEXT("waypoint"),TEXT("waypoint.map"),TEXT("ucm.log"),TEXT("supplyledger"),TEXT("huntledger"),TEXT("transmog"),TEXT("custom")})
            Icons->AddSlot().AutoWidth()[SNew(SBox).WidthOverride(70).HeightOverride(50)[SNew(SACEPluginIcon).Id(Id)]];
        Renderer.DrawWidget(Target,Icons,FVector2D(840,50),0);FlushRenderingCommands();
        PNG.Reset();FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,[&]{TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);return Pixels;}(),PNG);
        FFileHelper::SaveArrayToFile(PNG,*(H->StorageRoot/TEXT("plugin-icons.png")));
        auto Labels=SNew(SVerticalBox);
        double PreviousTextHeight=0;
        for(int32 FontSize:{10,12,14,16,18,24})
        {
            auto Label=SNew(SACEPluginText).Text(FText::FromString(TEXT("Pin map to interface - illi iii Inventory"))).Font(FCoreStyle::GetDefaultFontStyle("Regular",FontSize));
            Labels->AddSlot().AutoHeight()[Label];
            Label->SlatePrepass(1); // Evaluate the bound font attribute before measuring.
            const double TextHeight=Label->ComputeDesiredSize(1).Y;
            TestTrue(TEXT("Requested plugin text sizes produce increasing line heights"),TextHeight>PreviousTextHeight);
            PreviousTextHeight=TextHeight;
        }
        Renderer.DrawWidget(Target,Labels,FVector2D(770,220),0);FlushRenderingCommands();
        PNG.Reset();FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,[&]{TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);return Pixels;}(),PNG);
        FFileHelper::SaveArrayToFile(PNG,*(H->StorageRoot/TEXT("plugin-text.png")));
        Target->ReleaseResource();
    }
    Session->State=EACESessionState::Disconnected;GI->Shutdown();return true;
}
#endif
