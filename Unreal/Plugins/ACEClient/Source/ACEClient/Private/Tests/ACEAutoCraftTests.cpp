#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Mods/ACEAutoCraft.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Engine/GameInstance.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Protocol/ACEBinaryReader.h"
#include "Protocol/ACEIsaac.h"
#include "ACEOpcodes.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "RenderingThread.h"
#include "ImageUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEAutoCraftTest,"ACE.Plugins.AutoCraft.Production",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEAutoCraftTest::RunTest(const FString&)
{
    using namespace ACEAutoCraft;
    FCatalog Catalog;const auto Directory=IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/autocraft");
    if(!TestTrue(TEXT("Bundled catalog loads"),Catalog.Load(Directory/TEXT("catalog.json"))))return false;
    TestTrue(TEXT("Full production coverage has at least 4700 outputs"),Catalog.ByOutput.Num()>=4700);
    TestTrue(TEXT("Alternate ingredient recipes retained"),Catalog.Recipes.Num()>11000);
    const auto* ArrowIndex=Catalog.ById.Find(TEXT("29794"));const auto* SoupIndex=Catalog.ById.Find(TEXT("219"));
    if(!TestNotNull(TEXT("Prismatic arrows are present"),ArrowIndex)||!TestNotNull(TEXT("Hearty carrot soup is present"),SoupIndex))return false;
    const auto& Arrow=Catalog.Recipes[*ArrowIndex];const auto& Soup=Catalog.Recipes[*SoupIndex];
    TestEqual(TEXT("Retail arrow yield"),Arrow.Yield,100);TestEqual(TEXT("Retail arrow source direction"),Arrow.Source,44070);
    TestEqual(TEXT("Soup uses victual oil"),Soup.Source,5336);TestEqual(TEXT("Soup has cooking category"),Soup.Category,FString(TEXT("Cooking")));
    FInventory Inventory;Inventory.Counts={{Arrow.Source,7},{Arrow.Target,5},{Arrow.Output,200}};
    auto P=Plan(Catalog,*ArrowIndex,Inventory,101,false);
    TestTrue(TEXT("Partial batch request is feasible"),P.Valid);TestEqual(TEXT("Additional units round up without counting existing arrows"),P.Units,200);TestEqual(TEXT("Two operations for two batches"),P.Operations,2);
    P=Maximum(Catalog,*ArrowIndex,Inventory,false);TestTrue(TEXT("Maximum is feasible"),P.Valid);TestEqual(TEXT("Maximum uses limiting reagent"),P.Units,500);
    TestFalse(TEXT("Missing ingredients are not silently skipped"),Plan(Catalog,*SoupIndex,Inventory,1,false).Valid);
    {
        const FInventory Empty;
        const auto Missing=InspectMissing(Catalog,*ArrowIndex,Empty,101,false);
        TestEqual(TEXT("Checklist lists both absent operands"),Missing.Missing.Num(),2);
        TestEqual(TEXT("Missing heads use rounded batch count"),Missing.Missing.FindRef(Arrow.Source),int64(2));
        TestEqual(TEXT("Missing shafts use rounded batch count"),Missing.Missing.FindRef(Arrow.Target),int64(2));
        TestFalse(TEXT("Diagnostic supplies never authorize execution"),Missing.Valid);
        TestFalse(TEXT("Checklist never modifies real inventory"),Plan(Catalog,*ArrowIndex,Empty,101,false).Valid);
        const auto Max=Maximum(Catalog,*ArrowIndex,Empty,false);
        TestFalse(TEXT("Zero available batches cannot start"),Max.Valid);
        TestEqual(TEXT("Zero maximum diagnoses first whole batch"),Max.Units,Arrow.Yield);
    }
    {
        FCatalog C;for(int32 Id=1;Id<=8;++Id){FItem Item;Item.Id=Id;Item.Name=LexToString(Id);C.Items.Add(Id,Item);}
        auto Add=[&](int32 Source,int32 Target,int32 Output,int32 Yield,int32 SourceCount,int32 TargetCount,bool Chain=true)
        {FRecipe R;R.Id=LexToString(C.Recipes.Num());R.Source=Source;R.Target=Target;R.Output=Output;R.Yield=Yield;R.SourceCount=SourceCount;R.TargetCount=TargetCount;R.Chain=Chain;
            C.ByOutput.FindOrAdd(Output).Add(C.Recipes.Num());C.Recipes.Add(R);};
        Add(1,2,3,2,0,1); // Reusable tool + raw material -> two intermediates.
        Add(3,4,5,1,1,1);
        FInventory I;I.RetainedTools.Add(1,1);I.Counts={{2,3},{4,6}};
        auto Chain=Plan(C,1,I,6,true);TestTrue(TEXT("Recursive ingredients use retained reusable tool"),Chain.Valid);
        TestEqual(TEXT("Intermediate batches use yield correctly"),Chain.Operations,9);TestEqual(TEXT("Dependency is made before output"),Chain.Steps[0].Recipe,0);
        TestFalse(TEXT("Do not make ingredients option is honored"),Plan(C,1,I,1,false).Valid);
        TestEqual(TEXT("Maximum includes recursive material bottleneck"),Maximum(C,1,I,true).Units,6);
        FInventory Short;Short.Counts={{2,1},{4,2}};
        const auto Shopping=InspectMissing(C,1,Short,6,true);
        TestEqual(TEXT("Recursive checklist lists all raw shortages and tools"),Shopping.Missing.Num(),3);
        TestEqual(TEXT("Missing reusable tool is acquired once"),Shopping.Missing.FindRef(1),int64(1));
        TestEqual(TEXT("Intermediate yield reduces raw shortage"),Shopping.Missing.FindRef(2),int64(2));
        TestEqual(TEXT("Final operand shortage includes all batches"),Shopping.Missing.FindRef(4),int64(4));
        TestFalse(TEXT("Checklist does not claim hypothetical steps can run"),Shopping.Valid);
        const auto Direct=InspectMissing(C,1,Short,6,false);
        TestEqual(TEXT("Without ingredient crafting report missing intermediate"),Direct.Missing.FindRef(3),int64(6));
        TestFalse(TEXT("Without ingredient crafting do not request its tool"),Direct.Missing.Contains(1));
        Short.RetainedTools.Add(1,1);
        TestFalse(TEXT("Retained reusable tool satisfies checklist"),InspectMissing(C,1,Short,6,true).Missing.Contains(1));
        I.Counts.Remove(2);I.RetainedTools.Add(2,3);TestFalse(TEXT("Protected materials cannot be consumed"),Plan(C,1,I,1,true).Valid);
        // Both branches need the same raw ingredient. Counting each separately
        // would overestimate the available maximum by a factor of two.
        C.Recipes[0].SourceCount=1;C.Recipes[0].Yield=1;C.Recipes[1].Target=2;I.RetainedTools.Reset();I.Counts={{1,3},{2,5}};
        TestEqual(TEXT("Shared ingredient stock is reserved across branches"),Maximum(C,1,I,true).Units,2);
        TestEqual(TEXT("Checklist reserves shared raw stock across branches"),InspectMissing(C,1,I,3,true).Missing.FindRef(2),int64(1));
        Add(6,7,2,1,1,1);Add(2,7,6,1,1,1);I.Counts={{1,3},{7,10}};
        const double Begin=FPlatformTime::Seconds();TestFalse(TEXT("Cyclic recipes fail cleanly"),Plan(C,1,I,1,true).Valid);
        TestTrue(TEXT("Cycles have bounded planning work"),FPlatformTime::Seconds()-Begin<1.0);
        const double InspectBegin=FPlatformTime::Seconds();const auto CycleList=InspectMissing(C,1,I,1,true);
        TestTrue(TEXT("Cyclic checklist remains bounded and nonexecutable"),FPlatformTime::Seconds()-InspectBegin<1.0&&!CycleList.Valid);
        Add(1,2,8,1,0,0);TestFalse(TEXT("Max does not loop on nonconsuming recipes"),Maximum(C,4,I,true).Valid);
        Add(2,2,8,1,1,1);I.Counts={{2,1}};
        TestEqual(TEXT("Same-type operands aggregate missing quantities"),InspectMissing(C,5,I,2,false).Missing.FindRef(2),int64(3));
    }
    {
        auto After=Inventory;
        TestTrue(TEXT("UseDone without inventory receipt remains pending"),Outcome(Arrow,Inventory,After)==EOutcome::Waiting);
        After.Counts[Arrow.Source]--;After.Counts[Arrow.Target]--;After.Counts[Arrow.Output]+=100;
        TestTrue(TEXT("Actual output receipt confirms success"),Outcome(Arrow,Inventory,After)==EOutcome::Success);
        After.Counts[Arrow.Output]-=100;TestTrue(TEXT("Consumption without output is a failed attempt"),Outcome(Arrow,Inventory,After)==EOutcome::Failure);
        After.Counts[Arrow.Output]+=50;TestTrue(TEXT("Unexpected output count is not called success"),Outcome(Arrow,Inventory,After)==EOutcome::Unexpected);
        After=Inventory;After.Counts[Arrow.Output]+=100;TestTrue(TEXT("Output alone waits for input consumption"),Outcome(Arrow,Inventory,After)==EOutcome::Unexpected);
        auto Optional=Arrow;Optional.OptionalSource=true;After.Counts[Arrow.Target]--;
        TestTrue(TEXT("Probabilistic source survival is accepted"),Outcome(Optional,Inventory,After)==EOutcome::Success);
        auto Before=Inventory;Before.HasReceipts=true;Before.ReceiptCounts=Before.Counts;
        After=Before;After.ReceiptCounts[Arrow.Source]--;After.ReceiptCounts[Arrow.Target]--;After.ReceiptCounts[Arrow.Output]+=Arrow.Yield;
        TestTrue(TEXT("Protected or container outputs still count as delivered"),Outcome(Arrow,Before,After)==EOutcome::Success);
    }
    // A real native host + loopback transport verifies direction, ownership,
    // pending confirmation, pause, timeout and the Plugin Manager lifecycle.
    auto* GI=NewObject<UGameInstance>();GI->Init();ON_SCOPE_EXIT{GI->Shutdown();};
    auto* C=GI->GetSubsystem<UACEClientSubsystem>();auto* H=GI->GetSubsystem<UACEPluginSubsystem>();
    H->StorageRoot=FPaths::ProjectSavedDir()/TEXT("AutoCraftTests")/FGuid::NewGuid().ToString(EGuidFormats::Digits);H->Settings=MakeShared<FJsonObject>();
    C->Session=MakeShared<FACESession>();auto& S=*C->Session;S.State=EACESessionState::InWorld;S.PlayerGuid=0x50000001;
    S.PlayerVitals.bValid=true;S.PlayerVitals.Health=100;S.PlayerVitals.CombatMode=1;S.PlayerVitals.StatQualityInts.Add(40,1);
    FACESkillInfo Skill;Skill.SkillId=37;Skill.AdvancementClass=2;Skill.Current=400;S.PlayerVitals.Skills.Add(Skill);
    FACEWorldObject Self;Self.Guid=S.PlayerGuid;Self.ItemsCapacity=120;Self.ContainersCapacity=7;S.WorldObjects.Add(Self.Guid,Self);
    auto Item=[&](int32 Id,int32 Wcid,int32 Count){FACEWorldObject O;O.Guid=Id;O.WeenieClassId=Wcid;O.ContainerId=S.PlayerGuid;O.StackSize=Count;O.MaxStackSize=1000;O.Name=Catalog.Name(Wcid);S.WorldObjects.Add(Id,O);};
    Item(10,Arrow.Source,2);Item(11,Arrow.Target,2);
    auto Plugin=H->Find(TEXT("autocraft"));if(!TestTrue(TEXT("Discovered as independent Plugin Manager entry"),Plugin.IsValid()))return false;
    H->RefreshAutoCraft(true);Plugin->Profile->SetStringField(TEXT("recipe"),Arrow.Id);Plugin->Profile->SetNumberField(TEXT("quantity"),200);
    H->SetEnabled(Plugin->Id,true);TestTrue(TEXT("Enable checkbox grants the advertised capabilities"),Plugin->Enabled);
    TestTrue(TEXT("Selections persist to the character profile"),H->SaveCurrentProfile(Plugin->Id,false));
    S.PlayerGuid++;H->RefreshAutoCraft(true);TestTrue(TEXT("Another character has independent selection"),!H->AutoCraft->Preview.Valid);
    S.PlayerGuid--;H->RefreshAutoCraft(true);TestTrue(TEXT("Original character selection restored"),H->AutoCraft->Preview.Valid);
    {
        FACEWorldObject Pack;Pack.Guid=40;Pack.ContainerId=S.PlayerGuid;Pack.ItemType=ACEItemType::Container;Pack.ItemsCapacity=24;S.WorldObjects.Add(Pack.Guid,Pack);
        S.WorldObjects[10].ContainerId=Pack.Guid;S.WorldObjects[10].StackSize=1;
        S.WorldObjects[11].ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;
        Item(41,Arrow.Source,20);S.WorldObjects[41].ContainerId=0; // Nearby loot is not owned stock.
        ++S.InventoryDataRevision;H->AutoCraft->NextRefresh=0;H->RefreshAutoCraft();
        TestEqual(TEXT("Inventory detects inputs inside another pack"),H->AutoCraft->Inventory.Available(Arrow.Source),int64(1));
        TestEqual(TEXT("Protected inputs still appear as owned"),H->AutoCraft->Inventory.Receipt(Arrow.Target),int64(2));
        TestEqual(TEXT("Protected inputs are unavailable for consumption"),H->AutoCraft->Inventory.Available(Arrow.Target),int64(0));
        TestTrue(TEXT("Summary names both missing ingredients"),H->AutoCraft->InventorySummary.Contains(Catalog.Name(Arrow.Source))&&H->AutoCraft->InventorySummary.Contains(Catalog.Name(Arrow.Target)));
        TestTrue(TEXT("Details explain why owned stock cannot be used"),H->AutoCraft->Details.Contains(TEXT("2 unavailable: retained")));
        TestTrue(TEXT("Details include quantity-aware input counts"),H->AutoCraft->Details.Contains(TEXT("1 / 1 / 2"))&&H->AutoCraft->Details.Contains(TEXT("2 / 0 / 2")));
        TestFalse(TEXT("Incomplete inventory disables executable preview"),H->AutoCraft->Preview.Valid);
        Item(42,Arrow.Source,1);S.WorldObjects[42].ContainerId=0;S.WorldObjects[42].WielderId=S.PlayerGuid;
        ++S.InventoryDataRevision;H->AutoCraft->NextRefresh=0;H->RefreshAutoCraft();
        TestEqual(TEXT("Equipped items are counted as owned"),H->AutoCraft->Inventory.Receipt(Arrow.Source),int64(2));
        TestEqual(TEXT("Equipped items cannot satisfy consumable shortage"),H->AutoCraft->Inventory.Available(Arrow.Source),int64(1));
        S.WorldObjects.Remove(42);
        S.WorldObjects[10].StackSize=2;S.WorldObjects[11].ObjectDescriptionFlags&=~ACEObjectDescFlag::Retained;
        ++S.InventoryDataRevision;H->AutoCraft->NextRefresh=0;H->RefreshAutoCraft();
        TestTrue(TEXT("Inventory arrivals automatically clear shortage"),H->AutoCraft->Preview.Valid&&H->AutoCraft->InventorySummary.StartsWith(TEXT("Inventory ready:")));
        S.TradeSelfItems.Add(Pack.Guid);H->AutoCraft->NextRefresh=0;H->RefreshAutoCraft();
        TestFalse(TEXT("Offering a pack invalidates cached supplies without an inventory revision"),H->AutoCraft->Preview.Valid);
        S.TradeSelfItems.Reset();H->AutoCraft->NextRefresh=0;H->RefreshAutoCraft();
        TestTrue(TEXT("Removing trade offer restores availability"),H->AutoCraft->Preview.Valid);
        S.WorldObjects[10].ContainerId=S.PlayerGuid;S.WorldObjects.Remove(40);S.WorldObjects.Remove(41);
        H->RefreshAutoCraft(true);
    }
    auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("AutoCraft receiver"),false);
    auto Address=Sockets->CreateInternetAddr();bool Valid=false;Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
    if(!TestTrue(TEXT("Loopback receiver binds"),Receiver&&Receiver->Bind(*Address)))return false;Receiver->GetAddress(*Address);
    S.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("AutoCraft sender"),false);S.ServerC2SAddr=Address;S.IssacClient=MakeUnique<FACEIsaac>(123u);
    ON_SCOPE_EXIT{S.SocketC2S->Close();Sockets->DestroySocket(S.SocketC2S);S.SocketC2S=nullptr;S.ServerC2SAddr.Reset();S.IssacClient.Reset();Receiver->Close();Sockets->DestroySocket(Receiver);};
    auto SentUses=[&](){int32 Count=0;for(const auto& Packet:S.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()==ACEGameAction::UseWithTarget){++Count;TestEqual(TEXT("Packet applies arrowheads as source"),R.ReadUInt32(),10u);TestEqual(TEXT("Packet applies to arrowshafts"),R.ReadUInt32(),11u);}}return Count;};
    TestTrue(TEXT("Start succeeds"),H->StartAutoCraft());H->TickAutoCraft();TestTrue(TEXT("Craft is pending"),H->AutoCraft->Pending);TestEqual(TEXT("One craft was sent"),SentUses(),1);
    auto UseReceipt=[&](int32 From,int32 To,double Requested,uint32 Error,const TCHAR* Type=TEXT("use"))
    {
        auto E=MakeShared<FJsonObject>();E->SetStringField(TEXT("type"),Type);E->SetStringField(TEXT("stage"),TEXT("completed"));
        E->SetNumberField(TEXT("source"),uint32(From));E->SetNumberField(TEXT("target"),uint32(To));E->SetNumberField(TEXT("requested_at"),Requested);
        E->SetNumberField(TEXT("action_serial"),++H->ActionSerial);E->SetNumberField(TEXT("error"),Error);H->LastActionError=Error;H->EmitPluginAction(E);
    };
    UseReceipt(99,11,H->AutoCraft->RequestedAt,123);H->TickAutoCraft();TestTrue(TEXT("Unrelated source error cannot stop a craft"),Plugin->Running);
    UseReceipt(10,99,H->AutoCraft->RequestedAt,123);H->TickAutoCraft();TestTrue(TEXT("Same tool on another target cannot stop a craft"),Plugin->Running);
    UseReceipt(10,11,H->AutoCraft->RequestedAt-1,123);H->TickAutoCraft();TestTrue(TEXT("Older request on same ingredients cannot stop a craft"),Plugin->Running);
    UseReceipt(10,11,H->AutoCraft->RequestedAt,123,TEXT("cast"));H->TickAutoCraft();TestTrue(TEXT("Spell error cannot stop an item craft"),Plugin->Running);
    for(int32 N=0;N<20;++N)H->TickAutoCraft();TestEqual(TEXT("No duplicate crafts while awaiting network"),SentUses(),1);
    S.bUseBusy=false;H->ObserveUseDone(0);H->TickAutoCraft();TestEqual(TEXT("UseDone alone cannot increment progress"),H->AutoCraft->Completed,0);
    S.Confirmations.Add({5,42,TEXT("Chance of success")});H->TickAutoCraft();TestTrue(TEXT("Only job crafting confirmation accepted"),S.Confirmations.IsEmpty());
    S.WorldObjects[10].StackSize=1;S.WorldObjects[11].StackSize=1;
    H->TickAutoCraft();H->AutoCraft->ResultAt=FPlatformTime::Seconds()-2;H->TickAutoCraft();
    TestTrue(TEXT("Consumption packets do not prematurely end a delayed success"),H->AutoCraft->Pending);
    TestEqual(TEXT("No false failure from partial delivery"),H->AutoCraft->Failures,0);
    Item(12,Arrow.Output,100);
    H->TickAutoCraft();H->AutoCraft->ResultAt=FPlatformTime::Seconds()-1;H->TickAutoCraft();
    TestEqual(TEXT("Confirmed result advances exactly one batch"),H->AutoCraft->Completed,100);
    H->PauseAutoCraft();H->AutoCraft->NextAction=0;H->TickAutoCraft();TestEqual(TEXT("Pause emits no game action"),SentUses(),1);
    H->PauseAutoCraft();H->TickAutoCraft();TestEqual(TEXT("Resume sends next craft"),SentUses(),2);
    H->AutoCraft->Deadline=FPlatformTime::Seconds()-1;H->TickAutoCraft();TestFalse(TEXT("Unconfirmed action times out and stops"),Plugin->Running);
    TestEqual(TEXT("Timeout never blindly retries"),SentUses(),2);
    S.bUseBusy=false;H->SetEnabled(Plugin->Id,false);TestFalse(TEXT("Manager disable removes ability to start"),H->StartAutoCraft());
    H->SetEnabled(Plugin->Id,true);S.WorldObjects[10].ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;H->RefreshAutoCraft(true);
    TestFalse(TEXT("Retained ingredients block native execution"),H->StartAutoCraft());

    S.WorldObjects[10].ObjectDescriptionFlags&=~ACEObjectDescFlag::Retained;
    Plugin->Profile->SetNumberField(TEXT("quantity"),100);
    TestTrue(TEXT("New job can run after manual review of timeout"),H->StartAutoCraft());H->TickAutoCraft();
    S.bUseBusy=false;H->ObserveUseDone(0);S.WorldObjects.Remove(10);S.WorldObjects.Remove(11);
    H->ObserveMetaChat(Arrow.FailMessage,TEXT(""),0x18);H->TickAutoCraft();H->AutoCraft->ResultAt=FPlatformTime::Seconds()-1;H->TickAutoCraft();
    TestEqual(TEXT("Matching craft failure plus consumed inputs counts failure"),H->AutoCraft->Failures,1);
    H->Stop(Plugin->Id,TEXT("test reset"));

    Item(10,Arrow.Source,1);Item(11,Arrow.Target,1);
    TestTrue(TEXT("Rejection fixture starts"),H->StartAutoCraft());H->TickAutoCraft();
    UseReceipt(10,11,H->AutoCraft->RequestedAt,123);const auto BeforeReject=SentUses();H->TickAutoCraft();
    TestFalse(TEXT("Matching failed receipt stops only the current craft"),Plugin->Running);
    TestEqual(TEXT("Rejected craft is never retried"),SentUses(),BeforeReject);
    S.bUseBusy=false;S.WorldObjects.Remove(10);S.WorldObjects.Remove(11);

    // A same-type recipe must split a single stack before using two distinct IDs.
    const int32 PairRecipe=Catalog.Recipes.IndexOfByPredicate([](const FRecipe& R){return R.Source==R.Target&&R.SourceCount==1&&R.TargetCount==1&&!R.Skill;});
    if(TestTrue(TEXT("Same-type recipe exists"),PairRecipe!=INDEX_NONE))
    {
        const auto R=Catalog.Recipes[PairRecipe];Item(20,R.Source,4);Plugin->Profile->SetStringField(TEXT("recipe"),R.Id);Plugin->Profile->SetNumberField(TEXT("quantity"),R.Yield);
        TestTrue(TEXT("Same-stack job starts"),H->StartAutoCraft());H->TickAutoCraft();
        TestEqual(TEXT("Split precedes craft"),H->AutoCraft->Preparation,FString(TEXT("split")));
        TestFalse(TEXT("Stack preparation does not send a craft"),S.bUseBusy);
        const auto PacketCount=S.CachedC2SPackets.Num();H->TickAutoCraft();TestEqual(TEXT("Pending split is not repeated"),S.CachedC2SPackets.Num(),PacketCount);
        ++S.InventoryFailureSerial;S.InventoryFailureItem=999;H->TickAutoCraft();TestTrue(TEXT("Other inventory failure cannot stop ingredient split"),Plugin->Running);
        ++S.InventoryFailureSerial;S.InventoryFailureItem=0;H->TickAutoCraft();TestTrue(TEXT("Unscoped inventory failure cannot identify a split ingredient"),Plugin->Running);
        S.WorldObjects[20].StackSize=3;Item(21,R.Source,1);H->TickAutoCraft();H->AutoCraft->ResultAt=FPlatformTime::Seconds()-1;H->TickAutoCraft();
        TestFalse(TEXT("Both split receipts confirm preparation"),H->AutoCraft->Pending);
        H->AutoCraft->NextAction=0;H->TickAutoCraft();
        TestTrue(TEXT("Craft uses distinct operands after split"),H->AutoCraft->Pending&&H->AutoCraft->Preparation.IsEmpty()&&H->AutoCraft->SourceGuid!=H->AutoCraft->TargetGuid);
        H->Stop(Plugin->Id,TEXT("test reset"));S.bUseBusy=false;S.WorldObjects.Remove(20);S.WorldObjects.Remove(21);
    }
    // Large operand requirements combine fragmented supplies, with server ack.
    {
        auto R=Arrow;R.Id=TEXT("test-multistack");R.SourceCount=3;
        H->AutoCraft->Catalog.ById.Add(R.Id,H->AutoCraft->Catalog.Recipes.Num());H->AutoCraft->Catalog.Recipes.Add(R);
        Item(30,R.Source,R.SourceCount-1);Item(31,R.Source,1);Item(32,R.Target,FMath::Max(1,R.TargetCount));
        Plugin->Profile->SetStringField(TEXT("recipe"),R.Id);Plugin->Profile->SetNumberField(TEXT("quantity"),R.Yield);
        TestTrue(TEXT("Fragmented-stack job starts"),H->StartAutoCraft());H->TickAutoCraft();
        TestEqual(TEXT("Merge precedes craft"),H->AutoCraft->Preparation,FString(TEXT("merge")));
        const int32 Into=H->AutoCraft->TargetGuid,From=H->AutoCraft->SourceGuid;S.WorldObjects[Into].StackSize+=H->AutoCraft->PrepareAmount;S.WorldObjects.Remove(From);
        H->TickAutoCraft();H->AutoCraft->ResultAt=FPlatformTime::Seconds()-1;H->TickAutoCraft();TestFalse(TEXT("Merge completes after both receipts"),H->AutoCraft->Pending);
        H->Stop(Plugin->Id,TEXT("test reset"));S.WorldObjects.Remove(30);S.WorldObjects.Remove(31);S.WorldObjects.Remove(32);
        for(bool TargetFailure:{false,true})
        {
            Item(30,R.Source,2);Item(31,R.Source,1);Item(32,R.Target,FMath::Max(1,R.TargetCount));
            TestTrue(TEXT("Rejected merge fixture starts"),H->StartAutoCraft());H->TickAutoCraft();
            S.InventoryFailureItem=TargetFailure?H->AutoCraft->TargetGuid:H->AutoCraft->SourceGuid;++S.InventoryFailureSerial;
            H->TickAutoCraft();TestFalse(TEXT("Either ingredient failure stops pending merge"),Plugin->Running);
            S.WorldObjects.Remove(30);S.WorldObjects.Remove(31);S.WorldObjects.Remove(32);
        }
    }

    Item(10,Arrow.Source,2);Item(11,Arrow.Target,2);Plugin->Profile->SetStringField(TEXT("recipe"),Arrow.Id);Plugin->Profile->SetNumberField(TEXT("quantity"),100);
    S.WorldObjects[S.PlayerGuid].ItemsCapacity=3; // heads, shafts and existing output.
    TestTrue(TEXT("Full-pack job can be reviewed"),H->StartAutoCraft());H->TickAutoCraft();
    TestFalse(TEXT("Full packs stop before consuming ingredients"),Plugin->Running||S.bUseBusy);
    S.WorldObjects[S.PlayerGuid].ItemsCapacity=120;
    TestTrue(TEXT("Free space permits a new job"),H->StartAutoCraft());H->TickAutoCraft();
    S.Confirmations.Add({1,77,TEXT("Unrelated confirmation")});H->TickAutoCraft();
    TestFalse(TEXT("Unrelated confirmation stops job"),Plugin->Running);TestEqual(TEXT("Other prompts are never autoaccepted"),S.Confirmations.Num(),1);
    S.Confirmations.Reset();S.bUseBusy=false;
    TestTrue(TEXT("Clean job starts before lifecycle test"),H->StartAutoCraft());S.PlayerVitals.Health=0;H->TickAutoCraft();
    TestFalse(TEXT("Death stops before another craft"),Plugin->Running);S.PlayerVitals.Health=100;

    if(FApp::CanEverRender()&&FSlateApplication::IsInitialized())
    {
        Item(10,Arrow.Source,2);Item(11,Arrow.Target,2);Plugin->Profile->SetStringField(TEXT("category"),TEXT("All categories"));Plugin->Profile->SetStringField(TEXT("recipe"),Arrow.Id);
        Plugin->Profile->SetNumberField(TEXT("quantity"),200);H->RefreshAutoCraft(true);
        const FVector2D Size(600,850);auto Panel=H->MakeAutoCraftPanel();auto Window=SNew(SWindow).ClientSize(Size)[Panel];
        auto& Slate=FSlateApplication::Get();const auto OldFocus=Slate.GetUserFocusedWidget(0);Slate.AddWindow(Window,false);
        FWidgetRenderer Renderer(false,true);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
        ON_SCOPE_EXIT{Slate.DismissAllMenus();if(OldFocus)Slate.SetUserFocus(0,OldFocus);Window->RequestDestroyWindow();Target->ReleaseResource();};
        auto Frame=[&](){++GFrameCounter;Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();};Frame();Frame();
        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName,bool)> Find;
        Find=[&](TSharedRef<SWidget> Root,FName Name,bool Tag)->TSharedPtr<SWidget>
        {if(Tag?Root->GetTag()==Name:Root->GetType()==Name)return Root;auto* Children=Root->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Name,Tag))return Found;return nullptr;};
        auto Make=Find(Panel,TEXT("AutoCraft.Make"),true),Refresh=Find(Panel,TEXT("AutoCraft.InventoryRefresh"),true);
        auto Summary=StaticCastSharedPtr<STextBlock>(Find(Panel,TEXT("AutoCraft.InventorySummary"),true));
        auto Materials=StaticCastSharedPtr<STextBlock>(Find(Panel,TEXT("AutoCraft.Materials"),true));
        if(!TestTrue(TEXT("Inventory checklist and refresh control render"),Make&&Refresh&&Summary&&Materials))return false;
        TestTrue(TEXT("Make enabled when detected inventory is sufficient"),Make->IsEnabled());
        S.WorldObjects[10].StackSize=1;S.WorldObjects[11].ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;
        ++S.InventoryDataRevision;H->AutoCraft->NextRefresh=0;Frame();Frame();
        TestFalse(TEXT("Inventory loss disables Make on the existing widget"),Make->IsEnabled());
        TestTrue(TEXT("Rendered shortage summary names both missing inputs"),Summary->GetText().ToString().Contains(Catalog.Name(Arrow.Source))&&Summary->GetText().ToString().Contains(Catalog.Name(Arrow.Target)));
        TestTrue(TEXT("Rendered detail explains protected inventory"),Materials->GetText().ToString().Contains(TEXT("2 unavailable: retained")));
        {
            TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(int32(Size.X),int32(Size.Y),Pixels,Png);
            TestTrue(TEXT("Missing-inventory UI image saved"),FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("AutoCraftTests/inventory-missing.png"))));
        }
        S.WorldObjects[10].StackSize=2;S.WorldObjects[11].ObjectDescriptionFlags&=~ACEObjectDescFlag::Retained;
        // No revision bump: manually checking must force a fresh inventory scan.
        Slate.SetKeyboardFocus(Refresh);
        Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::SpaceBar,FModifierKeysState(),0,false,0,0));
        Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::SpaceBar,FModifierKeysState(),0,false,0,0));Frame();Frame();
        TestTrue(TEXT("Check inventory rescans and restores Make"),Make->IsEnabled()&&Summary->GetText().ToString().StartsWith(TEXT("Inventory ready:")));
        auto Picker=Find(Panel,TEXT("AutoCraft.Item"),true);if(!TestTrue(TEXT("Item selector renders"),Picker.IsValid()))return false;
        auto Combo=StaticCastSharedPtr<SComboButton>(Find(Picker.ToSharedRef(),TEXT("SACEPluginComboButton"),false));
        if(!TestTrue(TEXT("Craft picker uses the shared dropdown"),Combo.IsValid()))return false;
        Combo->SetIsOpen(true);Frame();auto Menu=Combo->GetMenuWindow();if(!TestTrue(TEXT("Search menu opens"),Menu.IsValid()))return false;
        auto Search=StaticCastSharedPtr<SEditableTextBox>(Find(Menu.ToSharedRef(),TEXT("AutoCraft.Search"),true));
        auto Matches=StaticCastSharedPtr<STextBlock>(Find(Menu.ToSharedRef(),TEXT("AutoCraft.Matches"),true));
        if(!TestTrue(TEXT("Search and count are rendered"),Search&&Matches))return false;
        Search->SetText(FText::FromString(TEXT("HEARTY carrot")));Frame();
        TestTrue(TEXT("Typing narrows catalog case-insensitively"),Matches->GetText().ToString().StartsWith(TEXT("7 matches")));
        ++S.InventoryDataRevision;H->RefreshAutoCraft(true);Frame();
        TestTrue(TEXT("Inventory refresh preserves open selector"),Combo->IsOpen()&&Find(Panel,TEXT("AutoCraft.Item"),true)==Picker);
        Search->SetText(FText::FromString(TEXT("no such recipe xyz")));Frame();TestEqual(TEXT("No match state"),Matches->GetText().ToString(),FString(TEXT("No matching items")));
        Search->SetText(FText::FromString(TEXT("HEARTY CARROT SOUP")));Frame();
        Slate.SetKeyboardFocus(Search);Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));Frame();
        TestFalse(TEXT("Enter chooses a search result and closes menu"),Combo->IsOpen());
        TestEqual(TEXT("Selected soup is the correct output"),H->AutoCraft->Catalog.Recipes[H->AutoCraft->Selected].Output,5822);
        Plugin->Profile->SetStringField(TEXT("recipe"),Arrow.Id);Plugin->Status=TEXT("Ready to craft");H->RefreshAutoCraft(true);Frame();
        TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(int32(Size.X),int32(Size.Y),Pixels,Png);
        TestTrue(TEXT("UI review image saved"),FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("AutoCraftTests/panel.png"))));
    }
    // UCM shares the executor, but owns its own one-batch job and failure domain.
    {
        auto UCM=H->Find(TEXT("ucm"));H->SetEnabled(UCM->Id,true);UCM->Running=true;UCM->Player=S.PlayerGuid;UCM->Server=C->GetServerName();
        UCM->Profile->SetBoolField(TEXT("auto_craft"),true);H->UCMServicesProfile.Reset();
        auto Supply=MakeShared<FJsonObject>();Supply->SetStringField(TEXT("name"),Catalog.Name(Arrow.Output));Supply->SetNumberField(TEXT("count"),100);
        UCM->Profile->SetArrayField(TEXT("craft_targets"),{MakeShared<FJsonValueObject>(Supply)});
        S.WorldObjects.Reset();S.WorldObjects.Add(Self.Guid,Self);S.Confirmations.Reset();S.bUseBusy=false;Item(10,Arrow.Source,3);Item(11,Arrow.Target,3);
        auto View=[&](){auto O=MakeShared<FJsonObject>();if(H->UCMCraft)H->UCMCraft->NextRefresh=0;H->ExtendUCMCraftSnapshot(O);return O->GetObjectField(TEXT("crafting"));};
        auto V=View();TestTrue(TEXT("Disabled standalone plugin is not required for UCM planning"),!V->GetArrayField(TEXT("candidates")).IsEmpty());
        auto Request=MakeShared<FJsonObject>();Request->SetStringField(TEXT("recipe"),Arrow.Id);
        // Inventory can change between the Lua snapshot and dispatch.
        Item(12,Arrow.Output,100);H->ExecuteUCMCraft(*UCM,Request);
        TestEqual(TEXT("Fresh stock cancels a stale recipe candidate"),H->UCMCraft->Selected,INDEX_NONE);
        S.WorldObjects.Remove(12);S.WorldObjects[10].ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;
        TestTrue(TEXT("Retained ingredients are excluded by UCM planner"),View()->GetArrayField(TEXT("candidates")).IsEmpty());S.WorldObjects[10].ObjectDescriptionFlags=0;
        S.TradeSelfItems.Add(11);TestTrue(TEXT("Offered ingredients cannot be crafted"),View()->GetArrayField(TEXT("candidates")).IsEmpty());S.TradeSelfItems.Reset();
        H->ExecuteUCMCraft(*UCM,Request);TestTrue(TEXT("UCM dispatches recipe using native executor"),H->UCMCraft->Pending);
        TestEqual(TEXT("One batch rather than entire stock target is scheduled"),H->UCMCraft->Goal,100);
        auto CraftPackets=[&](){int32 N=0;for(const auto& Packet:S.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()==ACEGameAction::UseWithTarget)++N;}return N;};
        const int32 Packets=CraftPackets();H->ExecuteUCMCraft(*UCM,Request);TestEqual(TEXT("Pending craft is not resent"),CraftPackets(),Packets);
        S.bUseBusy=false;UseReceipt(10,11,H->UCMCraft->RequestedAt,0);S.WorldObjects[10].StackSize--;S.WorldObjects[11].StackSize--;Item(12,Arrow.Output,100);
        H->ExecuteUCMCraft(*UCM,Request);H->UCMCraft->ResultAt=FPlatformTime::Seconds()-1;H->ExecuteUCMCraft(*UCM,Request);
        TestEqual(TEXT("Inventory confirmation finishes UCM batch"),H->UCMCraft->Selected,INDEX_NONE);TestTrue(TEXT("Craft completion leaves UCM running"),UCM->Running);
        TestTrue(TEXT("Met stock minimum has no further candidates"),View()->GetArrayField(TEXT("candidates")).IsEmpty());
        Supply->SetNumberField(TEXT("count"),200);TestEqual(TEXT("Partial stock uses idle priority"),View()->GetArrayField(TEXT("candidates"))[0]->AsObject()->GetStringField(TEXT("phase")),FString(TEXT("idle")));
        H->ExecuteUCMCraft(*UCM,Request);UseReceipt(10,11,H->UCMCraft->RequestedAt,123);H->ExecuteUCMCraft(*UCM,Request);
        TestEqual(TEXT("Rejected craft releases job"),H->UCMCraft->Selected,INDEX_NONE);TestTrue(TEXT("Server error leaves UCM enabled"),UCM->Running);
        TestTrue(TEXT("Failed supply cannot be retried through another recipe"),View()->GetArrayField(TEXT("candidates")).IsEmpty());
        TestTrue(TEXT("Craft failure is delivered to scheduler"),UCM->ActivityFailure.IsValid());
        S.bUseBusy=false;UCM->Profile->SetBoolField(TEXT("auto_craft"),false);auto Empty=MakeShared<FJsonObject>();H->ExtendUCMCraftSnapshot(Empty);
        TestFalse(TEXT("Disable releases crafting state"),H->UCMCraft.IsValid());H->Stop(UCM->Id,TEXT("Test complete"));
    }
    // Imported recipes are atomic, persistent and scoped to the observed server.
    {
        const FString CustomPath=H->StorageRoot/TEXT("custom-recipes.json");
        const FString Custom=TEXT(R"({"schema":1,"version":"fixture","items":[
          {"id":700001,"name":"Custom source","max_stack":100,"icon":0,"type":0},
          {"id":700002,"name":"Custom target","max_stack":100,"icon":0,"type":0},
          {"id":700003,"name":"Custom output","max_stack":100,"icon":0,"type":0}],
          "recipes":[{"id":"custom-fixture","category":"Custom","source":700001,"target":700002,"output":700003,"yield_count":2,"skill":0,"difficulty":0,
          "source_count":1,"target_count":1,"fail_source":0,"fail_target":0,"fail_output":0,"fail_yield":0,"chain":true}]})");
        TestTrue(TEXT("Custom catalog fixture saved"),FFileHelper::SaveStringToFile(Custom,*CustomPath));
        S.ServerName=TEXT("Recipe Server A");H->RefreshAutoCraft(true);
        TestTrue(TEXT("Server catalog imports"),H->ImportAutoCraftCatalog(CustomPath));
        TestTrue(TEXT("Custom recipe is in standalone selector"),H->AutoCraft->Catalog.ByName.Contains(TEXT("Custom output")));
        TestTrue(TEXT("Bundled recipes survive overlay"),H->AutoCraft->Catalog.ById.Contains(Arrow.Id));
        FACEAutoCraftState Shared;H->LoadAutoCraftCatalog(Shared);
        TestTrue(TEXT("UCM loads the same server catalog"),Shared.Catalog.ById.Contains(TEXT("custom-fixture")));
        FInventory CustomStock;CustomStock.Counts={{700001,2},{700002,3}};
        TestEqual(TEXT("Imported recipe plans with declared direction and yield"),Plan(Shared.Catalog,Shared.Catalog.ById[TEXT("custom-fixture")],CustomStock,3,false).Units,4);
        FFileHelper::SaveStringToFile(TEXT("{broken"),*CustomPath);
        TestFalse(TEXT("Invalid replacement is rejected"),H->ImportAutoCraftCatalog(CustomPath));
        H->AutoCraft.Reset();H->RefreshAutoCraft(true);
        TestTrue(TEXT("Rejected replacement keeps prior persisted recipe"),H->AutoCraft->Catalog.ById.Contains(TEXT("custom-fixture")));
        S.ServerName=TEXT("Recipe Server B");H->RefreshAutoCraft(true);
        TestFalse(TEXT("Other server never inherits custom recipe"),H->AutoCraft->Catalog.ById.Contains(TEXT("custom-fixture")));
        S.ServerName=TEXT("Recipe Server A");H->RefreshAutoCraft(true);
        TestTrue(TEXT("Returning to server restores custom recipe"),H->AutoCraft->Catalog.ById.Contains(TEXT("custom-fixture")));
        H->Find(TEXT("autocraft"))->Running=true;
        TestFalse(TEXT("Active crafting prevents catalog removal"),H->RemoveAutoCraftCatalog());
        H->Find(TEXT("autocraft"))->Running=false;
        TestTrue(TEXT("Server catalog can be removed"),H->RemoveAutoCraftCatalog());
        TestFalse(TEXT("Removing overlay restores bundled catalog"),H->AutoCraft->Catalog.ById.Contains(TEXT("custom-fixture")));
    }
    return true;
}
#endif
