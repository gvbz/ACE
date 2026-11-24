#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEItemTools.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Engine/GameInstance.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Protocol/ACEBinaryReader.h"
#include "Protocol/ACEIsaac.h"
#include "ACEOpcodes.h"

namespace
{
TSharedPtr<FJsonObject> ITJson(const TCHAR* Source)
{TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Source),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEItemToolsTest,"ACE.Plugins.ItemTools.Transactions",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEItemToolsTest::RunTest(const FString&)
{
    FString Source,Error;
    TestTrue(TEXT("Bundled policy exists"),FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/itemtools/main.lua"))));
    auto Snapshot=[](){return ITJson(TEXT(R"({"time":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"inventory":[{"id":10,"wcid":100,"name":"Gem","count":3,"transferable":true},{"id":11,"wcid":100,"name":"Gem","count":4,"transferable":true},{"id":12,"wcid":100,"name":"Gem","count":9,"transferable":false}],"it_objects":[{"id":20,"name":"Recipient","recipient":true,"player":true,"distance":2},{"id":30,"name":"Chest","chest":true,"distance":2,"identified":true,"locked":false}],"vendor":40,"it_currency":100,"vendor_uses_pyreals":true,"vendor_stock":[{"id":41,"name":"Scarab","wcid":200,"limit":1000,"room":100,"unit_value":10,"sell_rate":1}],"contents":[],"contents_ready":true})"));};
    auto Profile=[](){return ITJson(TEXT(R"({"mode":"give","give_name":"Gem","target_id":20,"target_name":"Recipient","delay_ms":0,"chest_id":30,"chest_name":"Chest","chest_cycles":1,"chest_rules":[{"name":"Gem","action":"keep"}]})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;Error.Empty();const bool OK=VM.Step(S,P,I,Error);TestTrue(*(FString(TEXT("Policy step: "))+Error),OK);return I?I:MakeShared<FJsonObject>();};
    auto Action=[](auto I){return ACEItemTools::String(I,TEXT("action"));};
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        P->SetStringField(TEXT("mode"),TEXT("shop"));P->SetStringField(TEXT("buy_name"),TEXT("Scarab"));P->SetNumberField(TEXT("buy_wcid"),200);
        P->SetNumberField(TEXT("buy_limit"),5);P->SetBoolField(TEXT("server_bank"),true);S->SetNumberField(TEXT("it_currency"),0);
        TestEqual(TEXT("Bank purchases send a normal server-validated buy without physical coins"),Action(Step(VM,S,P)),FString(TEXT("it_buy")));
        S->SetNumberField(TEXT("action_serial"),1);
        TestTrue(TEXT("Bank purchase requires actual item receipt"),Action(Step(VM,S,P)).IsEmpty());
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv.Add(MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"id":50,"wcid":200,"name":"Scarab","count":5,"transferable":true})"))));
        S->SetArrayField(TEXT("inventory"),Inv);
        auto Result=Step(VM,S,P);TestEqual(TEXT("Supplies confirm a bank purchase without a coin decrease"),Action(Result),FString(TEXT("stop")));
        TestTrue(TEXT("Bank purchase completes successfully"),ACEItemTools::String(Result,TEXT("status")).Contains(TEXT("Complete")));
    }
    {
        FACEPluginVM VM;TestTrue(TEXT("Item Tools policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();P->SetNumberField(TEXT("give_limit"),5);
        auto I=Step(VM,S,P);TestEqual(TEXT("Give takes first stack"),I->GetNumberField(TEXT("amount")),3.);
        S->SetNumberField(TEXT("action_serial"),1);TestFalse(TEXT("UseDone alone cannot confirm a transfer"),Step(VM,S,P)->HasField(TEXT("action")));
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv.RemoveAt(0);S->SetArrayField(TEXT("inventory"),Inv);
        I=Step(VM,S,P);TestEqual(TEXT("Next pack is included and quantity is capped"),I->GetNumberField(TEXT("amount")),2.);
        Inv[0]->AsObject()->SetNumberField(TEXT("count"),2);
        TestEqual(TEXT("Partial stack confirmation completes run"),Action(Step(VM,S,P)),FString(TEXT("stop")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Step(VM,S,P);S->SetNumberField(TEXT("time"),121);
        TestTrue(TEXT("Unconfirmed transfer stops without giving again"),ACEItemTools::String(Step(VM,S,P),TEXT("status")).Contains(TEXT("not confirmed")));
    }
    {
        // ACE quest NPCs consume one unit per give, regardless of offered amount.
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetNumberField(TEXT("give_limit"),5);
        S->GetArrayField(TEXT("it_objects"))[0]->AsObject()->SetBoolField(TEXT("player"),false);
        auto I=Step(VM,S,P);auto Inv=S->GetArrayField(TEXT("inventory"));
        for(int32 Given=1;Given<=5;++Given)
        {
            TestEqual(TEXT("Quest transfer is requested"),Action(I),FString(TEXT("it_give")));
            auto Item=Inv[0]->AsObject();const int32 Left=int32(Item->GetNumberField(TEXT("count")))-1;
            if(Left)Item->SetNumberField(TEXT("count"),Left);else Inv.RemoveAt(0);
            S->SetArrayField(TEXT("inventory"),Inv);I=Step(VM,S,P);
            TestEqual(TEXT("Counts only accepted quest units"),I->GetNumberField(TEXT("completed")),double(Given));
            if(Given==1)TestEqual(TEXT("Remaining units in same stack stay queued"),I->GetNumberField(TEXT("item")),10.);
            if(Given==3)TestEqual(TEXT("Continues into next pack stack"),I->GetNumberField(TEXT("item")),11.);
        }
        TestEqual(TEXT("Quest giving respects total limit"),Action(I),FString(TEXT("stop")));
        TestEqual(TEXT("Does not consume beyond limit"),Inv[0]->AsObject()->GetNumberField(TEXT("count")),2.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Step(VM,S,P);
        S->SetNumberField(TEXT("it_failure_serial"),1);S->SetNumberField(TEXT("it_failure_item"),99);
        TestFalse(TEXT("Unrelated item rejection cannot resend a pending gift"),Step(VM,S,P)->HasField(TEXT("action")));
        for(int32 Attempt=1;Attempt<=7;++Attempt)
        {
            S->SetNumberField(TEXT("it_failure_serial"),Attempt+1);S->SetNumberField(TEXT("it_failure_item"),10);
            auto I=Step(VM,S,P);
            TestEqual(TEXT("Rejections never count as gifts"),I->GetNumberField(TEXT("completed")),0.);
            if(Attempt==7)TestEqual(TEXT("Repeated rejections stop after six retries"),Action(I),FString(TEXT("stop")));
            else
            {
                TestFalse(TEXT("Explicit rejection backs off before retry"),I->HasField(TEXT("action")));
                TestFalse(TEXT("Same rejection cannot cause an immediate retry"),Step(VM,S,P)->HasField(TEXT("action")));
                S->SetNumberField(TEXT("time"),100+Attempt*4);I=Step(VM,S,P);
                TestEqual(TEXT("Retries explicitly rejected gift"),Action(I),FString(TEXT("it_give")));
                TestEqual(TEXT("Retries same queued stack"),I->GetNumberField(TEXT("item")),10.);
                TestFalse(TEXT("Old rejection cannot reject new attempt"),Step(VM,S,P)->HasField(TEXT("action")));
            }
        }
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Step(VM,S,P);
        S->SetNumberField(TEXT("it_failure_serial"),1);S->SetNumberField(TEXT("it_failure_item"),10);
        Step(VM,S,P);S->SetNumberField(TEXT("time"),104);
        S->GetArrayField(TEXT("it_objects"))[0]->AsObject()->SetNumberField(TEXT("distance"),8);
        TestEqual(TEXT("Recipient moving away cancels rejected-gift retry"),Action(Step(VM,S,P)),FString(TEXT("stop")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Step(VM,S,P);
        S->SetNumberField(TEXT("it_failure_serial"),1);S->SetNumberField(TEXT("it_failure_item"),10);S->SetNumberField(TEXT("it_failure_error"),42);
        TestTrue(TEXT("Known terminal inventory failure does not retry"),ACEItemTools::String(Step(VM,S,P),TEXT("status")).Contains(TEXT("Server rejected")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Step(VM,S,P);S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),42);
        TestTrue(TEXT("Server rejection is reported"),ACEItemTools::String(Step(VM,S,P),TEXT("status")).Contains(TEXT("Server rejected")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();S->GetArrayField(TEXT("it_objects"))[0]->AsObject()->SetNumberField(TEXT("distance"),8);
        TestEqual(TEXT("Moved recipient gets no items"),Action(Step(VM,S,P)),FString(TEXT("stop")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("mode"),TEXT("profile"));
        P->SetArrayField(TEXT("profile_rules"),{MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"name":"Gem","action":"keep","keep_up_to":10})")))});
        TestEqual(TEXT("GiveVT keeps Keep # items in inventory"),Action(Step(VM,S,P)),FString(TEXT("stop")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("mode"),TEXT("profile"));
        P->SetArrayField(TEXT("profile_rules"),{MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"name":"Gem","action":"keep","conditions":[{"field":"int","key":19,"op":"ge","value":100}]})")))});
        TestEqual(TEXT("Missing properties request appraisal"),Action(Step(VM,S,P)),FString(TEXT("identify")));
        auto Item=S->GetArrayField(TEXT("inventory"))[0]->AsObject();Item->SetBoolField(TEXT("identified"),true);Item->SetObjectField(TEXT("int_properties"),ITJson(TEXT("{\"19\":100}")));
        TestEqual(TEXT("Appraisal resumes same rule"),Action(Step(VM,S,P)),FString(TEXT("it_give")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("mode"),TEXT("shop"));P->SetStringField(TEXT("buy_name"),TEXT("Scarab"));
        auto I=Step(VM,S,P);TestEqual(TEXT("Unlimited buying uses affordable quantity"),I->GetNumberField(TEXT("amount")),10.);
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv.Add(MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"id":50,"name":"Scarab","wcid":200,"count":10,"transferable":true})"))));S->SetArrayField(TEXT("inventory"),Inv);S->SetNumberField(TEXT("it_currency"),0);
        TestTrue(TEXT("Stops when funds exhausted"),ACEItemTools::String(Step(VM,S,P),TEXT("status")).Contains(TEXT("Insufficient currency")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("mode"),TEXT("restock"));P->SetNumberField(TEXT("vendor_id"),40);
        P->SetArrayField(TEXT("restock"),{MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"name":"Scarab","wcid":200,"quantity":25})")))});
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv.Add(MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"id":60,"name":"Trade Note","wcid":300,"count":5,"transferable":true,"can_sell":true,"trade_note":true,"sell_unit":100})"))));S->SetArrayField(TEXT("inventory"),Inv);
        auto I=Step(VM,S,P);TestEqual(TEXT("Restock splits the minimum funding notes"),Action(I),FString(TEXT("it_split")));TestEqual(TEXT("Only required note count"),I->GetNumberField(TEXT("amount")),2.);
        Inv.Last()->AsObject()->SetNumberField(TEXT("count"),3);Inv.Add(MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"id":61,"name":"Trade Note","wcid":300,"count":2,"transferable":true,"can_sell":true,"trade_note":true,"sell_unit":100})"))));S->SetArrayField(TEXT("inventory"),Inv);
        I=Step(VM,S,P);TestEqual(TEXT("Sells only the confirmed split"),I->GetNumberField(TEXT("item")),61.);
        Inv.Pop();S->SetArrayField(TEXT("inventory"),Inv);
        TestFalse(TEXT("Waits for funding currency before selling another note"),Step(VM,S,P)->HasField(TEXT("action")));S->SetNumberField(TEXT("it_currency"),300);
        I=Step(VM,S,P);TestEqual(TEXT("Buys after confirmed note redemption"),Action(I),FString(TEXT("it_buy")));TestEqual(TEXT("Buys desired shortage"),I->GetNumberField(TEXT("amount")),25.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("mode"),TEXT("chest"));
        TestEqual(TEXT("Chest opens before looting"),Action(Step(VM,S,P)),FString(TEXT("it_open")));
        S->SetNumberField(TEXT("container"),30);S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"id":70,"name":"Gem","wcid":100,"count":2})")))});
        TestEqual(TEXT("Chest profile collects Keep item"),Action(Step(VM,S,P)),FString(TEXT("it_pickup")));
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("count"),5);S->SetArrayField(TEXT("contents"),{});
        TestEqual(TEXT("Confirmed merge closes chest"),Action(Step(VM,S,P)),FString(TEXT("it_close")));
        S->SetNumberField(TEXT("container"),0);P->SetNumberField(TEXT("mule1_id"),20);P->SetStringField(TEXT("mule1_name"),TEXT("Recipient"));P->SetArrayField(TEXT("mule1_rules"),P->GetArrayField(TEXT("chest_rules")));
        auto I=Step(VM,S,P);TestEqual(TEXT("Mule gets only newly acquired units"),I->GetNumberField(TEXT("amount")),2.);
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("count"),3);
        TestTrue(TEXT("One chest cycle completes"),ACEItemTools::String(Step(VM,S,P),TEXT("status")).Contains(TEXT("1 chest cycles")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("mode"),TEXT("chest"));P->SetStringField(TEXT("key_name"),TEXT("Gem"));P->SetArrayField(TEXT("chest_rules"),{});
        S->GetArrayField(TEXT("it_objects"))[1]->AsObject()->SetBoolField(TEXT("locked"),true);
        TestEqual(TEXT("Opener-only uses selected key"),Action(Step(VM,S,P)),FString(TEXT("it_key")));
        S->SetNumberField(TEXT("action_serial"),1);
        TestTrue(TEXT("Opener-only completes without taking loot"),ACEItemTools::String(Step(VM,S,P),TEXT("status")).Contains(TEXT("unlocking complete")));
    }
    // Exercise native ownership/range/capacity checks and retail packet layout
    // using a loopback receiver; no test contacts a game server.
    {
        auto* GI=NewObject<UGameInstance>();GI->Init();ON_SCOPE_EXIT{GI->Shutdown();};
        auto* C=GI->GetSubsystem<UACEClientSubsystem>();auto* H=GI->GetSubsystem<UACEPluginSubsystem>();
        H->StorageRoot=FPaths::ProjectSavedDir()/TEXT("ItemToolsReview")/FGuid::NewGuid().ToString(EGuidFormats::Digits);H->Settings=MakeShared<FJsonObject>();
        C->Session=MakeShared<FACESession>();auto& S=*C->Session;S.State=EACESessionState::InWorld;S.PlayerGuid=0x50000001;
        S.PlayerVitals.bValid=true;S.PlayerVitals.Health=100;S.PlayerPosition.CellId=0x7D640001;
        auto P=H->Find(TEXT("itemtools"));if(!TestTrue(TEXT("Native plugin exists"),P.IsValid()))return false;
        P->Enabled=true;P->Running=false;
        H->UpdateItemToolsCharacter();const FString FirstCharacter=P->ProfileName;
        P->Profile->SetNumberField(TEXT("give_limit"),17);P->Profile->SetNumberField(TEXT("target_id"),20);P->Profile->SetStringField(TEXT("target_name"),TEXT("Recipient"));
        TestTrue(TEXT("Character configuration saves"),H->SaveProfile(P->Id,P->ProfileName,H->ProfileJson(P->Id),false));
        S.PlayerGuid++;H->UpdateItemToolsCharacter();TestEqual(TEXT("Another character gets own configuration"),ACEItemTools::Number(P->Profile,TEXT("give_limit")),0.);
        S.PlayerGuid--;H->UpdateItemToolsCharacter();TestEqual(TEXT("Original character quantity is restored"),ACEItemTools::Number(P->Profile,TEXT("give_limit")),17.);
        TestEqual(TEXT("Character profile identity persists"),P->ProfileName,FirstCharacter);
        TestFalse(TEXT("Old recipient GUID is not restored across logins"),P->Profile->HasField(TEXT("target_id")));
        TestEqual(TEXT("Recipient name remains visible"),ACEItemTools::String(P->Profile,TEXT("target_name")),FString(TEXT("Recipient")));
        TestFalse(TEXT("Malformed restock data is rejected"),H->SaveProfile(P->Id,P->ProfileName,TEXT("{\"restock\":[4]}"),false));
        FACEWorldObject Self;Self.Guid=S.PlayerGuid;Self.ItemsCapacity=1;Self.ContainersCapacity=7;S.WorldObjects.Add(Self.Guid,Self);
        FACEWorldObject Item;Item.Guid=10;Item.WeenieClassId=100;Item.Name=TEXT("Gem");Item.ContainerId=Self.Guid;Item.StackSize=3;S.WorldObjects.Add(Item.Guid,Item);
        FACEWorldObject Recipient;Recipient.Guid=20;Recipient.Name=TEXT("Recipient");Recipient.ItemType=ACEItemType::Creature;Recipient.bHasPosition=true;Recipient.bIsPlayer=true;Recipient.Position=S.PlayerPosition;S.WorldObjects.Add(Recipient.Guid,Recipient);
        auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("ItemTools tests"),false);
        auto Address=Sockets->CreateInternetAddr();bool Valid=false;Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
        if(!TestTrue(TEXT("Loopback receiver binds"),Receiver&&Receiver->Bind(*Address)))return false;Receiver->GetAddress(*Address);
        S.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("ItemTools sender"),false);S.ServerC2SAddr=Address;S.IssacClient=MakeUnique<FACEIsaac>(123u);
        ON_SCOPE_EXIT{S.SocketC2S->Close();Sockets->DestroySocket(S.SocketC2S);S.SocketC2S=nullptr;S.ServerC2SAddr.Reset();S.IssacClient.Reset();Receiver->Close();Sockets->DestroySocket(Receiver);};
        auto CountAction=[&](uint32 Opcode){int32 Count=0;for(const auto& Packet:S.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()==Opcode)++Count;}return Count;};
        {
            auto Target=Item;Target.Guid=19;Target.MaxStackSize=100;Target.StackSize=90;
            S.WorldObjects.Add(Target.Guid,Target);S.WorldObjects[10].MaxStackSize=100;
            auto Merge=ITJson(TEXT(R"({"action":"it_merge","item":10,"target":19,"amount":3})"));
            H->ExecuteItemTools(*P,Merge);
            TestEqual(TEXT("Bulk merge uses the retail StackableMerge action"),CountAction(ACEGameAction::StackableMerge),1);
            TestEqual(TEXT("Bulk merge does not predict the resulting count"),S.WorldObjects[19].StackSize,90);
            FACEBinaryWriter Receipt;Receipt.WriteUInt32(ACEOpcode::SetStackSize);Receipt.WriteUInt8(1);
            Receipt.WriteInt32(19);Receipt.WriteInt32(99);Receipt.WriteInt32(0);
            S.HandleGameMessage(Receipt.GetData());
            S.CachedC2SPackets.Reset();H->ExecuteItemTools(*P,Merge);
            TestEqual(TEXT("Stale capacity prevents a bulk merge"),CountAction(ACEGameAction::StackableMerge),0);
            S.WorldObjects[19].StackSize=90;S.WorldObjects[19].WeenieClassId=101;H->ExecuteItemTools(*P,Merge);
            TestEqual(TEXT("Same name with different WCID cannot merge"),CountAction(ACEGameAction::StackableMerge),0);
            S.WorldObjects[19].WeenieClassId=100;S.TradeSelfItems.Add(19);H->ExecuteItemTools(*P,Merge);
            TestEqual(TEXT("Offered target cannot merge"),CountAction(ACEGameAction::StackableMerge),0);S.TradeSelfItems.Reset();
            S.WorldObjects[19].ObjectDescriptionFlags=ACEObjectDescFlag::Retained;H->ExecuteItemTools(*P,Merge);
            TestEqual(TEXT("Retained target cannot be merged by Item Tools"),CountAction(ACEGameAction::StackableMerge),0);
            S.WorldObjects[19].ObjectDescriptionFlags=0;S.WorldObjects[19].WielderId=S.PlayerGuid;H->ExecuteItemTools(*P,Merge);
            TestEqual(TEXT("Equipped target cannot be merged by Item Tools"),CountAction(ACEGameAction::StackableMerge),0);
            S.WorldObjects[19].WielderId=0;S.WorldObjects[19].ContainerId=999;H->ExecuteItemTools(*P,Merge);
            TestEqual(TEXT("Unowned target cannot be merged by Item Tools"),CountAction(ACEGameAction::StackableMerge),0);
            S.WorldObjects.Remove(19);S.WorldObjects[10].MaxStackSize=Item.MaxStackSize;++S.InventoryDataRevision;
        }
        {
            const auto SavedObjects=S.WorldObjects;
            ON_SCOPE_EXIT
            {
                S.WorldObjects=SavedObjects;++S.InventoryDataRevision;S.CachedC2SPackets.Reset();
                P->Profile->SetStringField(TEXT("mode"),TEXT("give"));
            };
            S.WorldObjects[10].MaxStackSize=100;
            auto Target=S.WorldObjects[10];Target.Guid=19;Target.StackSize=90;S.WorldObjects.Add(Target.Guid,Target);
            auto SourceStack=Target;SourceStack.Guid=21;SourceStack.StackSize=5;S.WorldObjects.Add(SourceStack.Guid,SourceStack);
            S.CachedC2SPackets.Reset();
            if(!TestTrue(TEXT("Merge starts through the Item Tools entry point"),H->StartItemTools(TEXT("merge"))))return false;
            auto Wake=[&]()
            {
                FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);
                H->RunDecisionWake(0.f);
            };
            TestTrue(TEXT("Starting merge schedules work immediately"),H->DecisionWakeTicker.IsValid());
            Wake();
            TestEqual(TEXT("Initial merge sends a single request"),CountAction(ACEGameAction::StackableMerge),1);
            FACEBinaryWriter TargetUpdate;
            TargetUpdate.WriteUInt32(ACEOpcode::SetStackSize);TargetUpdate.WriteUInt8(1);
            TargetUpdate.WriteUInt32(19);TargetUpdate.WriteUInt32(93);TargetUpdate.WriteUInt32(0);
            S.HandleGameMessage(TargetUpdate.GetData());
            TestTrue(TEXT("Server stack update wakes Item Tools before its next poll"),H->DecisionWakeTicker.IsValid()&&P->NextDecision==0.);
            Wake();
            TestEqual(TEXT("Target receipt alone still holds the next merge"),CountAction(ACEGameAction::StackableMerge),1);
            FACEBinaryWriter Removed;
            Removed.WriteUInt32(ACEOpcode::InventoryRemoveObject);Removed.WriteUInt32(10);
            S.HandleGameMessage(Removed.GetData());
            TestTrue(TEXT("Source removal schedules another immediate check"),H->DecisionWakeTicker.IsValid()&&P->NextDecision==0.);
            Wake();
            TestEqual(TEXT("Both receipts dispatch the next merge on the wake"),CountAction(ACEGameAction::StackableMerge),2);
            H->Stop(P->Id);
        }
        auto Give=ITJson(TEXT(R"({"action":"it_give","item":10,"target":20,"amount":2})"));
        H->ExecuteItemTools(*P,Give);TestEqual(TEXT("Native giving uses retail GiveObject"),CountAction(ACEGameAction::GiveObjectRequest),1);
        S.CachedC2SPackets.Reset();S.WorldObjects[10].Bonded=1;H->ExecuteItemTools(*P,Give);
        TestEqual(TEXT("Bonded items remain transferable to players in retail"),CountAction(ACEGameAction::GiveObjectRequest),1);
        S.CachedC2SPackets.Reset();S.WorldObjects[10].Attuned=1;H->ExecuteItemTools(*P,Give);
        TestEqual(TEXT("Attuned items cannot be given to players"),CountAction(ACEGameAction::GiveObjectRequest),0);
        S.WorldObjects[20].bIsPlayer=false;H->ExecuteItemTools(*P,Give);
        TestEqual(TEXT("NPC turn-ins may use attuned quest items"),CountAction(ACEGameAction::GiveObjectRequest),1);
        S.WorldObjects[10].Attuned=0;
        {
            // Real server packet handlers -> native snapshot -> Lua. The busy
            // reply contains no UseDone and must not mutate inventory.
            FACEPluginVM VM;VM.Load(Source,Error);auto Settings=Profile();Settings->SetNumberField(TEXT("give_limit"),3);
            auto Snap=[&](double Time){auto J=H->ItemToolsSnapshot();J->SetNumberField(TEXT("time"),Time);J->SetBoolField(TEXT("ready"),true);return J;};
            auto I=Step(VM,Snap(100),Settings);TestEqual(TEXT("Native snapshot starts NPC give"),Action(I),FString(TEXT("it_give")));
            FACEBinaryWriter Failure;Failure.WriteUInt32(10);Failure.WriteUInt32(0);FACEBinaryReader Failed(Failure.GetData());S.HandleInventoryServerSaveFailed(Failed);
            TestEqual(TEXT("Rejected gift preserves inventory stack"),S.WorldObjects[10].StackSize,3);
            I=Step(VM,Snap(100),Settings);TestTrue(TEXT("Real inventory rejection schedules retry"),ACEItemTools::String(I,TEXT("status")).Contains(TEXT("retry")));
            I=Step(VM,Snap(101),Settings);TestEqual(TEXT("Real rejection resumes same gift"),Action(I),FString(TEXT("it_give")));
            for(int32 Left=2;Left>=0;--Left)
            {
                if(Left)
                {
                    FACEBinaryWriter Count;Count.WriteUInt8(uint8(3-Left));Count.WriteUInt32(10);Count.WriteUInt32(Left);Count.WriteUInt32(0);
                    FACEBinaryReader Update(Count.GetData());S.HandleSetStackSize(Update);
                }
                else
                {
                    FACEBinaryWriter Remove;Remove.WriteUInt32(10);FACEBinaryReader Update(Remove.GetData());S.HandleInventoryRemoveObject(Update);
                }
                I=Step(VM,Snap(102+3-Left),Settings);
                TestEqual(TEXT("Replicated inventory change confirms actual units"),I->GetNumberField(TEXT("completed")),double(3-Left));
                TestEqual(TEXT("Partial stack continues until exhausted"),Action(I),FString(Left?TEXT("it_give"):TEXT("stop")));
            }
            S.WorldObjects.Add(Item.Guid,Item);
        }
        for(const TCHAR* SalvageName:{TEXT("Steel Salvage (100)"),TEXT("Green Garnet Salvage (100)")})
        {
            const auto SavedObjects=S.WorldObjects;
            S.WorldObjects[20].bIsPlayer=true;
            FACEWorldObject Bag;Bag.Guid=90;Bag.ContainerId=Self.Guid;Bag.ItemType=ACEItemType::Container;Bag.ItemsCapacity=24;S.WorldObjects.Add(Bag.Guid,Bag);
            for(int N=0;N<14;++N)
            {
                FACEWorldObject Salvage;Salvage.Guid=100+N;Salvage.ContainerId=N<8?Self.Guid:Bag.Guid;
                Salvage.Name=SalvageName;Salvage.ItemType=0x40000000;
                Salvage.StackSize=1;Salvage.Bonded=N<8?0:1;S.WorldObjects.Add(Salvage.Guid,Salvage);
            }
            ++S.InventoryDataRevision;
            FACEPluginVM VM;VM.Load(Source,Error);auto Settings=Profile();Settings->SetStringField(TEXT("give_name"),SalvageName);
            Settings->SetNumberField(TEXT("give_limit"),0);Settings->SetNumberField(TEXT("delay_ms"),250);
            auto Snap=[&](double Time){auto J=H->ItemToolsSnapshot();J->SetNumberField(TEXT("time"),Time);J->SetBoolField(TEXT("ready"),true);return J;};
            for(int N=0;N<14;++N)
            {
                auto I=Step(VM,Snap(200+N*0.25),Settings);
                if(!TestEqual(TEXT("All fourteen salvage bags across packs are offered"),Action(I),FString(TEXT("it_give"))))break;
                TestEqual(TEXT("Each salvage bag counts as one item"),I->GetNumberField(TEXT("amount")),1.);
                FACEBinaryWriter Remove;Remove.WriteUInt32(int32(I->GetNumberField(TEXT("item"))));FACEBinaryReader Update(Remove.GetData());S.HandleInventoryRemoveObject(Update);
                TestFalse(TEXT("Acknowledging a transfer starts the configured pause"),Step(VM,Snap(200+N*0.25),Settings)->HasField(TEXT("action")));
                if(N<13)TestFalse(TEXT("250 ms delay prevents an early second transfer"),Step(VM,Snap(200+N*0.25+0.125),Settings)->HasField(TEXT("action")));
            }
            auto I=Step(VM,Snap(215),Settings);
            TestEqual(TEXT("Give-all finishes after all fourteen confirmations"),I->GetNumberField(TEXT("completed")),14.);
            TestEqual(TEXT("Give-all stops cleanly"),Action(I),FString(TEXT("stop")));
            S.WorldObjects=SavedObjects;++S.InventoryDataRevision;
        }
        {
            // A real Pyreal update must unblock the Lua note-redemption state.
            FACEWorldObject Coins;Coins.Guid=95;Coins.ContainerId=Self.Guid;Coins.ItemType=ACEItemType::Money;
            Coins.StackSize=100;Coins.MaxStackSize=25000;S.UpsertWorldObject(Coins);
            S.VendorCurrencyWeenie=0;S.VendorCurrencyCount=0;
            FACEPluginVM VM;VM.Load(Source,Error);auto J=Snapshot(),Settings=Profile();
            Settings->SetStringField(TEXT("mode"),TEXT("restock"));Settings->SetNumberField(TEXT("vendor_id"),40);
            Settings->SetArrayField(TEXT("restock"),{MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"name":"Scarab","wcid":200,"quantity":25})")))});
            auto Inv=J->GetArrayField(TEXT("inventory"));
            Inv.Add(MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"id":96,"name":"Trade Note","count":1,"transferable":true,"can_sell":true,"trade_note":true,"sell_unit":200})"))));
            J->SetArrayField(TEXT("inventory"),Inv);J->SetNumberField(TEXT("it_currency"),S.GetVendorCurrencyCount());
            TestEqual(TEXT("Ordinary vendor counts live owned Pyreals"),S.GetVendorCurrencyCount(),100);
            TestEqual(TEXT("Restock redeems a note for the shortage"),Action(Step(VM,J,Settings)),FString(TEXT("it_sell")));
            Inv.Pop();J->SetArrayField(TEXT("inventory"),Inv);
            TestFalse(TEXT("Note removal alone does not confirm its proceeds"),Step(VM,J,Settings)->HasField(TEXT("action")));
            FACEBinaryWriter Count;Count.WriteUInt8(1);Count.WriteUInt32(Coins.Guid);Count.WriteUInt32(300);Count.WriteUInt32(300);
            FACEBinaryReader Update(Count.GetData());S.HandleSetStackSize(Update);
            J->SetNumberField(TEXT("it_currency"),H->ItemToolsSnapshot()->GetNumberField(TEXT("it_currency")));
            TestEqual(TEXT("Native currency acknowledgement advances restock to buying"),Action(Step(VM,J,Settings)),FString(TEXT("it_buy")));
            S.WorldObjects.Remove(Coins.Guid);++S.InventoryDataRevision;
        }
        S.CachedC2SPackets.Reset();S.WorldObjects[10].Bonded=0;S.WorldObjects[10].ObjectDescriptionFlags=ACEObjectDescFlag::Retained;H->ExecuteItemTools(*P,Give);
        TestEqual(TEXT("Retained items are rechecked before sending"),CountAction(ACEGameAction::GiveObjectRequest),0);S.WorldObjects[10].ObjectDescriptionFlags=0;
        FACEWorldObject Pack;Pack.Guid=50;Pack.ContainerId=Self.Guid;Pack.ItemType=ACEItemType::Container;Pack.ItemsCapacity=1;S.WorldObjects.Add(Pack.Guid,Pack);
        TestTrue(TEXT("Empty pack can be transferred"),ACEItemTools::Transferable(C,Pack));
        FACEWorldObject Stock;Stock.Guid=80;Stock.WeenieClassId=200;Stock.MaxStackSize=100;Stock.VendorQuantityAvailable=-1;
        TestEqual(TEXT("Purchase capacity includes the empty side pack"),ACEItemTools::PurchaseRoom(C,Stock),100);
        Stock.Name=TEXT("Scarab");Stock.Value=10;S.OpenVendorGuid=40;S.VendorMerchandise={Stock};S.VendorSellRate=1;
        auto Buy=ITJson(TEXT(R"({"action":"it_buy","vendor":40,"item":80,"amount":5})"));
        S.CachedC2SPackets.Reset();H->ExecuteItemTools(*P,Buy);
        TestEqual(TEXT("Ordinary purchases still require physical currency"),CountAction(ACEGameAction::Buy),0);
        P->Profile->SetBoolField(TEXT("server_bank"),true);H->ExecuteItemTools(*P,Buy);
        TestEqual(TEXT("Server bank opt-in reaches native Buy without inventing coins"),CountAction(ACEGameAction::Buy),1);
        P->Profile->SetBoolField(TEXT("server_bank"),false);S.CachedC2SPackets.Reset();
        Stock.ItemType=ACEItemType::Container;Stock.MaxStackSize=1;
        TestEqual(TEXT("Backpack purchases use pack slots"),ACEItemTools::PurchaseRoom(C,Stock),1);
        S.WorldObjects[Self.Guid].ContainersCapacity=1;
        TestEqual(TEXT("Full pack slots block backpack purchases"),ACEItemTools::PurchaseRoom(C,Stock),0);
        FACEWorldObject Chest;Chest.Guid=30;Chest.bHasPosition=true;Chest.Position=S.PlayerPosition;Chest.ItemType=ACEItemType::Container;Chest.ItemUseable=0x20;Chest.ObjectDescriptionFlags=ACEObjectDescFlag::Openable;S.WorldObjects.Add(Chest.Guid,Chest);
        FACEWorldObject Drop;Drop.Guid=31;Drop.ContainerId=30;Drop.Name=TEXT("Loot");Drop.StackSize=1;S.WorldObjects.Add(Drop.Guid,Drop);S.OpenExternalContainerGuid=30;
        auto Pickup=ITJson(TEXT(R"({"action":"it_pickup","item":31,"target":30,"amount":1})"));H->ExecuteItemTools(*P,Pickup);
        TestEqual(TEXT("Full main pack still permits chest pickup"),CountAction(ACEGameAction::PutItemInContainer),1);
        for(const auto& Packet:S.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()==ACEGameAction::PutItemInContainer){R.ReadUInt32();TestEqual(TEXT("Loot goes to secondary pack"),R.ReadUInt32(),50u);}}
        S.CachedC2SPackets.Reset();Item.Guid=51;Item.ContainerId=50;S.WorldObjects.Add(Item.Guid,Item);H->ExecuteItemTools(*P,Pickup);
        TestEqual(TEXT("All packs full prevents transfer"),CountAction(ACEGameAction::PutItemInContainer),0);
        TestFalse(TEXT("Nonempty pack cannot be transferred"),ACEItemTools::Transferable(C,Pack));
        auto UCM=H->Find(TEXT("ucm"));UCM->Running=true;UCM->Player=S.PlayerGuid;UCM->Server=C->GetServerName();P->Running=true;
        H->Stop(P->Id,TEXT("Complete"));TestTrue(TEXT("UCM remains enabled after tool finishes"),UCM->Running);TestTrue(TEXT("UCM resets stale pending work on resume"),UCM->ResumeMetaPending);
        P->Profile->SetBoolField(TEXT("auto_restock"),true);
        P->Profile->SetArrayField(TEXT("restock"),{MakeShared<FJsonValueObject>(ITJson(TEXT(R"({"name":"Scarab","wcid":200,"quantity":25})")))});
        Stock.Name=TEXT("Scarab");Stock.ItemType=0;S.OpenVendorGuid=40;S.VendorMerchandise={Stock};H->ItemToolsLastVendor=0;
        H->UpdateItemToolsCharacter();TestTrue(TEXT("Matching vendor starts opted-in restocking"),P->Running);
        TestEqual(TEXT("Autobuy starts restock activity"),ACEItemTools::String(P->Profile,TEXT("mode")),FString(TEXT("restock")));
        TestTrue(TEXT("Starting the tool preserves UCM run state"),UCM->Running);H->Stop(P->Id);
        H->UpdateItemToolsCharacter();TestFalse(TEXT("Same open vendor cannot restart purchases in a loop"),P->Running);
        P->Profile->SetBoolField(TEXT("auto_restock"),false);H->ItemToolsLastVendor=0;H->UpdateItemToolsCharacter();
        TestFalse(TEXT("Vendor opening never starts purchases without opt-in"),P->Running);
        UCM->Running=false;
    }
    {
        // Worst-case nonmatching name filters must yield and eventually finish,
        // rather than repeatedly restarting at the first inventory item.
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("mode"),TEXT("profile"));
        TArray<TSharedPtr<FJsonValue>> Rules,Inventory;
        for(int32 N=0;N<1000;++N){auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("name"),FString::Printf(TEXT("No match %d"),N));R->SetStringField(TEXT("action"),TEXT("keep"));Rules.Add(MakeShared<FJsonValueObject>(R));}
        for(int32 N=0;N<128;++N){auto R=ITJson(TEXT(R"({"name":"Gem","count":1,"transferable":true})"));R->SetNumberField(TEXT("id"),100+N);Inventory.Add(MakeShared<FJsonValueObject>(R));}
        P->SetArrayField(TEXT("profile_rules"),Rules);S->SetArrayField(TEXT("inventory"),Inventory);
        bool Complete=false;int32 Calls=0;
        for(;Calls<160&&!Complete;++Calls){S->SetNumberField(TEXT("time"),100+Calls*.25);Complete=Action(Step(VM,S,P))==TEXT("stop");}
        TestTrue(TEXT("128 items x 1000 rules finish cooperatively"),Complete);
        TestTrue(TEXT("Large rule processing stays under sandbox memory cap"),VM.GetPeakMemoryUsage()<=FACEPluginVM::MemoryLimit);
        AddInfo(FString::Printf(TEXT("Large profile scan: %d callbacks; peak %.2f MiB"),Calls,VM.GetPeakMemoryUsage()/1048576.));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEItemToolsMergeTest,"ACE.Plugins.ItemTools.Merge",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEItemToolsMergeTest::RunTest(const FString&)
{
    FString Source,Error;
    FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/itemtools/main.lua")));
    auto Snapshot=[](){return ITJson(TEXT(R"({"time":100,"ready":true,"inventory":[{"id":11,"wcid":100,"name":"Gem","count":60,"max_stack":100,"container":1,"transferable":true},{"id":12,"wcid":100,"name":"Gem","count":70,"max_stack":100,"container":2,"transferable":true},{"id":13,"wcid":100,"name":"Renamed gem","count":50,"max_stack":100,"container":3,"transferable":true},{"id":14,"wcid":100,"name":"Gem","count":100,"max_stack":100,"transferable":true},{"id":15,"wcid":101,"name":"Gem","count":12,"max_stack":100,"transferable":true},{"id":16,"wcid":100,"name":"Gem","count":80,"max_stack":100,"transferable":false},{"id":17,"wcid":100,"name":"Gem","count":1,"max_stack":1,"transferable":true}]})"));};
    auto P=ITJson(TEXT(R"({"mode":"merge","delay_ms":250})"));
    auto Step=[&](FACEPluginVM& VM,auto S){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto Action=[](auto I){return ACEItemTools::String(I,TEXT("action"));};
    for(bool TargetFirst:{false,true})
    {
        FACEPluginVM VM;TestTrue(TEXT("Merge policy loads"),VM.Load(Source,Error));auto S=Snapshot();
        auto I=Step(VM,S);
        for(int32 Operation=0;Operation<2;++Operation)
        {
            if(!TestEqual(TEXT("Partial stacks across packs are merged"),Action(I),FString(TEXT("it_merge"))))break;
            TestEqual(TEXT("Smallest partial stack is the source"),ACEItemTools::Guid(I,TEXT("item")),13);
            TestEqual(TEXT("Fill the fullest partial stack first"),ACEItemTools::Guid(I,TEXT("target")),Operation==0?12:11);
            TestEqual(TEXT("Merge stops at maximum capacity"),int32(ACEItemTools::Number(I,TEXT("amount"))),Operation==0?30:20);
            S->SetNumberField(TEXT("action_serial"),Operation+1);
            TestTrue(TEXT("UseDone alone cannot confirm merging"),Action(Step(VM,S)).IsEmpty());
            auto Inventory=S->GetArrayField(TEXT("inventory"));
            auto UpdateSource=[&](){if(Operation==0)Inventory[2]->AsObject()->SetNumberField(TEXT("count"),20);else Inventory.RemoveAt(2);S->SetArrayField(TEXT("inventory"),Inventory);};
            auto UpdateTarget=[&](){Inventory[Operation==0?1:0]->AsObject()->SetNumberField(TEXT("count"),Operation==0?100:80);};
            if(TargetFirst)UpdateTarget();else UpdateSource();
            TestTrue(TEXT("One side of a merge cannot start another transaction"),Action(Step(VM,S)).IsEmpty());
            if(TargetFirst)UpdateSource();else UpdateTarget();
            I=Step(VM,S);
        }
        TestEqual(TEXT("Merge finishes once no partial pair remains"),Action(I),FString(TEXT("stop")));
        TestEqual(TEXT("Both confirmed merges are counted"),ACEItemTools::Number(I,TEXT("operations")),2.);
        TestTrue(TEXT("Run reports merge completion"),ACEItemTools::String(I,TEXT("status")).StartsWith(TEXT("Merge complete")));
        const auto& Inventory=S->GetArrayField(TEXT("inventory"));
        TestEqual(TEXT("Already-full stack stays intact"),ACEItemTools::Number(Inventory[2]->AsObject(),TEXT("count")),100.);
        TestEqual(TEXT("Same-name other item type stays separate"),ACEItemTools::Number(Inventory[3]->AsObject(),TEXT("count")),12.);
        TestEqual(TEXT("Protected stack stays intact"),ACEItemTools::Number(Inventory[4]->AsObject(),TEXT("count")),80.);
    }
    for(bool Reject:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();auto I=Step(VM,S);
        if(Reject){S->SetNumberField(TEXT("it_failure_serial"),1);S->SetNumberField(TEXT("it_failure_item"),12);}
        else S->SetNumberField(TEXT("time"),121);
        I=Step(VM,S);
        TestEqual(TEXT("Rejection or timeout stops without another merge"),Action(I),FString(TEXT("stop")));
        TestEqual(TEXT("Unconfirmed merges are not counted"),ACEItemTools::Number(I,TEXT("operations")),0.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
        P->SetNumberField(TEXT("merge_delay_ms"),125);
        Step(VM,S);
        const auto& Inventory=S->GetArrayField(TEXT("inventory"));
        Inventory[1]->AsObject()->SetNumberField(TEXT("count"),100);
        Inventory[2]->AsObject()->SetNumberField(TEXT("count"),20);
        const auto I=Step(VM,S);
        TestTrue(TEXT("Optional pause waits after confirmation"),Action(I).IsEmpty());
        TestEqual(TEXT("Merge requests a wake at the configured deadline"),ACEItemTools::Number(I,TEXT("merge_resume_at")),100.125);
        S->SetNumberField(TEXT("time"),100.125);
        TestEqual(TEXT("The next merge can dispatch at the deadline"),Action(Step(VM,S)),FString(TEXT("it_merge")));
    }
    return true;
}
#endif
