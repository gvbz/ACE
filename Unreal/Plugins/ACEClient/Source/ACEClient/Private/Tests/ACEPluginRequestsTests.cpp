#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ACEDatSubsystem.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "Interfaces/IPluginManager.h"
#include "Mods/ACEPluginSight.h"
#include "Mods/ACEPluginEquipment.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEInventoryRules.h"
#include "GameFramework/Pawn.h"
#include "ACESession.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "ProceduralMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Misc/App.h"
#include "ShaderCompiler.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginRequestsTest,"ACE.Plugins.RouteAndTellRequests",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginRequestsTest::RunTest(const FString&)
{
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
    auto* GI=NewObject<UGameInstance>(GEngine);World->SetGameInstance(GI);
    Context.OwningGameInstance=GI;Context.SetCurrentWorld(World);GI->OnWorldChanged(nullptr,World);GI->Init();
    ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);};
    auto* C=GI->GetSubsystem<UACEClientSubsystem>();auto* H=GI->GetSubsystem<UACEPluginSubsystem>();
    const FString ProfileRoot=FPaths::ProjectSavedDir()/TEXT("Automation/RouteAndTellProfiles")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    H->StorageRoot=ProfileRoot;H->Settings=MakeShared<FJsonObject>();H->Discover();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProfileRoot,false,true); };
    C->Session=MakeShared<FACESession>();auto& Session=*C->Session;
    Session.State=EACESessionState::InWorld;Session.PlayerGuid=0x50000001;
    {
        FACEWorldObject Door;Door.Guid=77;Door.Name=TEXT("Route door");Door.ItemUseable=32;
        Door.ObjectDescriptionFlags=ACEObjectDescFlag::Door;Door.bHasPosition=true;
        Session.WorldObjects.Add(Door.Guid,Door);
        for(int32 Motion:{int32(ACEMotion::OnCommandU16),int32(ACEMotion::OffCommandU16)})
        for(bool Passable:{false,true,false})
        {
            auto& Live=Session.WorldObjects[Door.Guid];Live.InitialMotionCommand=Motion;
            Live.PhysicsState=Passable?ACEPhysicsState::Ethereal:0;
            auto Snapshot=MakeShared<FJsonObject>();H->ExtendSnapshot(Snapshot);
            const auto& Objects=Snapshot->GetArrayField(TEXT("route_objects"));
            TestEqual(TEXT("Usable door is in route snapshot"),Objects.Num(),1);
            if(Objects.Num()==1)TestEqual(TEXT("Door passage follows collision state even with stale animation"),Objects[0]->AsObject()->GetBoolField(TEXT("door_open")),Passable);
            if(Objects.Num()==1)TestFalse(TEXT("Unassessed door cannot imply unlocked"),Objects[0]->AsObject()->GetBoolField(TEXT("door_identified")));
        }
        FACEAppraisalInfo A;A.bSuccess=true;A.BoolProperties.Add(3,true);A.IntProperties.Add(38,250);H->Appraisals.Add(Door.Guid,A);
        auto Snapshot=MakeShared<FJsonObject>();H->ExtendSnapshot(Snapshot);
        auto Row=Snapshot->GetArrayField(TEXT("route_objects"))[0]->AsObject();
        TestTrue(TEXT("Appraised route door exposes lock state"),Row->GetBoolField(TEXT("door_identified"))&&Row->GetBoolField(TEXT("door_locked")));
        TestEqual(TEXT("Lock difficulty retains server value"),Row->GetNumberField(TEXT("lock_difficulty")),250.);
        H->Appraisals.Remove(Door.Guid);
        Session.WorldObjects.Remove(Door.Guid);
    }
    {
        FACEWorldObject Wand; Wand.Guid = int32(0xF06A016A); Wand.Name = TEXT("Live metadata wand");
        Wand.ItemType = ACEItemType::Caster; Wand.ContainerId = Session.PlayerGuid;
        Wand.IconId = 0x06001234; Wand.IconOverlayId = 0x06001235; Wand.IconUnderlayId = 0x06001236; Wand.SpellDID = 1;
        Session.UpsertWorldObject(Wand);
        FACEAppraisalInfo A; A.ObjectGuid = Wand.Guid; A.bSuccess = true;
        H->ObserveAppraisal(A);
        auto Snapshot = MakeShared<FJsonObject>(); H->ExtendInventorySnapshot(Snapshot);
        const auto First = Snapshot->GetArrayField(TEXT("inventory"));
        if (!TestEqual(TEXT("Metadata fixture starts with one owned item"), First.Num(), 1)) return false;
        TestEqual(TEXT("Initial innate spell reaches Lua"), First[0]->AsObject()->GetNumberField(TEXT("spell")), 1.);
        for (uint32 Key : {8u, 28u, 50u, 52u})
        {
            FACEBinaryWriter W; W.WriteUInt32(0x2D8); W.WriteUInt8(1);
            W.WriteUInt32(Wand.Guid); W.WriteUInt32(Key); W.WriteUInt32(0);
            Session.ReceiveGameMessage(W.GetData());
        }
        H->ExtendInventorySnapshot(Snapshot);
        const auto Updated = Snapshot->GetArrayField(TEXT("inventory"));
        if (!TestEqual(TEXT("Metadata update does not duplicate inventory rows"), Updated.Num(), 1)) return false;
        for (const TCHAR* Field : {TEXT("icon"), TEXT("icon_overlay"), TEXT("icon_underlay"), TEXT("spell")})
            TestEqual(TEXT("Cached Lua inventory observes cleared DataID"), Updated[0]->AsObject()->GetNumberField(Field), 0.);
        TestTrue(TEXT("Cosmetic updates retain a successful appraisal"), Updated[0]->AsObject()->GetBoolField(TEXT("identified")));
        H->ExtendInventorySnapshot(Snapshot);
        TestTrue(TEXT("Unchanged metadata reuses immutable Lua row"), Snapshot->GetArrayField(TEXT("inventory"))[0] == Updated[0]);
        TestEqual(TEXT("Previous published snapshot stays immutable"), First[0]->AsObject()->GetNumberField(TEXT("spell")), 1.);
        H->Appraisals.Remove(Wand.Guid); Session.WorldObjects.Remove(Wand.Guid); ++Session.InventoryDataRevision;
    }
    auto ConfirmPut=[&](int32 Item,int32 Container)
    {
        FACEBinaryWriter W;W.WriteUInt32(Item);W.WriteUInt32(Container);W.WriteInt32(0);W.WriteInt32(0);
        FACEBinaryReader R(W.GetData());Session.HandleInventoryPutObjInContainer(R);
    };
    {
        FACEBinaryWriter Login;Login.WriteUInt32(0x97);Login.WriteUInt32(1);
        auto Header=[&](){Login.WriteUInt16(1);Login.WriteUInt16(1);};
        Header();Login.WriteUInt32(25);Login.WriteInt32(275);
        Header();Login.WriteUInt32(2);Login.WriteUInt64(8000000000000ULL);
        Header();Login.WriteUInt32(9001);Login.WriteUInt32(1);
        Header();Login.WriteUInt32(9002);Login.WriteDouble(0.25);
        Header();Login.WriteUInt32(9003);Login.WriteString16L(TEXT("Custom"));
        for(int I=0;I<8;++I)Login.WriteUInt32(0); // empty vectors, player module and inventory
        FACEBinaryReader LR(Login.GetData());Session.HandlePlayerDescription(LR);
        auto Snapshot=MakeShared<FJsonObject>();H->ExtendSnapshot(Snapshot);
        TestEqual(TEXT("Login quad reaches plugin snapshot without int32 truncation"),Snapshot->GetObjectField(TEXT("char_quads"))->GetNumberField(TEXT("2")),8000000000000.);
        TestEqual(TEXT("Login decimal reaches plugin snapshot"),Snapshot->GetObjectField(TEXT("char_doubles"))->GetNumberField(TEXT("9002")),0.25);
        TestEqual(TEXT("Login custom bool reaches plugin snapshot"),Snapshot->GetObjectField(TEXT("char_bools"))->GetNumberField(TEXT("9001")),1.);
        TestEqual(TEXT("Login custom string reaches plugin snapshot"),Snapshot->GetObjectField(TEXT("char_strings"))->GetStringField(TEXT("9003")),FString(TEXT("Custom")));
        for(bool Public:{false,true})
        {
            FACEBinaryWriter Q;Q.WriteUInt32(Public?ACEOpcode::PublicUpdatePropertyInt64:ACEOpcode::PrivateUpdatePropertyInt64);Q.WriteUInt8(1);
            if(Public)Q.WriteUInt32(Session.PlayerGuid);Q.WriteUInt32(2);Q.WriteUInt64(Public?9000000000001ULL:9000000000000ULL);Session.HandleGameMessage(Q.GetData());
            FACEBinaryWriter D;D.WriteUInt32(Public?ACEOpcode::PublicUpdatePropertyFloat:ACEOpcode::PrivateUpdatePropertyFloat);D.WriteUInt8(1);
            if(Public)D.WriteUInt32(Session.PlayerGuid);D.WriteUInt32(9002);D.WriteDouble(Public?0.75:0.5);Session.HandleGameMessage(D.GetData());
            FACEBinaryWriter B;B.WriteUInt32(Public?ACEOpcode::PublicUpdatePropertyBool:ACEOpcode::PrivateUpdatePropertyBool);B.WriteUInt8(1);
            if(Public)B.WriteUInt32(Session.PlayerGuid);B.WriteUInt32(9001);B.WriteUInt32(Public?1:0);Session.HandleGameMessage(B.GetData());
            FACEBinaryWriter S;S.WriteUInt32(Public?ACEOpcode::PublicUpdatePropertyString:ACEOpcode::PrivateUpdatePropertyString);S.WriteUInt8(1);S.WriteUInt32(9003);
            if(Public)S.WriteUInt32(Session.PlayerGuid);S.Align();S.WriteString16L(Public?TEXT("Public"):TEXT("Private"));Session.HandleGameMessage(S.GetData());
            H->ExtendSnapshot(Snapshot);
            TestEqual(TEXT("Live quad uses the server value"),Snapshot->GetObjectField(TEXT("char_quads"))->GetNumberField(TEXT("2")),Public?9000000000001.:9000000000000.);
            TestEqual(TEXT("Live decimal uses the server value"),Snapshot->GetObjectField(TEXT("char_doubles"))->GetNumberField(TEXT("9002")),Public?0.75:0.5);
            TestEqual(TEXT("Live bool updates before player ObjectCreate"),Snapshot->GetObjectField(TEXT("char_bools"))->GetNumberField(TEXT("9001")),Public?1.:0.);
            TestEqual(TEXT("String packet handles property/GUID order and alignment"),Snapshot->GetObjectField(TEXT("char_strings"))->GetStringField(TEXT("9003")),FString(Public?TEXT("Public"):TEXT("Private")));
        }
        TestTrue(TEXT("Absent exemption still requires components"),Snapshot->GetBoolField(TEXT("components_required")));
        for(bool Required:{false,true})
        {
            FACEBinaryWriter B;B.WriteUInt32(Required?ACEOpcode::PublicUpdatePropertyBool:ACEOpcode::PrivateUpdatePropertyBool);B.WriteUInt8(1);
            if(Required)B.WriteUInt32(Session.PlayerGuid);B.WriteUInt32(68);B.WriteUInt32(Required?1:0);Session.HandleGameMessage(B.GetData());
            H->ExtendSnapshot(Snapshot);TestEqual(TEXT("Server component policy updates live"),Snapshot->GetBoolField(TEXT("components_required")),Required);
        }
        FACEBinaryWriter Other;Other.WriteUInt32(ACEOpcode::PublicUpdatePropertyFloat);Other.WriteUInt8(1);Other.WriteUInt32(Session.PlayerGuid+1);Other.WriteUInt32(9002);Other.WriteDouble(99);Session.HandleGameMessage(Other.GetData());
        TestEqual(TEXT("Another player's update cannot contaminate self qualities"),Session.PlayerVitals.QualityDoubles.FindRef(9002),0.75);
        FACEBinaryWriter Short;Short.WriteUInt32(ACEOpcode::PrivateUpdatePropertyString);Short.WriteUInt8(1);Short.WriteUInt32(9003);Short.Align();Short.WriteUInt16(50);Session.HandleGameMessage(Short.GetData());
        TestEqual(TEXT("Truncated string preserves last valid value"),Session.PlayerVitals.QualityStrings.FindRef(9003),FString(TEXT("Public")));
        FACEBinaryWriter Exemption;Exemption.WriteUInt32(0x2D1);Exemption.WriteUInt8(2);Exemption.WriteUInt32(68);Exemption.WriteUInt32(0);
        Session.ReceiveGameMessage(Exemption.GetData());H->ExtendSnapshot(Snapshot);
        TestFalse(TEXT("Explicit component exemption reaches automation"),Snapshot->GetBoolField(TEXT("components_required")));
        struct RemovedQuality {uint32 Opcode,Key;const TCHAR* Field;};
        for(const RemovedQuality Q : {
            RemovedQuality{0x1D2,25,TEXT("char_ints")},
            RemovedQuality{0x2B9,2,TEXT("char_quads")},
            RemovedQuality{0x1D4,9001,TEXT("char_bools")},
            RemovedQuality{0x1D6,9002,TEXT("char_doubles")},
            RemovedQuality{0x1D8,9003,TEXT("char_strings")},
            RemovedQuality{0x1D4,68,TEXT("char_bools")}})
        {
            FACEBinaryWriter Remove;Remove.WriteUInt32(Q.Opcode);Remove.WriteUInt8(3);
            Remove.WriteUInt32(Session.PlayerGuid);Remove.WriteUInt32(Q.Key);
            Session.ReceiveGameMessage(Remove.GetData());H->ExtendSnapshot(Snapshot);
            TestFalse(TEXT("Server deletion removes the key from the next Lua snapshot"),
                Snapshot->GetObjectField(Q.Field)->HasField(FString::FromInt(Q.Key)));
        }
        TestTrue(TEXT("Deleting exemption restores required components for UCM"),Snapshot->GetBoolField(TEXT("components_required")));
        TestEqual(TEXT("Deleted level cannot leave a stale top-level Lua value"),Snapshot->GetNumberField(TEXT("level")),0.);
        FACEBinaryWriter Relog;Relog.WriteUInt32(0);Relog.WriteUInt32(1);for(int I=0;I<8;++I)Relog.WriteUInt32(0);
        FACEBinaryReader RR(Relog.GetData());Session.HandlePlayerDescription(RR);
        TestTrue(TEXT("Relog cannot retain previous character qualities"),Session.PlayerVitals.QualityStrings.IsEmpty()&&Session.PlayerVitals.QualityDoubles.IsEmpty()&&Session.PlayerVitals.QualityBools.IsEmpty()&&Session.PlayerVitals.QualityInt64s.IsEmpty());
    }
    auto P=H->Find(TEXT("ucm"));if(!TestTrue(TEXT("UCM discovered"),P.IsValid()))return false;
    P->Enabled=true;P->Running=true;P->Profile=MakeShared<FJsonObject>();
    P->Player=Session.PlayerGuid;P->Server=C->GetServerName();Session.ApplyServerCombatMode(1);
    P->Profile->SetBoolField(TEXT("buffing"),true);P->Profile->SetBoolField(TEXT("buff_others"),true);
    {
        FACEWorldObject Vendor;Vendor.Guid=500;Vendor.Name=TEXT("Arcanist");Vendor.WeenieClassId=200;
        auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        FSocket* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("UCM vendor test receiver"),false);
        const auto Address=Sockets->CreateInternetAddr();bool Valid=false;Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
        if(!TestTrue(TEXT("Isolated vendor test socket binds"),Receiver&&Receiver->Bind(*Address)))return false;
        Receiver->GetAddress(*Address);Session.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("UCM vendor test sender"),false);
        Session.ServerC2SAddr=Address;Session.IssacClient=MakeUnique<FACEIsaac>(123u);
        ON_SCOPE_EXIT {Session.SocketC2S->Close();Sockets->DestroySocket(Session.SocketC2S);Session.SocketC2S=nullptr;Session.ServerC2SAddr.Reset();Session.IssacClient.Reset();Receiver->Close();Sockets->DestroySocket(Receiver);Session.CachedC2SPackets.Reset();};
        {
            auto* FellowPC=World->SpawnActor<AACEPlayerController>();auto* Local=NewObject<ULocalPlayer>(GEngine);
            GI->AddLocalPlayer(Local,FPlatformUserId::CreateFromInternalId(0));FellowPC->SetPlayer(Local);
            const auto SavedFellowship=Session.Fellowship;const bool WasGDLE=Session.Creds.bGDLE;const auto Permissions=P->Permissions;
            ON_SCOPE_EXIT {Session.Fellowship=SavedFellowship;Session.Creds.bGDLE=WasGDLE;P->Permissions=Permissions;P->NextAction=0;P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));Session.WorldObjects.Remove(8101);GI->RemoveLocalPlayer(Local);FellowPC->Destroy();Session.CachedC2SPackets.Reset();};
            P->Permissions.Add(TEXT("fellowship"));
            FACEFellowshipInfo F;F.bValid=true;F.Name=TEXT("Test party");F.LeaderGuid=Session.PlayerGuid;
            FACEFellowshipMember Self,Ally;Self.Guid=Session.PlayerGuid;Ally.Guid=int32(0xF0008100);Ally.Name=TEXT("Ally");F.Members={Self,Ally};
            FACEWorldObject Nearby;Nearby.Guid=8101;Nearby.Name=TEXT("Nearby");Nearby.bIsPlayer=true;Session.WorldObjects.Add(Nearby.Guid,Nearby);
            auto Intent=MakeShared<FJsonObject>();Intent->SetStringField(TEXT("action"),TEXT("fellowship"));
            auto Dispatch=[&](const TCHAR* Op,int32 Target)
            {P->NextAction=0;P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));Session.CachedC2SPackets.Reset();Intent->SetStringField(TEXT("operation"),Op);Intent->SetNumberField(TEXT("target"),uint32(Target));H->Execute(*P,Intent);};
            for(bool GDLE:{false,true})for(const TCHAR* Op:{TEXT("dismiss"),TEXT("leader"),TEXT("recruit")})
            {
                Session.Creds.bGDLE=GDLE;Session.Fellowship=F;const bool Recruit=FString(Op)==TEXT("recruit");const int32 Target=Recruit?Nearby.Guid:Ally.Guid;
                Dispatch(Op,Target);TestEqual(TEXT("Fellowship API sends one ACE/GDLE action"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                    TestEqual(TEXT("Fellowship operation retains retail opcode"),R.ReadUInt32(),Recruit?ACEGameAction::FellowshipRecruit:FString(Op)==TEXT("leader")?ACEGameAction::FellowshipAssignNewLeader:ACEGameAction::FellowshipDismiss);
                    TestEqual(TEXT("Fellowship GUID remains unsigned on wire"),R.ReadUInt32(),uint32(Target));
                }
                TestEqual(TEXT("Sending never predicts membership or leadership"),Session.Fellowship.LeaderGuid,F.LeaderGuid);
                TestEqual(TEXT("Sending never predicts member removal"),Session.Fellowship.Members.Num(),2);
                Session.Fellowship.LeaderGuid=Ally.Guid;Dispatch(Op,Target);TestTrue(TEXT("Closed fellowship nonleader cannot manage members"),Session.CachedC2SPackets.IsEmpty()&&P->ActivityFailure.IsValid());
            }
            Session.Fellowship=F;Session.Fellowship.LeaderGuid=Ally.Guid;Session.Fellowship.bOpen=true;Dispatch(TEXT("recruit"),Nearby.Guid);
            TestEqual(TEXT("Open fellowship permits nonleader recruiting"),Session.CachedC2SPackets.Num(),1);
            Session.Fellowship=F;
            for(const TCHAR* Op:{TEXT("dismiss"),TEXT("leader")})for(int32 Id:{Session.PlayerGuid,999})
            {Dispatch(Op,Id);TestTrue(TEXT("Self or nonmember management is rejected"),Session.CachedC2SPackets.IsEmpty()&&P->ActivityFailure.IsValid());}
            Dispatch(TEXT("recruit"),Ally.Guid);TestTrue(TEXT("Already joined member is not recruited again"),Session.CachedC2SPackets.IsEmpty()&&!P->ActivityFailure.IsValid());
            Intent->SetStringField(TEXT("name"),F.Name);Dispatch(TEXT("create"),0);TestTrue(TEXT("Existing fellowship is not recreated"),Session.CachedC2SPackets.IsEmpty()&&!P->ActivityFailure.IsValid());
            Intent->SetStringField(TEXT("name"),TEXT("Other party"));Dispatch(TEXT("create"),0);TestTrue(TEXT("Creation cannot silently replace a fellowship"),Session.CachedC2SPackets.IsEmpty()&&P->ActivityFailure.IsValid());
            Session.Fellowship.bLocked=true;Dispatch(TEXT("open"),0);TestTrue(TEXT("Locked fellowship openness is rejected"),Session.CachedC2SPackets.IsEmpty()&&P->ActivityFailure.IsValid());
            P->Permissions.Add(TEXT("say"));P->Profile->SetBoolField(TEXT("auto_fellowship"),true);H->FellowshipTells.Reset();H->UCMServicesProfile.Reset();
            H->QueueFellowshipTell(TEXT("hello"),TEXT("Visitor"),0x50000020);H->QueueFellowshipTell(TEXT("xp"),TEXT("NPC"),0x70000001);
            TestTrue(TEXT("Only authenticated player commands enter manager queue"),H->FellowshipTells.IsEmpty());
            H->QueueFellowshipTell(TEXT("xp"),TEXT("Visitor"),0x50000020);TestEqual(TEXT("Recruitment tell captured without buffing"),H->FellowshipTells.Num(),1);
            auto Public=MakeShared<FJsonObject>();H->ExtendSnapshot(Public);TestFalse(TEXT("Other data snapshots cannot read manager tells"),Public->HasField(TEXT("fellowship_tells")));
            for(bool GDLE:{false,true})
            {
                Session.Creds.bGDLE=GDLE;Session.CachedC2SPackets.Reset();P->NextAction=0;
                auto Reply=MakeShared<FJsonObject>();Reply->SetStringField(TEXT("action"),TEXT("say"));Reply->SetStringField(TEXT("recipient"),TEXT("Visitor"));Reply->SetStringField(TEXT("text"),TEXT("Your spot is reserved"));H->Execute(*P,Reply);
                TestEqual(TEXT("Manager reply uses one ACE/GDLE tell"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);TestEqual(TEXT("Reply uses retail tell opcode"),R.ReadUInt32(),ACEGameAction::Tell);TestEqual(TEXT("Tell text survives dispatch"),R.ReadString16L(),FString(TEXT("Your spot is reserved")));TestEqual(TEXT("Tell goes to applicant"),R.ReadString16L(),FString(TEXT("Visitor")));}
            }
            H->FellowshipTells.Reset();P->Profile->SetBoolField(TEXT("auto_fellowship"),false);
            P->Permissions.Remove(TEXT("fellowship"));Dispatch(TEXT("leader"),Ally.Guid);TestTrue(TEXT("New operations still require fellowship capability"),Session.CachedC2SPackets.IsEmpty()&&P->ActivityFailure.IsValid());
        }
        FACEWorldObject Stock;Stock.Guid=501;Stock.Name=TEXT("Scarab");Stock.WeenieClassId=300;Stock.MaxStackSize=100;Stock.VendorQuantityAvailable=-1;
        Session.OpenVendorGuid=Vendor.Guid;Session.WorldObjects.Add(Vendor.Guid,Vendor);Session.VendorMerchandise={Stock};
        auto Snapshot=MakeShared<FJsonObject>();H->ExtendSnapshot(Snapshot);
        TestEqual(TEXT("Vendor snapshot carries stable identity"),Snapshot->GetNumberField(TEXT("vendor_wcid")),200.);
        TestEqual(TEXT("Vendor stock includes retail quantity limit"),Snapshot->GetArrayField(TEXT("vendor_stock"))[0]->AsObject()->GetNumberField(TEXT("limit")),100.);
        auto Buy=MakeShared<FJsonObject>();Buy->SetNumberField(TEXT("vendor"),500);Buy->SetNumberField(TEXT("item"),501);Buy->SetNumberField(TEXT("count"),3);
        Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Buy,TEXT("buy"));
        TestEqual(TEXT("Restock sends one retail action"),Session.CachedC2SPackets.Num(),1);
        for(const auto& Packet:Session.CachedC2SPackets)
        {
            FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
            TestEqual(TEXT("Restock uses retail Buy opcode"),R.ReadUInt32(),ACEGameAction::Buy);
            TestEqual(TEXT("Purchase identifies open vendor"),R.ReadUInt32(),500u);TestEqual(TEXT("Purchase has one stock row"),R.ReadUInt32(),1u);
            TestEqual(TEXT("Purchase transmits missing units"),R.ReadInt32(),3);TestEqual(TEXT("Purchase transmits stock GUID"),R.ReadUInt32(),501u);
        }
        auto HasPurchase=[&](){for(const auto& Packet:Session.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()==ACEGameAction::Buy)return true;}return false;};
        {
            const auto SavedObjects=Session.WorldObjects;
            ON_SCOPE_EXIT {Session.WorldObjects=SavedObjects;Session.VendorCurrencyName.Empty();Session.VendorCurrencyWeenie=0;Session.TradeSelfItems.Reset();Session.CachedC2SPackets.Reset();P->Running=true;};
            FACEWorldObject Self;Self.Guid=Session.PlayerGuid;Self.ItemsCapacity=102;Session.WorldObjects.Add(Self.Guid,Self);
            FACEWorldObject Note;Note.Guid=600;Note.WeenieClassId=400;Note.Name=TEXT("Trade Note");Note.ItemType=ACEItemType::PromissoryNote;
            Note.ContainerId=Session.PlayerGuid;Note.StackSize=10;Note.MaxStackSize=100;Note.Value=10000;Session.WorldObjects.Add(Note.Guid,Note);
            FACEWorldObject Coins;Coins.Guid=601;Coins.WeenieClassId=273;Coins.ItemType=ACEItemType::Money;Coins.ContainerId=Self.Guid;Coins.StackSize=100;Session.WorldObjects.Add(Coins.Guid,Coins);
            auto Refresh=[&](){auto Out=MakeShared<FJsonObject>();H->ExtendSnapshot(Out);return Out;};
            auto Money=Refresh();TestEqual(TEXT("Live restock purse counts carried pyreals"),Money->GetNumberField(TEXT("pyreals")),100.);
            TestEqual(TEXT("Trade note redemption uses face value per unit"),Money->GetArrayField(TEXT("vendor_trade_notes"))[0]->AsObject()->GetNumberField(TEXT("unit_value")),1000.);
            auto CountSales=[&](){int32 Count=0;for(const auto& Packet:Session.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.CanRead(4)&&R.ReadUInt32()==ACEGameAction::Sell)++Count;}return Count;};
            auto Redeem=MakeShared<FJsonObject>();Redeem->SetNumberField(TEXT("vendor"),500);Redeem->SetNumberField(TEXT("item"),600);Redeem->SetNumberField(TEXT("count"),4);
            Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Redeem,TEXT("split_note"));
            TestEqual(TEXT("Partial redemption sends exactly one split"),Session.CachedC2SPackets.Num(),1);
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);TestEqual(TEXT("Trade note split uses retail opcode"),R.ReadUInt32(),ACEGameAction::StackableSplitToContainer);
                TestEqual(TEXT("Split identifies original stack"),R.ReadUInt32(),600u);TestEqual(TEXT("Split stays in owned pack"),R.ReadUInt32(),uint32(Self.Guid));
                R.ReadInt32();TestEqual(TEXT("Split transmits needed quantity only"),R.ReadInt32(),4);
            }
            Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Redeem,TEXT("sell_note"));
            TestEqual(TEXT("Cannot sell an unsplit partial stack"),CountSales(),0);
            Session.CachedC2SPackets.Reset();P->Running=true;Note.Guid=602;Note.StackSize=4;Note.Value=4000;Session.WorldObjects.Add(Note.Guid,Note);Redeem->SetNumberField(TEXT("item"),602);
            H->ExecuteInventory(*P,Redeem,TEXT("sell_note"));TestEqual(TEXT("Confirmed note object can be redeemed"),Session.CachedC2SPackets.Num(),1);
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);TestEqual(TEXT("Redemption uses retail Sell opcode"),R.ReadUInt32(),ACEGameAction::Sell);
                TestEqual(TEXT("Redemption identifies current vendor"),R.ReadUInt32(),500u);TestEqual(TEXT("One note stack is sold"),R.ReadUInt32(),1u);
                TestEqual(TEXT("Exact split quantity is sold"),R.ReadInt32(),4);TestEqual(TEXT("Only the new stack is sold"),R.ReadUInt32(),602u);
            }
            Session.WorldObjects[600].ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;
            Session.WorldObjects[602].ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;
            TestEqual(TEXT("Retained notes excluded immediately without appraisal refresh"),Refresh()->GetArrayField(TEXT("vendor_trade_notes")).Num(),0);
            Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Redeem,TEXT("sell_note"));TestEqual(TEXT("Retained note cannot be redeemed even with stale intent"),CountSales(),0);
            Session.WorldObjects[602].ObjectDescriptionFlags=0;Session.VendorCurrencyName=TEXT("Pyreals");P->Running=true;
            H->ExecuteInventory(*P,Redeem,TEXT("sell_note"));TestEqual(TEXT("Ordinary currency display name does not block note redemption"),CountSales(),1);
            Session.CachedC2SPackets.Reset();Session.VendorCurrencyName=TEXT("Tokens");Session.VendorCurrencyWeenie=999;P->Running=true;
            H->ExecuteInventory(*P,Redeem,TEXT("sell_note"));TestEqual(TEXT("Custom currency vendor cannot redeem for restocking"),CountSales(),0);
            Session.CachedC2SPackets.Reset();Session.VendorCurrencyName.Empty();P->Running=true;
            TestFalse(TEXT("Unnamed alternate currency is not pyreals"),Refresh()->GetBoolField(TEXT("vendor_uses_pyreals")));
            H->ExecuteInventory(*P,Redeem,TEXT("sell_note"));TestEqual(TEXT("Unnamed alternate currency cannot sell notes"),CountSales(),0);
            Session.CachedC2SPackets.Reset();Session.VendorCurrencyWeenie=0;Session.TradeSelfItems.Add(602);P->Running=true;
            TestEqual(TEXT("Notes offered in trade excluded from redemption"),Refresh()->GetArrayField(TEXT("vendor_trade_notes")).Num(),0);
            H->ExecuteInventory(*P,Redeem,TEXT("sell_note"));TestEqual(TEXT("Stale trade-offered intent cannot sell notes"),CountSales(),0);
        }
        Session.CachedC2SPackets.Reset();Session.VendorMerchandise[0].VendorQuantityAvailable=0;H->ExecuteInventory(*P,Buy,TEXT("buy"));
        TestTrue(TEXT("Sold-out stock cannot send a purchase"),!HasPurchase());
        P->Running=true;Session.OpenVendorGuid=0;H->ExecuteInventory(*P,Buy,TEXT("buy"));
        TestTrue(TEXT("Closed vendor cannot send a purchase"),!HasPurchase());
        Session.CachedC2SPackets.Reset();Session.OpenVendorGuid=500;
        auto Close=MakeShared<FJsonObject>();Close->SetNumberField(TEXT("vendor"),501);
        H->ExecuteInventory(*P,Close,TEXT("close_vendor"));
        TestEqual(TEXT("Stale vendor close cannot close a different shop"),Session.OpenVendorGuid,500);
        TestEqual(TEXT("Stale close sends no packet"),Session.CachedC2SPackets.Num(),0);
        Close->SetNumberField(TEXT("vendor"),500);H->ExecuteInventory(*P,Close,TEXT("close_vendor"));
        TestEqual(TEXT("Vendor close updates observed state"),Session.OpenVendorGuid,0);
        TestEqual(TEXT("Close sends one retail request"),Session.CachedC2SPackets.Num(),1);
        for(const auto& Packet:Session.CachedC2SPackets)
        {
            FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
            TestEqual(TEXT("Close uses NoLongerViewingContents"),R.ReadUInt32(),ACEGameAction::NoLongerViewingContents);
            TestEqual(TEXT("Close identifies exact vendor"),R.ReadUInt32(),500u);
        }
        P->Running=true;Session.WorldObjects.Remove(Vendor.Guid);Session.VendorMerchandise.Empty();
        {
            auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
            if(!TestTrue(TEXT("Recovery implement DAT loads"),Dat->LoadDatDirectory(ACETestDatDirectory())))return false;
            uint32 Heal=0;
            for(uint32 Id=1;Id<7000;++Id){FString Name;uint32 Icon=0;if(Dat->TryGetSpellInfo(Id,Name,Icon)&&Name==TEXT("Heal Self I")){Heal=Id;break;}}
            if(!TestTrue(TEXT("Innate recovery spell found"),Heal!=0))return false;
            {
                FACEAppraisalInfo Requirements;Requirements.bSuccess=true;FACEPlayerVitals V;
                Requirements.IntProperties.Add(188,3);V.HeritageGroup=3;
                Requirements.IntProperties.Add(110,4);V.EffectiveAllegianceRank=4;
                TestTrue(TEXT("Activation uses authoritative heritage and effective rank"),ACEPluginEquipment::CanActivate(Requirements,V));
                V.HeritageGroup=2;TestFalse(TEXT("Wrong heritage cannot activate"),ACEPluginEquipment::CanActivate(Requirements,V));V.HeritageGroup=3;
                V.EffectiveAllegianceRank=3;TestFalse(TEXT("Insufficient rank cannot activate"),ACEPluginEquipment::CanActivate(Requirements,V));V.EffectiveAllegianceRank=4;
                Requirements.IntProperties.Add(259,1);Requirements.IntProperties.Add(260,100);V.MaxHealth=99;
                TestFalse(TEXT("Activation vital requirement is checked"),ACEPluginEquipment::CanActivate(Requirements,V));V.MaxHealth=100;
                TestTrue(TEXT("Activation vital equality is accepted"),ACEPluginEquipment::CanActivate(Requirements,V));
                Requirements.IntProperties.Add(369,50);V.Level=49;
                TestFalse(TEXT("Use level requirement is checked"),ACEPluginEquipment::CanActivate(Requirements,V));
            }
            FACEWorldObject Wand;Wand.Guid=9300;Wand.Name=TEXT("Recovery wand");Wand.ItemType=ACEItemType::Caster;
            Wand.SpellDID=Heal;Wand.WielderId=Session.PlayerGuid;Wand.CurrentWieldedLocation=0x1000000;Wand.ItemUseable=0x00100008;
            Session.WorldObjects.Add(Wand.Guid,Wand);
            FACEAppraisalInfo A;A.bSuccess=true;A.IntProperties.Add(107,100);A.IntProperties.Add(108,100);H->Appraisals.Add(Wand.Guid,A);
            const int32 OldMode=Session.PlayerVitals.CombatMode;const bool WasGDLE=Session.Creds.bGDLE;
            Session.PendingCombatMode=0;Session.ApplyServerCombatMode(8);
            auto Use=MakeShared<FJsonObject>();Use->SetNumberField(TEXT("item"),Wand.Guid);Use->SetNumberField(TEXT("target"),uint32(Session.PlayerGuid));Use->SetNumberField(TEXT("spell"),Heal);
            for(bool GDLE:{false,true})
            {
                Session.Creds.bGDLE=GDLE;Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Use,TEXT("cast_item"));
                TestEqual(TEXT("Innate recovery sends one action on ACE/GDLE"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                    TestEqual(TEXT("Item spell is UseWithTarget, never CastTargetedSpell"),R.ReadUInt32(),ACEGameAction::UseWithTarget);
                    TestEqual(TEXT("Innate source is wand"),R.ReadUInt32(),uint32(Wand.Guid));TestEqual(TEXT("Innate target is self"),R.ReadUInt32(),uint32(Session.PlayerGuid));
                }
                H->CompletePendingAction(0);TestTrue(TEXT("Resource refresh preserves wand assessment"),H->Appraisals.Contains(Wand.Guid));
                Session.ClearPendingUse();P->WaitAction.Empty();P->NextAction=0;
            }
            for(int32 Case=0;Case<5;++Case)
            {
                H->Appraisals[Wand.Guid]=A;Session.WorldObjects[Wand.Guid]=Wand;Use->SetNumberField(TEXT("target"),uint32(Session.PlayerGuid));Use->SetNumberField(TEXT("spell"),Heal);
                if(Case==0)H->Appraisals[Wand.Guid].IntProperties.Add(109,10000);
                if(Case==1)Session.WorldObjects[Wand.Guid].ItemCurrentMana=0;
                if(Case==2)Use->SetNumberField(TEXT("spell"),Heal+1);
                if(Case==3)Use->SetNumberField(TEXT("target"),500);
                if(Case==4){Session.WorldObjects[Wand.Guid].WielderId=0;Session.WorldObjects[Wand.Guid].CurrentWieldedLocation=0;}
                Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Use,TEXT("cast_item"));
                TestEqual(TEXT("Stale or ineligible innate request sends nothing"),Session.CachedC2SPackets.Num(),0);
                P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));
            }
            uint32 Imperil=0;
            for(uint32 Id=1;Id<7000;++Id){FString Name;uint32 Icon=0;if(Dat->TryGetSpellInfo(Id,Name,Icon)&&Name==TEXT("Imperil Other I")){Imperil=Id;break;}}
            if(!TestTrue(TEXT("Retail debuff spell found"),Imperil!=0))return false;
            {
                FACEWorldObject Weapon=Wand;Weapon.Guid=9390;Weapon.ItemType=ACEItemType::MeleeWeapon;Weapon.CurrentWieldedLocation=ACEEquipMask::MeleeWeapon;
                Weapon.ValidLocations=ACEEquipMask::MeleeWeapon;Weapon.SpellDID=0;
                FACEAppraisalInfo Proc=A;Proc.SpellIds={int32(Imperil)};Proc.IntProperties.Add(106,500);
                H->Appraisals.Add(Weapon.Guid,Proc);Session.WorldObjects.Add(Weapon.Guid,Weapon);
                auto Row=H->PluginItemSnapshot(Weapon)->AsObject();
                TestTrue(TEXT("Physical first-spell proc exposes native eligibility"),Row->GetBoolField(TEXT("proc_debuff_available")));
                TestEqual(TEXT("Physical proc carries actual spellcraft"),Row->GetObjectField(TEXT("proc_debuff"))->GetNumberField(TEXT("skill")),500.);
                TestFalse(TEXT("Physical proc is not an activatable innate wand spell"),Row->HasField(TEXT("innate_spell")));
                Weapon.ItemCurrentMana=0;TestFalse(TEXT("Drained proc weapon unavailable"),H->PluginItemSnapshot(Weapon)->AsObject()->GetBoolField(TEXT("proc_debuff_available")));Weapon.ItemCurrentMana=-1;
                H->Appraisals[Weapon.Guid].IntProperties.Add(109,10000);TestFalse(TEXT("Proc activation requirements checked"),H->PluginItemSnapshot(Weapon)->AsObject()->GetBoolField(TEXT("proc_debuff_available")));
                H->Appraisals[Weapon.Guid]=Proc;H->Appraisals[Weapon.Guid].SpellIds={int32(Heal),int32(Imperil)};
                TestFalse(TEXT("A later spell cannot be invented as the weapon proc"),H->PluginItemSnapshot(Weapon)->AsObject()->HasField(TEXT("proc_debuff")));
                H->Appraisals.Remove(Weapon.Guid);Session.WorldObjects.Remove(Weapon.Guid);
            }
            Wand.SpellDID=Imperil;Session.WorldObjects[Wand.Guid]=Wand;H->Appraisals[Wand.Guid]=A;
            FACEWorldObject Enemy;Enemy.Guid=9301;Enemy.Name=TEXT("Item debuff target");Enemy.ItemType=ACEItemType::Creature;
            Enemy.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;Enemy.bHasPosition=true;Enemy.Position=Session.PlayerPosition;Enemy.Position.Location.X+=3;
            Session.WorldObjects.Add(Enemy.Guid,Enemy);Use->SetNumberField(TEXT("target"),Enemy.Guid);Use->SetNumberField(TEXT("spell"),Imperil);
            for(bool GDLE:{false,true})
            {
                Session.Creds.bGDLE=GDLE;Session.CachedC2SPackets.Reset();Session.ClearPendingUse();P->WaitAction.Empty();
                H->ExecuteInventory(*P,Use,TEXT("cast_item"));TestEqual(TEXT("Debuff implement sends one ACE/GDLE request"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);TestEqual(TEXT("Debuff uses item activation"),R.ReadUInt32(),ACEGameAction::UseWithTarget);TestEqual(TEXT("Debuff source is wand"),R.ReadUInt32(),uint32(Wand.Guid));TestEqual(TEXT("Debuff target is creature"),R.ReadUInt32(),uint32(Enemy.Guid));}
                H->CompletePendingAction(0);
            }
            for(int Case=0;Case<3;++Case)
            {
                Session.WorldObjects[Enemy.Guid]=Enemy;
                if(Case==0)Session.WorldObjects[Enemy.Guid].bIsPlayer=true;
                if(Case==1)Session.WorldObjects[Enemy.Guid].PetOwnerId=Session.PlayerGuid;
                if(Case==2)Session.WorldObjects[Enemy.Guid].bDying=true;
                Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Use,TEXT("cast_item"));
                TestEqual(TEXT("Player, pet and dying targets reject debuff activation"),Session.CachedC2SPackets.Num(),0);P->ActivityFailure.Reset();
            }
            Session.WorldObjects.Remove(Enemy.Guid);H->PendingDebuffCasts.Empty();Session.ClearPendingUse();
            FACEWorldObject Grenade;Grenade.Guid=9302;Grenade.Name=TEXT("Fixture grenade");Grenade.ItemType=ACEItemType::MissileWeapon;Grenade.ContainerId=Session.PlayerGuid;Grenade.StackSize=3;
            Session.WorldObjects.Add(Grenade.Guid,Grenade);
            auto Catalog=MakeShared<FJsonObject>(),Entry=MakeShared<FJsonObject>(),Assist=MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("WieldReqType"),0);Entry->SetNumberField(TEXT("WieldReqAttribute"),0);Entry->SetNumberField(TEXT("WieldReqValue"),0);Entry->SetNumberField(TEXT("Spell"),Imperil);Entry->SetNumberField(TEXT("Spellcraft"),500);Catalog->SetObjectField(Grenade.Name,Entry);
            Assist->SetStringField(TEXT("name"),Grenade.Name);Assist->SetNumberField(TEXT("type"),7);
            H->UCMServicesProfile=MakeShared<FJsonObject>();H->UCMServicesProfile->SetObjectField(TEXT("grenade_catalog"),Catalog);H->UCMServicesProfile->SetArrayField(TEXT("assist_items"),{MakeShared<FJsonValueObject>(Assist)});P->Running=true;
            TestTrue(TEXT("Known grenade supplies expose native spell metadata"),H->UCMGrenadeSpell(Grenade).IsValid());
            Session.TradeSelfItems.Add(Grenade.Guid);TestFalse(TEXT("Traded grenades are ineligible"),H->UCMGrenadeSpell(Grenade).IsValid());Session.TradeSelfItems.Reset();
            Grenade.StackSize=0;TestFalse(TEXT("Empty grenades are ineligible"),H->UCMGrenadeSpell(Grenade).IsValid());Grenade.StackSize=3;
            Entry->SetNumberField(TEXT("WieldReqType"),7);Entry->SetNumberField(TEXT("WieldReqValue"),10000);TestFalse(TEXT("Grenade level requirement is rechecked"),H->UCMGrenadeSpell(Grenade).IsValid());
            Entry->SetNumberField(TEXT("WieldReqType"),0);Entry->SetNumberField(TEXT("Spell"),Heal);TestFalse(TEXT("Beneficial spells cannot masquerade as grenades"),H->UCMGrenadeSpell(Grenade).IsValid());
            H->UCMServicesProfile.Reset();Session.WorldObjects.Remove(Grenade.Guid);P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));
            Session.Creds.bGDLE=WasGDLE;Session.ApplyServerCombatMode(OldMode);H->Appraisals.Remove(Wand.Guid);Session.WorldObjects.Remove(Wand.Guid);
        }
        {
            const int32 OldMode=Session.PlayerVitals.CombatMode;
            FACEWorldObject Essence;Essence.Guid=9200;Essence.ContainerId=Session.PlayerGuid;
            Essence.Name=TEXT("Custom Summoning Essence");Essence.ItemType=ACEItemType::Misc;Essence.ItemUseable=8;
            Session.WorldObjects.Add(Essence.Guid,Essence);
            FACEAppraisalInfo Appraisal;Appraisal.bSuccess=true;Appraisal.IntProperties.Add(280,213);H->Appraisals.Add(Essence.Guid,Appraisal);
            auto Use=MakeShared<FJsonObject>();Use->SetNumberField(TEXT("item"),Essence.Guid);
            for(int32 Cooldown:{213,999})for(int32 Mode:{1,2,4,8})
            {
                H->Appraisals[Essence.Guid].IntProperties.Add(280,Cooldown);
                H->Appraisals[Essence.Guid].IntProperties.Add(366,54);
                Session.PendingCombatMode=0;Session.ApplyServerCombatMode(Mode);Session.bServerAttackInProgress=false;Session.LastAttackError=0x20;Session.CachedC2SPackets.Reset();
                H->ExecuteInventory(*P,Use,TEXT("use_item"));
                TestEqual(TEXT("Summoning preserves current combat mode"),Session.PlayerVitals.CombatMode,Mode);
                TestEqual(TEXT("Summon sends only Use, with no peace-mode packet"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                    TestEqual(TEXT("Summon uses standard retail Use opcode"),R.ReadUInt32(),ACEGameAction::Use);
                    TestEqual(TEXT("Summon targets selected essence"),R.ReadUInt32(),uint32(Essence.Guid));
                }
            }
            Session.WorldObjects.Remove(Essence.Guid);H->Appraisals.Remove(Essence.Guid);
            Session.PendingCombatMode=0;Session.ApplyServerCombatMode(OldMode);P->WaitAction.Empty();P->NextAction=0;
        }
        {
            const int32 OldMode=Session.PlayerVitals.CombatMode;Session.PendingCombatMode=0;Session.ApplyServerCombatMode(1);
            FACEWorldObject Stone;Stone.Guid=9100;Stone.ContainerId=Session.PlayerGuid;Stone.Name=TEXT("Mana Stone");Stone.ItemType=ACEItemType::ManaStone;Stone.ItemUseable=0x00100008;
            FACEWorldObject Donor;Donor.Guid=9101;Donor.ContainerId=Session.PlayerGuid;Donor.Name=TEXT("Donor");Donor.ItemType=ACEItemType::Jewelry;
            Session.WorldObjects.Add(Stone.Guid,Stone);Session.WorldObjects.Add(Donor.Guid,Donor);
            FACEAppraisalInfo StoneID;StoneID.bSuccess=true;H->Appraisals.Add(Stone.Guid,StoneID);
            FACEAppraisalInfo DonorID;DonorID.bSuccess=true;DonorID.IntProperties.Add(107,5000);DonorID.IntProperties.Add(108,5000);H->Appraisals.Add(Donor.Guid,DonorID);
            auto Use=MakeShared<FJsonObject>();Use->SetNumberField(TEXT("item"),Stone.Guid);Use->SetNumberField(TEXT("target"),Donor.Guid);Use->SetStringField(TEXT("activity"),TEXT("item_mana"));
            Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Use,TEXT("apply_item"));
            TestEqual(TEXT("Stone filling sends one normal network action"),Session.CachedC2SPackets.Num(),1);
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                TestEqual(TEXT("Stone filling uses retail UseWithTarget opcode"),R.ReadUInt32(),ACEGameAction::UseWithTarget);
                TestEqual(TEXT("Packet carries stone as source"),R.ReadUInt32(),uint32(Stone.Guid));
                TestEqual(TEXT("Packet carries donor as target"),R.ReadUInt32(),uint32(Donor.Guid));
            }
            TestTrue(TEXT("Source stone queued for fresh appraisal"),H->PendingResourceRefresh.Contains(Stone.Guid));
            H->ObserveUseDone(0);TestFalse(TEXT("Old stone contents discarded on completion"),H->Appraisals.Contains(Stone.Guid));
            TestFalse(TEXT("Old donor appraisal discarded on completion"),H->Appraisals.Contains(Donor.Guid));
            H->Appraisals.Add(Stone.Guid,StoneID);H->Appraisals.Add(Donor.Guid,DonorID);
            Session.WorldObjects[Donor.Guid].ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;
            Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Use,TEXT("apply_item"));
            TestEqual(TEXT("Stale intent cannot consume an item now retained"),Session.CachedC2SPackets.Num(),0);
            TestTrue(TEXT("Rejected refill leaves UCM running"),P->Running);
            H->Appraisals[Stone.Guid].IntProperties.Add(107,5000);Session.WorldObjects[Stone.Guid].ItemCurrentMana=0;
            H->ExecuteInventory(*P,Use,TEXT("apply_item"));
            TestEqual(TEXT("Live empty-stone update keeps donor retention protection despite older filled appraisal"),Session.CachedC2SPackets.Num(),0);
            Session.WorldObjects[Stone.Guid].ItemCurrentMana=-1;
            const bool WasGDLE=Session.Creds.bGDLE;
            FACEWorldObject Pack;Pack.Guid=9102;Pack.ContainerId=Session.PlayerGuid;Pack.ItemType=ACEItemType::Container;
            Session.WorldObjects.Add(Pack.Guid,Pack);
            Use->RemoveField(TEXT("target"));
            for(bool GDLE:{false,true})for(int Case=0;Case<4;++Case)
            {
                Session.Creds.bGDLE=GDLE;Session.TradeSelfItems.Reset();Session.CachedC2SPackets.Reset();
                auto& Live=Session.WorldObjects[Stone.Guid];Live.ContainerId=Case==3?Pack.Guid:Session.PlayerGuid;
                Live.ObjectDescriptionFlags=Case==1?ACEObjectDescFlag::Retained:0;
                if(Case==2)Session.TradeSelfItems.Add(Stone.Guid);
                if(Case==3)Session.TradeSelfItems.Add(Pack.Guid);
                const bool Available=Case<2;
                TestEqual(TEXT("Mana supply availability allows retention but excludes trade and traded packs"),H->PluginItemSnapshot(Live)->AsObject()->GetBoolField(TEXT("mana_supply_available")),Available);
                H->ExecuteInventory(*P,Use,TEXT("use_item"));
                TestEqual(TEXT("ACE and GDLE revalidate mana supply before sending use"),Session.CachedC2SPackets.Num(),Available?1:0);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                    TestEqual(TEXT("Equipment recharge uses targeted item action on both servers"),R.ReadUInt32(),ACEGameAction::UseWithTarget);
                    TestEqual(TEXT("Recharge source is the selected supply"),R.ReadUInt32(),uint32(Stone.Guid));
                    TestEqual(TEXT("Recharge target is the player, not the selected mob"),R.ReadUInt32(),uint32(Session.PlayerGuid));
                }
                H->CompletePendingAction(0);P->WaitAction.Empty();P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));
            }
            Session.Creds.bGDLE=WasGDLE;Session.TradeSelfItems.Reset();Session.WorldObjects.Remove(Pack.Guid);
            Session.WorldObjects.Remove(Stone.Guid);Session.WorldObjects.Remove(Donor.Guid);H->Appraisals.Remove(Stone.Guid);H->Appraisals.Remove(Donor.Guid);
            P->Profile->RemoveField(TEXT("ucm_activity_failure"));Session.PendingCombatMode=0;Session.ApplyServerCombatMode(OldMode);
        }
    }
    H->RefreshAutomationData();
    {
        const auto SavedObjects=Session.WorldObjects;const auto SavedAppraisals=H->Appraisals;
        const auto SavedRequests=H->AppraisalRequests;const auto SavedSelection=Session.SelectedObject;
        ON_SCOPE_EXIT {Session.WorldObjects=SavedObjects;H->Appraisals=SavedAppraisals;H->AppraisalRequests=SavedRequests;
            Session.SelectedObject=SavedSelection;H->InventoryRevision=MAX_uint64;};
        Session.WorldObjects.Empty();H->Appraisals.Empty();H->AppraisalRequests.Empty();
        FACEWorldObject Owner;Owner.Guid=Session.PlayerGuid;Owner.bIsPlayer=true;Session.WorldObjects.Add(Owner.Guid,Owner);
        FACEWorldObject Item;Item.Guid=9201;Item.ContainerId=Session.PlayerGuid;Item.ItemType=256;Item.Name=TEXT("Atlatl dart");
        Session.WorldObjects.Add(Item.Guid,Item);
        FACEAppraisalInfo Info;Info.ObjectGuid=Item.Guid;Info.bSuccess=true;Info.Damage=30;Info.DamageType=2;
        H->ObserveAppraisal(Info);
        for(int Shot=0;Shot<6;++Shot)
        {
            FACEBinaryWriter W;W.WriteUInt32(Item.Guid);W.WriteUInt32(ACEEquipMask::MissileAmmo);
            FACEBinaryReader WR(W.GetData());Session.HandleWieldItem(WR);
            FACEBinaryWriter Parent;Parent.WriteUInt32(Owner.Guid);Parent.WriteUInt32(Item.Guid);Parent.WriteUInt32(1);Parent.WriteUInt32(1);Parent.WriteUInt16(0);Parent.WriteUInt16(Shot*2+1);
            FACEBinaryReader PR(Parent.GetData());Session.HandleParentEvent(PR);
            TestTrue(TEXT("Reload/attachment retains assessed ammunition"),H->Appraisals.Contains(Item.Guid));
            FACEBinaryWriter Pickup;Pickup.WriteUInt32(Item.Guid);Pickup.WriteUInt16(0);Pickup.WriteUInt16(Shot*2+2);
            FACEBinaryReader RR(Pickup.GetData());Session.HandlePickupEvent(RR);
            TestTrue(TEXT("Shot removes only the visual, not the ammunition appraisal"),H->Appraisals.Contains(Item.Guid));
        }
        // Held weapon wield and appearance updates use the same cache.
        FACEBinaryWriter W;W.WriteUInt32(Item.Guid);W.WriteUInt32(ACEEquipMask::MissileWeapon);
        FACEBinaryReader WR(W.GetData());Session.HandleWieldItem(WR);
        TestTrue(TEXT("Switching the equipped weapon retains its appraisal"),H->Appraisals.Contains(Item.Guid));
        auto Visual=Session.WorldObjects[Item.Guid];Visual.bAppearanceOnlyUpdate=true;Session.OnObjectCreated.Broadcast(Visual);
        TestTrue(TEXT("Appearance-only changes retain owned item stats"),H->Appraisals.Contains(Item.Guid));
        H->AppraisalRequests.Add(Item.Guid,FPlatformTime::Seconds());
        Session.OnObjectCreated.Broadcast(Session.WorldObjects[Item.Guid]);
        TestFalse(TEXT("A real replacement description still invalidates appraisal"),H->Appraisals.Contains(Item.Guid));
        TestFalse(TEXT("Replacement data can be appraised immediately"),H->AppraisalRequests.Contains(Item.Guid));
        H->ObserveAppraisal(Info);Session.WorldObjects.Remove(Item.Guid);Session.OnObjectDeleted.Broadcast(Item.Guid);
        TestFalse(TEXT("Actual removal discards the item's appraisal"),H->Appraisals.Contains(Item.Guid));

        FACEWorldObject Player;Player.Guid=9202;Player.bIsPlayer=true;Player.ItemType=ACEItemType::Creature;
        Player.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;Session.WorldObjects.Add(Player.Guid,Player);
        Session.SelectedObject.Guid=Player.Guid;
        for(int Scan=0;Scan<3;++Scan)
        {
            H->NextAppraisalScan=0;H->LastAppraisalRequest=0;H->RefreshAutomationData();
            TestFalse(TEXT("Selecting a fellow player never schedules UCM appraisal"),H->AppraisalRequests.Contains(Player.Guid));
        }
        Session.WorldObjects[Player.Guid].bIsPlayer=false;
        H->NextAppraisalScan=0;H->LastAppraisalRequest=0;H->RefreshAutomationData();
        TestTrue(TEXT("Selected enemies still receive background appraisal"),H->AppraisalRequests.Contains(Player.Guid));
    }
    {
        const auto Objects=Session.WorldObjects;const auto Appraisals=H->Appraisals;
        const auto Requests=H->AppraisalRequests,Times=H->TargetAppraisalTimes;
        const auto Profile=P->Profile;const bool Running=P->Running;
        ON_SCOPE_EXIT {Session.WorldObjects=Objects;H->Appraisals=Appraisals;H->AppraisalRequests=Requests;
            H->TargetAppraisalTimes=Times;P->Profile=Profile;P->Running=Running;H->InventoryRevision=MAX_uint64;};
        Session.WorldObjects.Empty();H->Appraisals.Empty();H->AppraisalRequests.Empty();H->TargetAppraisalTimes.Empty();
        P->Running=false;P->Profile=MakeShared<FJsonObject>();P->Profile->SetBoolField(TEXT("mana_charges_when_off"),true);
        const double Now=FPlatformTime::Seconds();
        // More items than the old 120-second, one-request-per-second scan can
        // service. Worn jewelry comes after the generic inventory entries.
        for(int32 Id=9300;Id<9500;++Id)
        {
            FACEWorldObject O;O.Guid=Id;O.ItemType=ACEItemType::Armor;O.ContainerId=Session.PlayerGuid;
            Session.WorldObjects.Add(Id,O);FACEAppraisalInfo A;A.ObjectGuid=Id;A.bSuccess=true;H->Appraisals.Add(Id,A);
            H->AppraisalRequests.Add(Id,Now-200);H->TargetAppraisalTimes.Add(Id,Now-200);
        }
        for(int32 Id:{9500,9501})
        {
            FACEWorldObject O;O.Guid=Id;O.ItemType=ACEItemType::Jewelry;O.WielderId=Session.PlayerGuid;
            if(Id==9501){O.WielderId=0;O.ParentGuid=Session.PlayerGuid;O.CurrentWieldedLocation=1;}
            O.Name=Id==9501?TEXT("Encrusted Bloodstone Jewel"):TEXT("Full ring");Session.WorldObjects.Add(Id,O);
            FACEAppraisalInfo A;A.ObjectGuid=Id;A.bSuccess=true;A.IntProperties={{107,Id==9501?34:100},{108,100}};
            H->Appraisals.Add(Id,A);H->AppraisalRequests.Add(Id,Now-31);H->TargetAppraisalTimes.Add(Id,Now-31);
        }
        auto Scan=[&](){H->LastAppraisalRequest=FPlatformTime::Seconds()-2;H->NextAppraisalScan=0;H->RefreshAutomationData();};
        Scan();
        TestTrue(TEXT("Stopped UCM refreshes the lowest worn mana before a full inventory scan"),H->AppraisalRequests.FindRef(9501)>=Now);
        TestEqual(TEXT("General inventory does not displace the jewel refresh"),H->AppraisalRequests.FindRef(9300),Now-200);
        const double Sent=H->AppraisalRequests.FindRef(9501);H->RefreshAutomationData();
        TestEqual(TEXT("Recharge polling preserves the one-request-per-second budget"),H->AppraisalRequests.FindRef(9501),Sent);
        auto Low=H->Appraisals.FindChecked(9501);Low.IntProperties[107]=32;H->ObserveAppraisal(Low);
        TestEqual(TEXT("Fresh jewel mana reaches the policy"),H->PluginItemSnapshot(Session.WorldObjects[9501])->AsObject()->GetNumberField(TEXT("mana")),32.);
        H->PendingManaRefresh=true;H->PendingResourceRefresh.Empty();H->CompletePendingAction(0);
        TestTrue(TEXT("Mana use preserves equipped item appraisal while refreshing resources"),H->Appraisals.Contains(9501));
        Scan();
        TestTrue(TEXT("Post-use confirmation starts with the lowest-mana item"),H->AppraisalRequests.Contains(9501)&&!H->AppraisalRequests.Contains(9500));
        Low.IntProperties[107]=100;H->ObserveAppraisal(Low);
        TestEqual(TEXT("Confirmed recharge reaches the next decision"),H->PluginItemSnapshot(Session.WorldObjects[9501])->AsObject()->GetNumberField(TEXT("mana")),100.);
        FACEWorldObject Charge;Charge.Guid=9502;Charge.ItemType=ACEItemType::ManaStone;Charge.ContainerId=Session.PlayerGuid;
        Charge.Name=TEXT("Titan Mana Charge");Session.WorldObjects.Add(Charge.Guid,Charge);
        FACEAppraisalInfo ChargeInfo;ChargeInfo.ObjectGuid=Charge.Guid;ChargeInfo.bSuccess=true;ChargeInfo.IntProperties={{107,5000},{108,5000}};
        H->ObserveAppraisal(ChargeInfo);
        H->ObserveItemUse(Charge.Guid,Session.PlayerGuid);
        TestTrue(TEXT("Manual charge use requests the same mana refresh as UCM"),H->PendingManaRefresh&&H->PendingResourceRefresh.Contains(Charge.Guid));
        H->CompletePendingAction(0);
        TestFalse(TEXT("Manual charge cannot reuse stale contents after success"),H->Appraisals.Contains(Charge.Guid));
        TestTrue(TEXT("Manual charge refresh preserves the jewel's equipment appraisal"),H->Appraisals.Contains(9501));
        // GDLE's public mana quality must reach the same snapshot immediately,
        // without waiting for the appraisal lane to revisit the item.
        for(const auto& Entry:TArray<TPair<uint32,int32>>{{107,25},{108,200},{107,0}})
        {
            FACEBinaryWriter W;W.WriteUInt32(ACEOpcode::PublicUpdatePropertyInt);W.WriteUInt8(uint8(Entry.Value+1));
            W.WriteUInt32(9501);W.WriteUInt32(Entry.Key);W.WriteInt32(Entry.Value);
            // Direct handler isolates resource mapping; network stamp/order
            // validation has separate LivePropertyUpdates coverage.
            FACEBinaryReader R(W.GetData());R.ReadUInt32();Session.HandlePublicUpdatePropertyInt(R);
            const auto Item=H->PluginItemSnapshot(Session.WorldObjects[9501])->AsObject();
            TestEqual(TEXT("Live mana quality supersedes the older appraisal"),Item->GetNumberField(Entry.Key==107?TEXT("mana"):TEXT("max_mana")),double(Entry.Value));
            TestEqual(TEXT("Lua property map agrees with resource fields"),Item->GetObjectField(TEXT("int_properties"))->GetNumberField(FString::FromInt(Entry.Key)),double(Entry.Value));
        }
        Session.WorldObjects[9501].ItemCurrentMana=-2;
        const auto Removed=H->PluginItemSnapshot(Session.WorldObjects[9501])->AsObject();
        TestEqual(TEXT("Removed mana is empty for maintenance"),Removed->GetNumberField(TEXT("mana")),0.);
        TestFalse(TEXT("Removed mana does not fabricate an existing integer property for loot rules"),Removed->GetObjectField(TEXT("int_properties"))->HasField(TEXT("107")));
        Session.WorldObjects.Remove(Charge.Guid);H->AppraisalRequests.Remove(Charge.Guid);
        P->Profile->SetBoolField(TEXT("mana_charges_when_off"),false);
        H->AppraisalRequests.Add(9300,Now-121);H->AppraisalRequests.Add(9499,Now-400);
        // Keep worn objects recent so this checks fairness of the general lane.
        H->AppraisalRequests.Add(9500,Now);H->AppraisalRequests.Add(9501,Now);
        Scan();TestTrue(TEXT("Oldest inventory appraisal cannot starve behind repeated earlier entries"),H->AppraisalRequests.FindRef(9499)>=Now);
    }
    {
        TestTrue(TEXT("Native vitals view uses session storage"),&C->GetPlayerVitalsView()==&Session.GetPlayerVitals());
        H->DrawRoute();TestNull(TEXT("No route does not allocate a world actor"),H->RouteActor.Get());
        // A fully appraised inventory previously rescanned at render frequency.
        // Force the deadline only for the comparison; both paths execute the
        // real maintenance code over identical world data.
        for(int32 I=0;I<2000;++I)
        {FACEWorldObject O;O.Guid=0x60000000+I;O.ContainerId=Session.PlayerGuid;O.ItemType=ACEItemType::Money;Session.WorldObjects.Add(O.Guid,O);}
        H->LastAppraisalRequest=0;H->NextAppraisalScan=0;H->RefreshAutomationData();
        const double Deadline=H->NextAppraisalScan;
        double Begin=FPlatformTime::Seconds();for(int32 I=0;I<200;++I)H->RefreshAutomationData();
        const double Cached=(FPlatformTime::Seconds()-Begin)*1e6/200;
        TestEqual(TEXT("Empty maintenance scan keeps its deadline across frames"),H->NextAppraisalScan,Deadline);
        Begin=FPlatformTime::Seconds();for(int32 I=0;I<200;++I){H->NextAppraisalScan=0;H->RefreshAutomationData();}
        const double Repeated=(FPlatformTime::Seconds()-Begin)*1e6/200;
        AddInfo(FString::Printf(TEXT("Appraisal maintenance, 2000 objects: forced scan %.2f us/call, scheduled %.2f us/call (CPU microbenchmark, not FPS)"),Repeated,Cached));
        for(int32 I=0;I<2000;++I)Session.WorldObjects.Remove(0x60000000+I);
        H->NextAppraisalScan=0;
    }
    {
        const auto OriginalProfile=P->Profile,OriginalLinks=P->ProfileLinks,OriginalBaseline=P->ProfileBaseline;
        const FString OriginalName=P->ProfileName;const auto Vitals=Session.PlayerVitals;
        Session.PlayerVitals.bValid=true;Session.PlayerVitals.Health=100;
        TestTrue(TEXT("Meta setup is persisted with shared sections"),H->SaveProfile(TEXT("ucm"),TEXT("ResumeTest"),TEXT("{\"vt_meta\":[]}")));
        TestTrue(TEXT("Meta host starts"),H->Start(TEXT("ucm")));auto First=P->VM;
        H->Stop(TEXT("ucm"),TEXT("Stopped"),true);
        TestTrue(TEXT("Manual stop retains only resumable meta VM"),!P->Running&&P->CanResumeMeta&&P->VM==First);
        TestTrue(TEXT("Same character/profile resumes"),H->Start(TEXT("ucm")));
        TestTrue(TEXT("Resume uses retained VM and arms one resume callback"),P->VM==First&&P->ResumeMetaPending);
        H->Stop(TEXT("ucm"),TEXT("Paused before Force Buff"),true);
        TestTrue(TEXT("Force Buff can run from a paused meta"),H->RequestForceBuff());
        TestTrue(TEXT("Manual buff keeps the paused meta VM"),P->VM==First&&H->bForceBuffOnly);
        H->CancelForceBuff();
        TestTrue(TEXT("Cancelling manual buff preserves resumable meta"),!P->Running&&P->CanResumeMeta&&P->VM==First);
        H->RequestForceBuff();auto Done=MakeShared<FJsonObject>();Done->SetStringField(TEXT("action"),TEXT("force_buff_done"));
        Done->SetNumberField(TEXT("request"),H->ForceBuffRequest);H->Execute(*P,Done);
        TestTrue(TEXT("Completing manual buff preserves resumable meta"),!P->Running&&P->CanResumeMeta&&P->VM==First);
        H->Start(TEXT("ucm"));
        H->Stop(TEXT("ucm"),TEXT("Stopped"),true);P->Profile->SetNumberField(TEXT("radius"),77);
        TestTrue(TEXT("Changed settings are persisted to the linked Character file"),H->SaveProfile(P->Id,P->ProfileName,H->ProfileJson(P->Id)));
        H->Start(TEXT("ucm"));TestTrue(TEXT("Changed settings cannot resume stale meta state"),P->VM!=First&&!P->ResumeMetaPending);
        First=P->VM;H->Stop(TEXT("ucm"),TEXT("Stopped"),true);++Session.PlayerGuid;
        H->Start(TEXT("ucm"));TestTrue(TEXT("Another character cannot inherit a paused meta"),P->VM!=First&&!P->ResumeMetaPending);--Session.PlayerGuid;
        H->Stop(TEXT("ucm"),TEXT("Script error fixture"));TestTrue(TEXT("Errors discard the VM rather than repeatedly resuming broken state"),!P->VM&&!P->CanResumeMeta);
        P->Profile=OriginalProfile;P->ProfileLinks=OriginalLinks;P->ProfileBaseline=OriginalBaseline;P->ProfileName=OriginalName;
        P->Running=true;Session.PlayerVitals=Vitals;H->RefreshAutomationData();
    }
    {
        FACEWorldObject Monster;Monster.Guid=900;Monster.Name=TEXT("Wasp");Session.WorldObjects.Add(900,Monster);
        Session.SelectedObject.Guid=900;H->PhysicalAttackTarget=900;H->PhysicalAttackOwner=TEXT("ucm");H->PhysicalAttackUntil=FPlatformTime::Seconds()+30;
        Session.OnChatMessage.Broadcast(TEXT("Your missile attack hit the environment."),TEXT("Visitor"),0);
        Session.OnChatMessage.Broadcast(TEXT("Your missile attack hit the environment."),TEXT(""),ACEChatMessageType::Tell);
        TestEqual(TEXT("Player chat cannot fabricate obstruction failures"),H->CombatOutcomes.Num(),0);
        Session.OnChatMessage.Broadcast(TEXT("Your missile attack hit the environment."),TEXT(""),0);
        TestEqual(TEXT("Server environment miss records selected physical target"),H->CombatOutcomes.Num(),1);
        if(H->CombatOutcomes.Num())TestEqual(TEXT("Failure carries target GUID"),H->CombatOutcomes[0]->AsObject()->GetNumberField(TEXT("target")),900.);
        Session.OnCombatFeedback.Broadcast(TEXT("Wasp"),0,false,false);Session.OnCombatFeedback.Broadcast(TEXT("Wasp"),20,true,false);Session.OnCombatFeedback.Broadcast(TEXT("Other"),20,false,false);
        TestEqual(TEXT("Evades, incoming damage and other targets are not hits"),H->CombatOutcomes.Num(),1);
        Session.OnCombatFeedback.Broadcast(TEXT("Wasp"),20,false,false);TestEqual(TEXT("Confirmed damage resets target failures"),H->CombatOutcomes.Num(),2);
        Session.SelectedObject.Guid=901;Session.OnChatMessage.Broadcast(TEXT("Your missile attack hit the environment."),TEXT(""),0);
        TestEqual(TEXT("Changed selection rejects ambiguous late outcome"),H->CombatOutcomes.Num(),2);
        Session.SelectedObject.Guid=900;H->PhysicalAttackUntil=0;Session.OnChatMessage.Broadcast(TEXT("Your missile attack hit the environment."),TEXT(""),0);
        TestEqual(TEXT("Expired physical attack ignores late feedback"),H->CombatOutcomes.Num(),2);
        H->PhysicalAttackTarget=0;H->PhysicalAttackOwner.Empty();H->CombatOutcomes.Empty();Session.SelectedObject.Guid=0;Session.WorldObjects.Remove(900);
    }
    {
        FACEWorldObject Monster;Monster.Guid=900;Monster.Name=TEXT("Wasp");Session.WorldObjects.Add(900,Monster);
        FACEWorldObject Player;Player.Guid=Session.PlayerGuid;Player.Name=TEXT("Caster");Session.WorldObjects.Add(Player.Guid,Player);
        Session.SelectedObject.Guid=900;H->PendingSpell=123;
        auto Cast=[&](bool Started=true)
        {
            UACEPluginSubsystem::FOffensiveCast E;E.Spell=123;E.Target=900;E.Owner=TEXT("ucm");E.SpellName=TEXT("Flame Bolt VII");E.TargetName=TEXT("Wasp");
            E.Sent=FPlatformTime::Seconds();E.Started=Started?E.Sent:0;H->OffensiveCasts.Add(E);
            H->PendingSpell=E.Spell;H->PendingSpellTarget=E.Target;return E.Sent;
        };
        const double Unstarted=Cast(false);H->ExpireOffensiveCasts(Unstarted+6);
        TestTrue(TEXT("No spell words means no target failure on turn/equip timeout"),H->OffensiveCasts.IsEmpty()&&H->CombatOutcomes.IsEmpty());
        Cast(false);Session.OnChatMessage.Broadcast(TEXT("words"),TEXT("Other"),ACEChatMessageType::Spellcasting);
        TestEqual(TEXT("Another caster cannot arm result timeout"),H->OffensiveCasts[0].Started,0.);
        Session.OnChatMessage.Broadcast(TEXT("words"),TEXT("Caster"),ACEChatMessageType::Spellcasting);
        TestTrue(TEXT("Own spell words arm result deadline"),H->OffensiveCasts[0].Started>0);
        H->ExpireOffensiveCasts(H->OffensiveCasts[0].Started+3.629);
        TestEqual(TEXT("Started spell timeout counts once"),H->CombatOutcomes.Num(),1);
        H->ExpireOffensiveCasts(FPlatformTime::Seconds()+50);TestEqual(TEXT("Expired result is not counted again"),H->CombatOutcomes.Num(),1);
        Cast();Session.OnChatMessage.Broadcast(TEXT("Wasp resists your spell"),TEXT(""),ACEChatMessageType::Magic);
        Cast();Session.OnChatMessage.Broadcast(TEXT("Your spell fizzled."),TEXT(""),ACEChatMessageType::Magic);
        TestTrue(TEXT("Resists and fizzles discard pending result without blacklisting"),H->OffensiveCasts.IsEmpty()&&H->CombatOutcomes.Num()==1);
        Cast();H->ObserveUseDone(1);TestTrue(TEXT("UseDone error suppresses a later false timeout"),H->OffensiveCasts.IsEmpty());
        Cast();Session.OnChatMessage.Broadcast(TEXT("Target is out of range"),TEXT("Visitor"),ACEChatMessageType::Magic);
        TestEqual(TEXT("Player text cannot blacklist target"),H->CombatOutcomes.Num(),1);
        Session.OnChatMessage.Broadcast(TEXT("Target is out of range"),TEXT(""),ACEChatMessageType::Magic);
        TestTrue(TEXT("Permanent server failure marks target immediately unhittable"),H->CombatOutcomes.Last()->AsObject()->GetBoolField(TEXT("unhittable")));
        Cast();Session.OnChatMessage.Broadcast(TEXT("Critical hit! You burn Wasp for 100 points with Flame Bolt VII."),TEXT(""),ACEChatMessageType::Magic);
        TestTrue(TEXT("Magic damage success resets miss count after UseDone"),H->OffensiveCasts.IsEmpty()&&H->CombatOutcomes.Last()->AsObject()->GetBoolField(TEXT("hit")));
        TestEqual(TEXT("Correlated spell hit retains actual damage for VT streak finishing"),H->CombatOutcomes.Last()->AsObject()->GetNumberField(TEXT("damage")),100.);
        TestTrue(TEXT("Damage outcome identifies its spell rather than a generic use completion"),H->CombatOutcomes.Last()->AsObject()->GetNumberField(TEXT("spell"))>0);
        Cast();Session.WorldObjects.Remove(900);H->ExpireOffensiveCasts(FPlatformTime::Seconds()+10);
        TestTrue(TEXT("Released target cannot acquire a timeout for a reused GUID"),H->OffensiveCasts.IsEmpty());
        H->CombatOutcomes.Empty();H->PendingSpell=H->PendingSpellTarget=0;Session.SelectedObject.Guid=0;Session.WorldObjects.Remove(Player.Guid);
    }
    for(uint32 CastError:{0u,1u})
    {
        const double Sent=FPlatformTime::Seconds()-1.;
        H->PendingSpell=123;H->PendingSpellTarget=0;H->PendingSpellAt=Sent;H->PendingSpellOwner=P->Id;
        P->NextAction=Sent+3.5;H->ObserveUseDone(CastError);
        TestTrue(TEXT("Completed cast releases immediately; rejected cast retains minimum request interval"),P->NextAction<=(CastError?Sent+.5:FPlatformTime::Seconds()));
        TestTrue(TEXT("Cast completion clears owner and pending spell"),H->PendingSpellOwner.IsEmpty()&&H->PendingSpell==0);
        P->NextAction=Sent+8;H->ObserveUseDone(0);
        TestEqual(TEXT("Unrelated use completion cannot clear another action throttle"),P->NextAction,Sent+8);
        P->NextAction=0;
    }
    {
        const auto SavedPlugin=*P;
        const auto SavedVitals=Session.PlayerVitals;
        const bool SavedUseBusy=Session.bUseBusy;
        const int32 SavedEquipment=Session.PendingEquipmentGuid;
        const double SavedMaintenance=H->NextMaintenanceAt;
        ON_SCOPE_EXIT
        {
            FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);
            H->DecisionWakeTicker.Reset();H->DecisionWakeAt=0;
            H->NextMaintenanceAt=SavedMaintenance;*P=SavedPlugin;Session.PlayerVitals=SavedVitals;
            Session.bUseBusy=SavedUseBusy;Session.PendingEquipmentGuid=SavedEquipment;
            H->PendingSpell=0;H->PendingSpellOwner.Empty();
        };
        FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);H->DecisionWakeTicker.Reset();
        P->VM=MakeShared<FACEPluginVM>();FString Error;
        TestTrue(TEXT("Load cast scheduling probe"),P->VM->Load(TEXT("local n=0; return function(s,p) n=n+1; return {status='Decision '..n} end"),Error));
        P->Profile=MakeShared<FJsonObject>();P->Running=true;P->Player=Session.PlayerGuid;P->Server=C->GetServerName();
        Session.PlayerVitals.bValid=true;Session.PlayerVitals.Health=100;
        Session.bUseBusy=false;Session.PendingEquipmentGuid=0;
        P->WaitAction.Empty();P->NextDecision=0;P->NextAction=0;P->Status=TEXT("Waiting");
        H->NextMaintenanceAt=FPlatformTime::Seconds()+60;
        const double Maintenance=H->NextMaintenanceAt;
        auto CompleteCast=[&](uint32 Result)
        {
            FACEBinaryWriter Packet;Packet.WriteUInt32(Result);FACEBinaryReader Reader(Packet.GetData());
            Session.HandleUseDone(Reader);
        };
        auto DeliverWake=[&]()
        {
            // Invoke the registered callback without recursively ticking the global ticker.
            FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);
            H->RunDecisionWake(.016f);
        };
        for(int32 Cast=1;Cast<=20;++Cast)
        {
            H->PendingSpell=123;H->PendingSpellAt=FPlatformTime::Seconds();H->PendingSpellOwner=P->Id;
            H->PendingSpellConfirmed=false;H->PendingSpellFizzled=false;
            P->NextAction=H->PendingSpellAt+3.5;P->NextDecision=0;
            const FString Before=P->Status;H->Tick(.016f);
            TestEqual(TEXT("Pending spell prevents another decision"),P->Status,Before);
            CompleteCast(0);
            TestTrue(TEXT("Server completion schedules immediate deferred decision"),H->DecisionWakeTicker.IsValid()&&H->DecisionWakeAt<=FPlatformTime::Seconds());
            TestEqual(TEXT("Network callback does not execute policy reentrantly"),P->Status,Before);
            TestFalse(TEXT("Projectile still in flight is not falsely confirmed"),H->LastSpellConfirmed);
            const double WakeAt=H->DecisionWakeAt;H->ScheduleDecisionWake(WakeAt+1);
            TestEqual(TEXT("Later wake cannot delay an already ready cast"),H->DecisionWakeAt,WakeAt);
            DeliverWake();
            TestEqual(TEXT("Each acknowledged cast gets the next decision"),P->Status,FString::Printf(TEXT("Decision %d"),Cast));
            H->Tick(.016f);
            TestEqual(TEXT("Periodic poll cannot duplicate the same decision"),P->Status,FString::Printf(TEXT("Decision %d"),Cast));
            TestEqual(TEXT("Rapid cast completions do not repeat background scans"),H->NextMaintenanceAt,Maintenance);
        }
        FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);H->DecisionWakeTicker.Reset();
        H->PendingSpell=123;H->PendingSpellOwner=P->Id;H->PendingSpellAt=FPlatformTime::Seconds();
        P->NextAction=H->PendingSpellAt+3.5;CompleteCast(1);
        TestTrue(TEXT("Immediate rejection retains half-second retry floor"),H->DecisionWakeAt>=H->PendingSpellAt+.5);
        DeliverWake();TestEqual(TEXT("Early retry cannot execute another decision"),P->Status,FString(TEXT("Decision 20")));
        TestTrue(TEXT("Early event wake is retained at the action deadline instead of waiting for the half-second poll"),
            H->DecisionWakeTicker.IsValid()&&H->DecisionWakeAt==P->NextAction);
        P->NextAction=0;P->NextDecision=FPlatformTime::Seconds()+.1;
        const double DecisionDeadline=P->NextDecision;DeliverWake();
        TestTrue(TEXT("Early wake also survives a decision gate"),H->DecisionWakeTicker.IsValid()&&H->DecisionWakeAt==DecisionDeadline);
        TestEqual(TEXT("Early decision wake does not duplicate policy work"),P->Status,FString(TEXT("Decision 20")));
        P->NextDecision=0;DeliverWake();
        TestEqual(TEXT("Deferred wake runs as soon as its gate clears"),P->Status,FString(TEXT("Decision 21")));
        H->PendingSpell=123;H->PendingSpellOwner=P->Id;CompleteCast(0);
        P->Running=false;DeliverWake();
        TestEqual(TEXT("Stopping before a queued wake prevents another decision"),P->Status,FString(TEXT("Decision 21")));
    }
    auto Tell=[&](const FString& Text,int32 Sender=0x50000002,int32 Type=ACEChatMessageType::Tell)
    {
        FACEBinaryWriter W;W.WriteString16L(Text);W.WriteString16L(TEXT("Visitor"));W.WriteUInt32(Sender);
        W.WriteUInt32(Session.PlayerGuid);W.WriteUInt32(Type);W.WriteUInt32(0);
        FACEBinaryReader R(W.GetData());Session.HandleTell(R);
    };
    {
        auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        FSocket* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("UCM queue receiver"),false);
        auto Address=Sockets->CreateInternetAddr();bool Valid=false;Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
        if(!TestTrue(TEXT("Queue test binds only to loopback"),Receiver&&Receiver->Bind(*Address)))return false;
        Receiver->GetAddress(*Address);Session.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("UCM queue sender"),false);
        Session.ServerC2SAddr=Address;Session.IssacClient=MakeUnique<FACEIsaac>(123u);
        ON_SCOPE_EXIT{H->ClearBuffRequests();Session.SocketC2S->Close();Sockets->DestroySocket(Session.SocketC2S);Session.SocketC2S=nullptr;Session.ServerC2SAddr.Reset();Session.IssacClient.Reset();Receiver->Close();Sockets->DestroySocket(Receiver);Session.CachedC2SPackets.Reset();};
        auto HasTell=[&](const FString& Name,const FString& Part){for(const auto& Packet:Session.CachedC2SPackets)
        {FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()!=ACEGameAction::Tell)continue;const auto Message=R.ReadString16L(),Recipient=R.ReadString16L();if(Recipient==Name&&Message.Contains(Part))return true;}return false;};
        H->ObservePlayerTell(TEXT("heavy"),TEXT("Alice"),0x50000020);
        H->ObservePlayerTell(TEXT("mage"),TEXT("Bob"),0x50000021);
        TestTrue(TEXT("Second requester receives direct queue position"),HasTell(TEXT("Bob"),TEXT("#2 (1 ahead")));
        TestTrue(TEXT("Host can see queued names and order"),H->BuffQueueSummary().Contains(TEXT("2. Bob")));
        auto Start=MakeShared<FJsonObject>();Start->SetStringField(TEXT("action"),TEXT("buff_request_start"));Start->SetNumberField(TEXT("request"),H->BuffRequests[0]->AsObject()->GetNumberField(TEXT("id")));
        Session.CachedC2SPackets.Reset();H->Execute(*P,Start);TestTrue(TEXT("First requester gets turn notification"),HasTell(TEXT("Alice"),TEXT("your turn")));
        Session.CachedC2SPackets.Reset();H->Execute(*P,Start);TestTrue(TEXT("Repeated start cannot spam turn notifications"),Session.CachedC2SPackets.IsEmpty());
        auto Done=MakeShared<FJsonObject>(*Start);Done->SetStringField(TEXT("action"),TEXT("buff_request_done"));Done->SetStringField(TEXT("status"),TEXT("Buffs complete"));H->Execute(*P,Done);
        TestTrue(TEXT("Finished player gets a completion tell"),HasTell(TEXT("Alice"),TEXT("Buffs complete")));
        TestTrue(TEXT("Remaining player gets updated position"),HasTell(TEXT("Bob"),TEXT("#1")));
        Start->SetNumberField(TEXT("request"),H->BuffRequests[0]->AsObject()->GetNumberField(TEXT("id")));Session.CachedC2SPackets.Reset();H->Execute(*P,Start);
        TestTrue(TEXT("Next player is notified when their buff task starts"),HasTell(TEXT("Bob"),TEXT("your turn")));
        H->ClearBuffRequests();TestTrue(TEXT("Clear sends cancellation to the remaining requester"),HasTell(TEXT("Bob"),TEXT("cancelled")));
        FACEWorldObject Blade;Blade.Guid=9300;Blade.Name=TEXT("Tethered blade");Blade.ContainerId=Session.PlayerGuid;
        Blade.ItemType=ACEItemType::MeleeWeapon;Blade.ValidLocations=ACEEquipMask::MeleeWeapon;Blade.CombatUse=1;
        Session.WorldObjects.Add(Blade.Guid,Blade);FACEAppraisalInfo BladeInfo;BladeInfo.bSuccess=true;BladeInfo.IntProperties.Add(105,7);H->Appraisals.Add(Blade.Guid,BladeInfo);
        auto Equip=MakeShared<FJsonObject>();Equip->SetNumberField(TEXT("item"),Blade.Guid);
        for(bool Left:{true,false})
        {
            const uint64 Revision=Session.GetInventoryDataRevision();
            FACEBinaryWriter Update;Update.WriteUInt32(ACEOpcode::PublicUpdatePropertyBool);Update.WriteUInt8(Left?1:2);Update.WriteUInt32(Blade.Guid);Update.WriteUInt32(130);Update.WriteUInt32(Left?1:0);
            Session.HandleGameMessage(Update.GetData());
            TestTrue(TEXT("Tether property updates invalidate cached inventory"),Session.GetInventoryDataRevision()>Revision);
            TestTrue(TEXT("Ordinary inventory use honors live automatic hand flag"),ACEInventoryRules::DetermineOwnedUse(Session.WorldObjects[Blade.Guid],Session.PlayerGuid)==(Left?EACEOwnedItemUse::WieldLeft:EACEOwnedItemUse::WieldRight));
            auto Snapshot=MakeShared<FJsonObject>();H->ExtendSnapshot(Snapshot);
            bool Found=false;for(const auto& V:Snapshot->GetArrayField(TEXT("inventory")))if(V->AsObject()->GetNumberField(TEXT("id"))==Blade.Guid)
            {Found=true;TestEqual(TEXT("UCM sees tether changes without relog or appraisal"),V->AsObject()->GetBoolField(TEXT("auto_wield_left")),Left);}
            TestTrue(TEXT("Tethered weapon is in inventory snapshot"),Found);
            H->Appraisals.Add(Blade.Guid,BladeInfo); // changed inventory qualities require reassessment
            Session.PendingCombatMode=0;Session.ApplyServerCombatMode(1);Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Equip,TEXT("equip"));
            bool Sent=false;for(const auto& Packet:Session.CachedC2SPackets)
            {FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()!=ACEGameAction::GetAndWieldItem)continue;Sent=true;TestEqual(TEXT("Equip packet identifies blade"),R.ReadUInt32(),uint32(Blade.Guid));TestEqual(TEXT("Equip packet uses retail right/left location"),R.ReadUInt32(),uint32(Left?ACEEquipMask::Shield:ACEEquipMask::MeleeWeapon));}
            TestTrue(TEXT("UCM sends standard equipment request"),Sent);Session.CancelEquipmentSwap();
        }
        FACEWorldObject Ust;Ust.Guid=9301;Ust.ContainerId=Session.PlayerGuid;Ust.ItemType=0x20000000;Session.WorldObjects.Add(Ust.Guid,Ust);
        auto Salvage=MakeShared<FJsonObject>();Salvage->SetNumberField(TEXT("item"),Blade.Guid);Salvage->SetNumberField(TEXT("tool"),Ust.Guid);
        for(int Protection=0;Protection<5;++Protection)
        {
            auto& Live=Session.WorldObjects[Blade.Guid];Live=Blade;Live.MaterialType=59;Live.Structure=200;
            H->Appraisals.Add(Blade.Guid,BladeInfo); // equip invalidates the previous appraisal
            if(Protection==1)H->Appraisals[Blade.Guid].IntProperties.Add(171,1);
            if(Protection==2)H->Appraisals[Blade.Guid].StringProperties.Add(8,TEXT("Owner"));
            if(Protection==3)Live.ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;
            if(Protection==4)H->Appraisals[Blade.Guid].IntProperties.Remove(105);
            Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Salvage,TEXT("salvage"));
            bool Sent=false;for(const auto& Packet:Session.CachedC2SPackets)
            {FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()==ACEGameAction::CreateTinkeringTool)Sent=true;}
            TestEqual(TEXT("Host validates protected salvage and permits ordinary items with over 100 uses"),Sent,Protection==0);
        }
        {
            const auto SavedPlugin=*P;const auto SavedVitals=Session.PlayerVitals;
            const auto SavedBlade=Session.WorldObjects[Blade.Guid];const auto SavedAppraisal=H->Appraisals[Blade.Guid];
            const double SavedMaintenance=H->NextMaintenanceAt;
            ON_SCOPE_EXIT
            {
                FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);H->DecisionWakeTicker.Reset();H->DecisionWakeAt=0;
                *P=SavedPlugin;Session.PlayerVitals=SavedVitals;H->NextMaintenanceAt=SavedMaintenance;
                Session.WorldObjects.Add(Blade.Guid,SavedBlade);H->Appraisals.Add(Blade.Guid,SavedAppraisal);
                Session.WorldObjects.Remove(9303);Session.bUseBusy=false;
            };
            P->VM=MakeShared<FACEPluginVM>();FString Error;
            TestTrue(TEXT("Load salvage scheduling probe"),P->VM->Load(TEXT("local n=0; return function(s,p) n=n+1; return {status='Salvage decision '..n} end"),Error));
            P->Profile=MakeShared<FJsonObject>();P->ActivityFailure.Reset();P->Running=true;
            P->Player=Session.PlayerGuid;P->Server=C->GetServerName();
            Session.PlayerVitals.bValid=true;Session.PlayerVitals.Health=100;Session.bUseBusy=false;
            H->NextMaintenanceAt=FPlatformTime::Seconds()+60;
            const double Maintenance=H->NextMaintenanceAt;
            auto Remove=[&](int32 Id)
            {
                FACEBinaryWriter W;W.WriteUInt32(Id);FACEBinaryReader R(W.GetData());Session.HandleInventoryRemoveObject(R);
            };
            auto DeliverWake=[&]()
            {
                FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);H->RunDecisionWake(.016f);
            };
            int32 Decisions=0;
            for(bool Batch:{false,true})
            {
                FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);H->DecisionWakeTicker.Reset();H->DecisionWakeAt=0;
                P->NextAction=0;P->NextDecision=0;P->Status=TEXT("Waiting for salvage");
                auto Input=Blade;Input.MaterialType=59;
                if(Batch){Input.ItemType=0x40000000;Input.Structure=20;}
                Session.WorldObjects.Add(Input.Guid,Input);H->Appraisals.Add(Input.Guid,BladeInfo);
                auto Second=Input;Second.Guid=9303;Session.WorldObjects.Add(Second.Guid,Second);
                auto Intent=MakeShared<FJsonObject>(*Salvage);
                Intent->SetStringField(TEXT("items"),FString::Printf(TEXT("%u,9303"),uint32(Input.Guid)));
                Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Intent,Batch?TEXT("combine_salvage"):TEXT("salvage"));
                TestEqual(TEXT("Valid salvage sends exactly one request"),Session.CachedC2SPackets.Num(),1);
                const double Deadline=P->NextAction;
                TestTrue(TEXT("Unconfirmed salvage retains its fallback throttle"),Deadline>=P->ActionSentAt+2.9);
                H->RefreshActionWait(*P);TestEqual(TEXT("Still-present inputs cannot acknowledge salvage"),P->NextAction,Deadline);
                // This is also broadcast when an owned object's world visual is
                // removed: it must not be mistaken for consuming inventory.
                Session.OnObjectDeleted.Broadcast(Input.Guid);Remove(999999);
                FACEBinaryWriter UseDone;UseDone.WriteUInt32(0);FACEBinaryReader Reader(UseDone.GetData());Session.HandleUseDone(Reader);
                TestEqual(TEXT("Visual deletion, unrelated removal and UseDone do not complete salvage"),P->NextAction,Deadline);
                H->Tick(.016f);
                TestEqual(TEXT("Pending salvage does not run policy again"),P->Status,FString(TEXT("Waiting for salvage")));
                if(Batch)
                {
                    Remove(Input.Guid);
                    TestEqual(TEXT("Partial batch consumption cannot release the wait"),P->NextAction,Deadline);
                    Remove(Second.Guid);
                }
                else Remove(Input.Guid);
                TestTrue(TEXT("Confirmed consumption releases the three-second throttle immediately"),P->NextAction<=FPlatformTime::Seconds());
                TestTrue(TEXT("Consumption schedules a deferred decision without salvage-result chat"),H->DecisionWakeTicker.IsValid()&&H->DecisionWakeAt<=FPlatformTime::Seconds());
                TestEqual(TEXT("Inventory callback does not execute Lua reentrantly"),P->Status,FString(TEXT("Waiting for salvage")));
                Session.bUseBusy=true;DeliverWake();
                TestEqual(TEXT("Confirmation still respects another pending use"),P->Status,FString(TEXT("Waiting for salvage")));
                Session.bUseBusy=false;H->Tick(.016f);++Decisions;
                TestEqual(TEXT("Next decision runs without a fixed post-salvage pause"),P->Status,FString::Printf(TEXT("Salvage decision %d"),Decisions));
                TestEqual(TEXT("Repeated wait checks do not resubmit salvage"),Session.CachedC2SPackets.Num(),1);
                TestEqual(TEXT("Fast completions do not repeat background scans"),H->NextMaintenanceAt,Maintenance);
            }
            Session.WorldObjects.Add(Blade.Guid,Blade);Session.WorldObjects[Blade.Guid].MaterialType=59;H->Appraisals.Add(Blade.Guid,BladeInfo);
            H->ExecuteInventory(*P,Salvage,TEXT("salvage"));Remove(Blade.Guid);
            P->Running=false;DeliverWake();
            TestEqual(TEXT("Stop before a salvage wake prevents further decisions"),P->Status,FString::Printf(TEXT("Salvage decision %d"),Decisions));
        }
        FACEWorldObject ToolPack;ToolPack.Guid=9302;ToolPack.ContainerId=Session.PlayerGuid;ToolPack.ItemType=ACEItemType::Container;ToolPack.ItemsCapacity=24;
        Session.WorldObjects.Add(ToolPack.Guid,ToolPack);
        for(int Case=0;Case<5;++Case)
        {
            auto& Tool=Session.WorldObjects[Ust.Guid];Tool=Ust;Tool.WeenieClassId=ACEWeenieClass::TinkeringTool;
            Tool.ObjectDescriptionFlags|=ACEObjectDescFlag::Retained;
            Tool.ItemType=0; // Some custom servers omit the Decal tool category.
            Tool.ContainerId=Case==0?Session.PlayerGuid:ToolPack.Guid;
            Session.TradeSelfItems.Reset();
            if(Case==2)Session.TradeSelfItems.Add(Tool.Guid);
            if(Case==3)Session.TradeSelfItems.Add(ToolPack.Guid);
            if(Case==4)Tool.WeenieClassId=1;
            ++Session.InventoryDataRevision;
            auto Snapshot=MakeShared<FJsonObject>();H->ExtendSnapshot(Snapshot);
            bool Found=false;for(const auto& V:Snapshot->GetArrayField(TEXT("inventory")))if(V->AsObject()->GetNumberField(TEXT("id"))==Ust.Guid)
            {
                Found=true;TestEqual(TEXT("Retained Ust is usable in any pack, unless offered in trade"),V->AsObject()->GetBoolField(TEXT("salvage_tool_available")),Case<2);
                TestFalse(TEXT("Retained Ust remains protected from destructive loot actions"),V->AsObject()->GetBoolField(TEXT("resource_available")));
            }
            TestTrue(TEXT("Ust appears in owned inventory snapshot"),Found);
            Session.WorldObjects[Blade.Guid]=Blade;Session.WorldObjects[Blade.Guid].MaterialType=59;H->Appraisals[Blade.Guid]=BladeInfo;
            Session.CachedC2SPackets.Reset();H->ExecuteInventory(*P,Salvage,TEXT("salvage"));
            bool Sent=false;for(const auto& Packet:Session.CachedC2SPackets)
            {FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.ReadUInt32()==ACEGameAction::CreateTinkeringTool)Sent=true;}
            TestEqual(TEXT("Native salvage accepts the same tools as the snapshot"),Sent,Case<2);
        }
        Session.TradeSelfItems.Reset();Session.WorldObjects.Remove(ToolPack.Guid);
        Session.WorldObjects.Remove(Ust.Guid);P->ActivityFailure.Reset();
        Session.WorldObjects.Remove(Blade.Guid);H->Appraisals.Remove(Blade.Guid);P->WaitAction.Empty();P->NextAction=0;
        H->LastBuffReply.Empty();
    }
    Session.OnChatMessage.Broadcast(TEXT("mage"),TEXT("Visitor"),ACEChatMessageType::Tell);
    TestEqual(TEXT("Ordinary chat display cannot fabricate a buff request"),H->QueuedBuffRequests(),0);
    Tell(TEXT("mage"),0x70000001);Tell(TEXT("mage"),Session.PlayerGuid);Tell(TEXT("please mage"));
    TestEqual(TEXT("NPC, own tells and non-command text ignored"),H->QueuedBuffRequests(),0);
    Tell(TEXT("  MAGE  "));TestEqual(TEXT("Authenticated player tell queues exact case-insensitive keyword"),H->QueuedBuffRequests(),1);
    Tell(TEXT("mage"));TestEqual(TEXT("Duplicate request does not reset a buff cycle"),H->QueuedBuffRequests(),1);
    auto Done=MakeShared<FJsonObject>();Done->SetStringField(TEXT("action"),TEXT("buff_request_done"));
    Done->SetNumberField(TEXT("request"),H->BuffRequests[0]->AsObject()->GetNumberField(TEXT("id")));Done->SetStringField(TEXT("status"),TEXT("Complete"));
    H->Execute(*P,Done);Tell(TEXT("mage"));TestEqual(TEXT("Recent completed request is rate limited"),H->QueuedBuffRequests(),0);
    H->ClearBuffRequests();P->Profile->SetBoolField(TEXT("buffing"),false);Tell(TEXT("mage"));
    TestEqual(TEXT("Buff off does not accept requests"),H->QueuedBuffRequests(),0);
    P->Profile->SetBoolField(TEXT("buffing"),true);
    auto Command=MakeShared<FJsonObject>();Command->SetStringField(TEXT("keyword"),TEXT("warrior"));Command->SetStringField(TEXT("role"),TEXT("heavy"));
    P->Profile->SetArrayField(TEXT("buff_commands"),{MakeShared<FJsonValueObject>(Command)});
    Tell(TEXT("heavy"));TestEqual(TEXT("Removed default keyword no longer responds"),H->QueuedBuffRequests(),0);
    Tell(TEXT("warrior"));TestEqual(TEXT("Custom keyword maps to role"),H->BuffRequests[0]->AsObject()->GetStringField(TEXT("role")),FString(TEXT("heavy")));
    H->ClearBuffRequests();for(int32 I=0;I<10;++I)Tell(TEXT("warrior"),0x50000010+I);
    TestEqual(TEXT("Queue is bounded"),H->QueuedBuffRequests(),8);
    H->Stop(TEXT("ucm"));TestEqual(TEXT("Stopping clears request queue"),H->QueuedBuffRequests(),0);

    // A migrated profile omits route_preview. The UI defaults it to on, so the
    // real renderer must also draw, with ground projection and active markers.
    Session.PlayerPosition.CellId=0x01010001;Session.PlayerPosition.Location=FVector(0,0,0);
    const FVector Origin=Session.PlayerPosition.ToUnrealLocation();
    auto* Floor=World->SpawnActor<AActor>();auto* Box=NewObject<UBoxComponent>(Floor);
    Floor->SetRootComponent(Box);Box->SetBoxExtent(FVector(2000,2000,50));
    Box->SetCollisionObjectType(ECC_WorldStatic);Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Box->SetCollisionResponseToAllChannels(ECR_Block);Box->RegisterComponent();Floor->SetActorLocation(Origin-FVector(0,0,50));
    {
        auto* Wall=World->SpawnActor<AActor>();auto* Obstacle=NewObject<UBoxComponent>(Wall);
        Wall->SetRootComponent(Obstacle);Obstacle->SetBoxExtent(FVector(10,300,300));
        Obstacle->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Obstacle->SetCollisionResponseToAllChannels(ECR_Block);
        Obstacle->SetCollisionResponseToChannel(ECC_Camera,ECR_Ignore);Obstacle->RegisterComponent();Wall->SetActorLocation(Origin+FVector(-500,0,200));
        FACEPluginSightQuery Sight(*World,nullptr);
        for(double Height:{25.,120.,250.})
            TestFalse(TEXT("Wall / closed door blocks low, middle and high monster sight despite ignoring Camera"),Sight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,Height)));
        Obstacle->SetCollisionResponseToChannel(ECC_Pawn,ECR_Ignore);
        TestTrue(TEXT("Open door immediately restores sight"),Sight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120)));
        Obstacle->SetCollisionResponseToChannel(ECC_Pawn,ECR_Block);
        Wall->SetActorLocation(Origin+FVector(-500,305,200));
        TestTrue(TEXT("Center ray can clear a wall corner by five centimetres"),Sight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120)));
        TestFalse(TEXT("Finite firing corridor rejects a grazing corner"),Sight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120),15.f));
        Wall->SetActorLocation(Origin+FVector(-500,330,200));
        TestTrue(TEXT("Adequate corner clearance allows the shot"),Sight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120),15.f));
        Wall->Destroy();
        auto* Room=World->SpawnActor<AACEEnvCellActor>();
        Room->SetActorLocation(Origin+FVector(-500,0,0));
        Room->CellMesh->CreateMeshSection(0,{FVector(0,-300,0),FVector(0,300,0),FVector(0,300,400),FVector(0,-300,400)},
            {0,1,2,0,2,3},TArray<FVector>(),TArray<FVector2D>(),TArray<FColor>(),TArray<FProcMeshTangent>(),false);
        Room->SetActorHiddenInGame(true);Room->CellMesh->SetVisibility(false);
        Room->CellMesh->SetMeshSectionVisible(0,false);
        FACEPluginSightQuery HiddenSight(*World,nullptr);
        TestFalse(TEXT("Culled room wall without cooked collision still blocks combat"),HiddenSight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120)));
        Room->SetActorLocation(Origin+FVector(-500,305,0));
        TestTrue(TEXT("Uncooked corner clears the center ray"),HiddenSight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120)));
        TestFalse(TEXT("Hidden uncooked wall edges block the full firing corridor"),HiddenSight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120),15.f));
        Room->SetActorScale3D(FVector(1,2,.5));Room->SetActorLocation(Origin+FVector(-500,605,0));
        TestFalse(TEXT("Nonuniformly scaled room edges retain world-space clearance"),HiddenSight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120),15.f));
        Room->SetActorLocation(Origin+FVector(-500,630,0));
        TestTrue(TEXT("Cleared uncooked wall allows the full corridor"),HiddenSight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,120),15.f));
        TestTrue(TEXT("Low target corridor stays clear of its supporting floor"),HiddenSight.Clear(Origin+FVector(0,0,120),Origin+FVector(-1000,0,10),3.f));
        Room->Destroy();
    }
    // Exercise real snapshot -> Lua -> controller dispatch. The policy-only
    // route test could not detect that pursuit left the selected target unset.
    {
        const auto SavedPlugin=*P;const auto SavedObjects=Session.WorldObjects;
        const auto SavedSelected=Session.SelectedObject;
        const bool SavedBusy=Session.bUseBusy;const int32 SavedEquipment=Session.PendingEquipmentGuid;
        Session.bUseBusy=false;Session.PendingEquipmentGuid=0;
        auto* ChasePC=World->SpawnActor<AACEPlayerController>();auto* ChasePawn=World->SpawnActor<APawn>();
        auto* ChaseRoot=NewObject<USceneComponent>(ChasePawn);ChasePawn->SetRootComponent(ChaseRoot);ChaseRoot->RegisterComponent();
        ChasePawn->SetActorLocation(Origin);ChasePC->Possess(ChasePawn);
        auto* Local=NewObject<ULocalPlayer>(GEngine);GI->AddLocalPlayer(Local,FPlatformUserId::CreateFromInternalId(0));ChasePC->SetPlayer(Local);
        ON_SCOPE_EXIT {
            *P=SavedPlugin;Session.WorldObjects=SavedObjects;Session.SelectedObject=SavedSelected;
            Session.bUseBusy=SavedBusy;Session.PendingEquipmentGuid=SavedEquipment;
            H->Appraisals.Remove(975);H->CombatTarget=0;H->MovementOwner.Empty();H->MoveExpires=0;
            GI->RemoveLocalPlayer(Local);ChasePC->Destroy();ChasePawn->Destroy();
        };
        auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
        P->Profile=Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"missile","manual_combat":true,"approach":true,"approach_range":60,"radius":6})"));
        P->NextAction=0;P->CombatTransitionIntent.Reset();P->Permissions.AddUnique(TEXT("navigation"));P->Permissions.AddUnique(TEXT("combat"));
        FACEWorldObject Enemy;Enemy.Guid=975;Enemy.Name=TEXT("Distant monster");Enemy.ItemType=ACEItemType::Creature;
        Enemy.bHasPosition=true;Enemy.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
        Enemy.Position=Session.PlayerPosition;Enemy.Position.Location.Y+=60;
        Session.WorldObjects.Reset();Session.WorldObjects.Add(Enemy.Guid,Enemy);
        FACEAppraisalInfo Info;Info.bSuccess=true;H->Appraisals.Add(Enemy.Guid,Info);
        auto Snapshot=H->Snapshot();
        const auto& Targets=Snapshot->GetArrayField(TEXT("targets"));
        if(!TestEqual(TEXT("Real world snapshot includes a monster at acquisition range"),Targets.Num(),1))return false;
        TestEqual(TEXT("World distance uses metres"),Targets[0]->AsObject()->GetNumberField(TEXT("distance")),60.);
        TestTrue(TEXT("Distant clear target passes physical sight query"),Targets[0]->AsObject()->GetBoolField(TEXT("line_of_sight")));
        // Isolate equipment/ready state; world positions and sight are real.
        Snapshot->SetNumberField(TEXT("health"),100);Snapshot->SetNumberField(TEXT("max_health"),100);
        Snapshot->SetBoolField(TEXT("ready"),true);Snapshot->SetBoolField(TEXT("busy"),false);
        Snapshot->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":99,"type":256,"equipped":true,"identified":true,"can_wield":true,"weapon_skill":48,"damage":10,"name":"Thrown weapon"})")))});
        FString Script,Error;FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
        FACEPluginVM VM;TestTrue(TEXT("Pursuit policy loads"),VM.Load(Script,Error));TSharedPtr<FJsonObject> Intent;
        TestTrue(*Error,VM.Step(Snapshot,P->Profile,Intent,Error));if(!Intent)return false;
        TestEqual(TEXT("Distant target requests pursuit before any shot"),Intent->GetStringField(TEXT("action")),FString(TEXT("move")));
        TestFalse(TEXT("Pursuit fixture has no pending use or equipment request"),C->IsUseBusy());
        TestFalse(TEXT("Pursuit fixture is out of portal space"),ChasePC->IsWorldTransitionActive());
        H->Execute(*P,Intent);
        TestEqual(TEXT("Pursuit selects monster in game before entering attack range"),C->GetSelectedObject().Guid,Enemy.Guid);
        TestEqual(TEXT("Death/health wake follows the acquired monster"),H->CombatTarget,Enemy.Guid);
        TestEqual(TEXT("Pursuit reaches controller steering"),H->MovementOwner,FString(TEXT("ucm")));
        TestEqual(TEXT("Steering destination is the distant monster"),H->MoveTarget.Location,Enemy.Position.Location);
        float F=0,R=0,T=0;H->ApplyMovement(ChasePC,F,R,T,false,false,false,FVector::ForwardVector,100.f);
        TestTrue(TEXT("60m acquisition produces movement input"),!FMath::IsNearlyZero(F)||!FMath::IsNearlyZero(T));
        Session.WorldObjects.Remove(Enemy.Guid);P->NextAction=0;H->Execute(*P,Intent);
        TestTrue(TEXT("Despawned pursuit target cancels stale movement"),H->MovementOwner.IsEmpty());
    }
    auto Point=[&](double X,const FString& Kind=TEXT("walk"))
    {auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("cell"),0x01010001);O->SetNumberField(TEXT("x"),X);O->SetNumberField(TEXT("y"),0);O->SetNumberField(TEXT("z"),.2);O->SetStringField(TEXT("kind"),Kind);return MakeShared<FJsonValueObject>(O);};
    P->Profile->SetArrayField(TEXT("route"),{Point(0),Point(10)});H->RoutePoint=2;H->NextRouteCheck=0;H->DrawRoute();
    if(!TestNotNull(TEXT("Migrated profile creates route geometry"),H->RouteMesh.Get()))return false;
    int32 Path=0,Ring=0;bool Grounded=true;
    for(const auto& L:H->RouteSegments)
    {
        if(L.Thickness==10){++Path;Grounded&=FMath::IsNearlyEqual(L.Start.Z,10.,.1)&&FMath::IsNearlyEqual(L.End.Z,10.,.1);}
        if(L.Thickness==8)++Ring;
    }
    TestTrue(TEXT("Ground path connects waypoints in multiple segments"),Path>1);
    TestTrue(TEXT("Path is projected onto floor, not floating at the recorded height"),Grounded);
    TestEqual(TEXT("Current waypoint has a full circle"),Ring,32);
    TestTrue(TEXT("Other waypoint uses sphere geometry"),H->RouteSegments.Num()>Path+Ring);
    TestTrue(TEXT("Route culling bounds contain geometry far from world origin"),H->RouteMesh->Bounds.GetBox().IsInsideOrOn(Origin+FVector(-500,0,10)));
    if(FApp::CanEverRender())
    {
        auto* View=World->SpawnActor<AActor>();auto* Capture=NewObject<USceneCaptureComponent2D>(View);
        View->SetRootComponent(Capture);Capture->RegisterComponent();
        auto* Target=NewObject<UTextureRenderTarget2D>();Target->InitAutoFormat(256,256);Target->UpdateResourceImmediate(true);
        Capture->TextureTarget=Target;Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;
        Capture->FOVAngle=65;
        Capture->CaptureSource=ESceneCaptureSource::SCS_FinalColorLDR;
        Capture->ShowFlags.SetAtmosphere(false);Capture->ShowFlags.SetFog(false);Capture->ShowFlags.SetBloom(false);
        Capture->ShowFlags.SetEyeAdaptation(false);Capture->ShowFlags.SetAntiAliasing(false);
        Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;Capture->ShowOnlyComponent(H->RouteMesh);
        View->SetActorLocationAndRotation(Origin+FVector(-500,0,1500),FRotator(-90,0,0));
        if(GShaderCompilingManager)GShaderCompilingManager->FinishAllCompilation();
        World->SendAllEndOfFrameUpdates();Capture->CaptureScene();FlushRenderingCommands();
        TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        int32 Colored=0;for(const auto& Pixel:Pixels)Colored+=(Pixel.G>Pixel.R+30&&Pixel.G>80);
        AddInfo(FString::Printf(TEXT("Route capture: %d colored pixels, %d segments, visible=%d render-state=%d"),Colored,H->RouteSegments.Num(),H->RouteMesh->IsVisible(),H->RouteMesh->IsRenderStateCreated()));
        TestTrue(TEXT("Route lines and target circle actually render, not merely exist in a CPU list"),Colored>100);
        TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(256,256,Pixels,Png);
        FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/UCM-Route-Overlay.png")));
        Target->ReleaseResource();View->Destroy();
    }
    const int32 PreviousLines=H->RouteSegments.Num();H->NextRouteCheck=0;H->DrawRoute();
    TestEqual(TEXT("Unchanged preview reuses geometry"),H->RouteSegments.Num(),PreviousLines);
    const auto PreviousCell=Session.PlayerPosition.CellId;const double BuiltAt=H->RouteRebuiltAt;
    Session.PlayerPosition.CellId=(PreviousCell&int32(0xffff0000))|((uint32(PreviousCell)&0xffff)<0x100?2:0x102);
    H->NextRouteCheck=0;H->DrawRoute();
    TestEqual(TEXT("Cell index alone does not retrace or rebuild the route"),H->RouteRebuiltAt,BuiltAt);
    Session.PlayerPosition.CellId=PreviousCell;
    P->Profile->SetBoolField(TEXT("loop_route"),true);H->NextRouteCheck=0;H->DrawRoute();
    int32 LoopPath=0;for(const auto& L:H->RouteSegments)LoopPath+=L.Thickness==10;
    TestEqual(TEXT("Circular route draws the return leg to its first point"),LoopPath,Path*2);
    P->Profile->SetBoolField(TEXT("reverse_route"),true);H->NextRouteCheck=0;H->DrawRoute();
    int32 LinearPath=0;for(const auto& L:H->RouteSegments)LinearPath+=L.Thickness==10;
    TestEqual(TEXT("Reversing route reuses its path without a false closing leg"),LinearPath,Path);
    P->Profile->SetBoolField(TEXT("loop_route"),false);P->Profile->SetBoolField(TEXT("reverse_route"),false);
    // NAV coordinates can normalize into the neighboring LB even though the
    // player remains inside a dungeon with negative local coordinates.
    const auto OutdoorPosition=Session.PlayerPosition;
    Session.PlayerPosition.CellId=0x01010100;Session.PlayerPosition.Location=FVector(0,-10,0);
    auto DungeonPoint=Point(0)->AsObject();DungeonPoint->SetNumberField(TEXT("cell"),0x01000008);
    DungeonPoint->SetNumberField(TEXT("y"),182);DungeonPoint->SetBoolField(TEXT("legacy"),true);
    P->Profile->SetArrayField(TEXT("route"),{MakeShared<FJsonValueObject>(DungeonPoint)});H->RoutePoint=1;H->NextRouteCheck=0;H->DrawRoute();
    TestEqual(TEXT("Imported dungeon waypoint beyond LB boundary still draws its target circle"),H->RouteSegments.Num(),32);
    TestTrue(TEXT("Replacement route refreshes bounds to the new location"),H->RouteMesh->Bounds.GetBox().IsInsideOrOn(Session.PlayerPosition.ToUnrealLocation()+FVector(0,0,10)));
    Session.PlayerPosition=OutdoorPosition;
    P->Profile->SetArrayField(TEXT("route"),{Point(0,TEXT("portal")),Point(10)});H->NextRouteCheck=0;H->DrawRoute();
    Path=0;for(const auto& L:H->RouteSegments)Path+=L.Thickness==10;
    TestEqual(TEXT("Editing portal point immediately removes false ground connection"),Path,0);
    P->Profile->SetBoolField(TEXT("route_preview"),false);H->NextRouteCheck=0;H->DrawRoute();
    TestEqual(TEXT("Preview off clears markers and path"),H->RouteSegments.Num(),0);
    {
        auto* LootPC=World->SpawnActor<AACEPlayerController>();
        auto* Local=NewObject<ULocalPlayer>(GEngine);GI->AddLocalPlayer(Local,FPlatformUserId::CreateFromInternalId(0));LootPC->SetPlayer(Local);
        const auto SavedVitals=Session.PlayerVitals;const int32 SavedContainer=Session.OpenExternalContainerGuid;
        const auto SavedObjects=Session.WorldObjects;
        ON_SCOPE_EXIT {Session.WorldObjects=SavedObjects;Session.PlayerVitals=SavedVitals;Session.OpenExternalContainerGuid=SavedContainer;H->UseApproachOwner.Empty();GI->RemoveLocalPlayer(Local);LootPC->Destroy();};
        FACEWorldObject Self;Self.Guid=Session.PlayerGuid;Self.ItemsCapacity=102;Session.WorldObjects.Add(Self.Guid,Self);
        TestTrue(TEXT("Loot regression uses the real local-controller dispatch"),GI->GetFirstLocalPlayerController()==LootPC);
        FACEWorldObject Corpse;Corpse.Guid=870;Corpse.Name=TEXT("Corpse of Red Rat");Corpse.bHasPosition=true;Corpse.Position=Session.PlayerPosition;Corpse.ObjectDescriptionFlags=ACEObjectDescFlag::Corpse;Session.WorldObjects.Add(870,Corpse);
        FACEWorldObject Drop;Drop.Guid=871;Drop.Name=TEXT("Loot");Drop.ContainerId=870;Drop.StackSize=1;Session.WorldObjects.Add(871,Drop);
        auto Intent=MakeShared<FJsonObject>();Intent->SetNumberField(TEXT("item"),870);
        for(int32 Mode:{1,2,4,8})for(const TCHAR* Action:{TEXT("open_corpse"),TEXT("use_world")})
        {
            Session.PendingCombatMode=0;Session.ApplyServerCombatMode(Mode);Session.bServerAttackInProgress=false;Session.LastAttackError=0x20;
            TestTrue(TEXT("Corpse dispatch is handled"),H->ExecuteInventory(*P,Intent,Action));
            TestTrue(TEXT("Corpse dispatch starts normal approach"),LootPC->IsUseApproachActive());
            TestEqual(TEXT("Opening corpse preserves peace/melee/missile/magic mode"),Session.PlayerVitals.CombatMode,Mode);
            Session.OpenExternalContainerGuid=870;
            auto Pickup=MakeShared<FJsonObject>();Pickup->SetNumberField(TEXT("item"),871);
            H->ExecuteInventory(*P,Pickup,TEXT("loot"));
            TestEqual(TEXT("Taking corpse loot preserves mode"),Session.PlayerVitals.CombatMode,Mode);
            const double Deadline=P->NextAction;H->RefreshActionWait(*P);
            TestEqual(TEXT("Unconfirmed pickup retains throttle"),P->NextAction,Deadline);
            FACEAppraisalInfo OtherID;OtherID.ObjectGuid=123456;H->ObserveAppraisal(OtherID);
            TestEqual(TEXT("Unrelated appraisal cannot release a pickup"),P->NextAction,Deadline);
            ConfirmPut(871,Session.PlayerGuid);H->RefreshActionWait(*P);
            TestTrue(TEXT("Ownership confirmation removes fixed one-second pickup delay"),P->NextAction<=P->ActionSentAt+.25);
            TestEqual(TEXT("Confirmation wakes next policy decision"),P->NextDecision,0.);
            Session.WorldObjects[871].ContainerId=870;
            H->ExecuteInventory(*P,Intent,TEXT("close_corpse"));
            TestEqual(TEXT("Closing corpse preserves mode"),Session.PlayerVitals.CombatMode,Mode);
            TestTrue(TEXT("Closing corpse adds no artificial use delay"),P->NextAction<=FPlatformTime::Seconds());
            TestEqual(TEXT("Closing clears the container before another request"),Session.OpenExternalContainerGuid,0);
        }
        {
            const bool Busy=Session.bUseBusy;const int32 Equipment=Session.PendingEquipmentGuid;
            const int32 SourceGuid=Session.UseSourceGuid,TargetGuid=Session.UseTargetGuid;
            ON_SCOPE_EXIT {Session.bUseBusy=Busy;Session.PendingEquipmentGuid=Equipment;Session.UseSourceGuid=SourceGuid;Session.UseTargetGuid=TargetGuid;H->PendingSpell=0;};
            Session.PendingEquipmentGuid=0;Session.OpenExternalContainerGuid=0;H->PendingSpell=0;P->Running=true;
            LootPC->BeginUseApproach(870,.6f);H->UseApproachOwner=P->Id;
            H->TrackActionWait(*P,TEXT("open_corpse"),870);P->NextAction=FPlatformTime::Seconds()-1;
            Session.BeginPendingUse(870,0);H->RefreshActionWait(*P);
            TestEqual(TEXT("Long corpse approach retains ownership after initial throttle"),P->WaitAction,FString(TEXT("open_corpse")));
            TestTrue(TEXT("Only the owned corpse approach is available for interruption"),H->CanPauseLootApproach(*P));
            TestTrue(TEXT("Busy snapshot exposes the safe corpse interruption"),H->Snapshot()->GetBoolField(TEXT("loot_approaching")));
            H->PendingSpell=123;TestFalse(TEXT("A pending cast cannot be released as corpse approach"),H->CanPauseLootApproach(*P));H->PendingSpell=0;
            Session.PendingEquipmentGuid=871;TestFalse(TEXT("Pending equipment cannot be released as corpse approach"),H->CanPauseLootApproach(*P));Session.PendingEquipmentGuid=0;
            Session.UseSourceGuid=871;TestFalse(TEXT("An unrelated use cannot be released as corpse approach"),H->CanPauseLootApproach(*P));Session.UseSourceGuid=870;
            H->TrackActionWait(*P,TEXT("loot"),871);TestFalse(TEXT("An item transfer is not a cancellable approach"),H->CanPauseLootApproach(*P));
            H->TrackActionWait(*P,TEXT("open_corpse"),870);
            auto Pause=MakeShared<FJsonObject>();Pause->SetStringField(TEXT("action"),TEXT("pause_loot_approach"));
            H->Execute(*P,Pause);
            TestTrue(TEXT("Combat interruption releases only approach and its use wait"),!LootPC->IsUseApproachActive()&&!Session.IsUseBusy()&&P->WaitAction.IsEmpty()&&P->NextAction==0);
            TestTrue(TEXT("Interrupting looting keeps UCM running"),P->Running);
        }
        {
            const auto ObjectsBefore=Session.WorldObjects;const auto ContentsBefore=Session.ContainerContents;
            ON_SCOPE_EXIT {Session.WorldObjects=ObjectsBefore;Session.ContainerContents=ContentsBefore;Session.TradeSelfItems.Reset();P->ActivityFailure.Reset();P->WaitAction.Empty();Session.CachedC2SPackets.Reset();};
            auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            FSocket* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("UCM pickup test receiver"),false);
            const auto Address=Sockets->CreateInternetAddr();bool Valid=false;Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
            if(!TestTrue(TEXT("Isolated pickup test socket binds"),Receiver&&Receiver->Bind(*Address)))return false;
            Receiver->GetAddress(*Address);Session.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("UCM pickup test sender"),false);
            Session.ServerC2SAddr=Address;Session.IssacClient=MakeUnique<FACEIsaac>(123u);
            ON_SCOPE_EXIT {Session.SocketC2S->Close();Sockets->DestroySocket(Session.SocketC2S);Session.SocketC2S=nullptr;Session.ServerC2SAddr.Reset();Session.IssacClient.Reset();Receiver->Close();Sockets->DestroySocket(Receiver);};
            Session.WorldObjects.Reset();Session.ContainerContents.Reset();
            Self.ItemsCapacity=1;Self.ContainersCapacity=7;Session.WorldObjects.Add(Self.Guid,Self);
            Session.WorldObjects.Add(Corpse.Guid,Corpse);Session.WorldObjects.Add(Drop.Guid,Drop);
            FACEWorldObject Filler;Filler.Guid=880;Filler.ContainerId=Self.Guid;Session.WorldObjects.Add(Filler.Guid,Filler);
            for(int32 Id:{881,882,883})
            {
                FACEWorldObject Pack;Pack.Guid=Id;Pack.ContainerId=Self.Guid;Pack.ItemType=ACEItemType::Container;
                Pack.ItemsCapacity=1;Pack.PlacementPosition=Id-881;Session.WorldObjects.Add(Id,Pack);
            }
            FACEContainerItemRef Unknown;Unknown.ItemGuid=889;Session.ContainerContents.FindOrAdd(881).Add(Unknown);
            auto Pickup=MakeShared<FJsonObject>();Pickup->SetStringField(TEXT("action"),TEXT("loot"));Pickup->SetNumberField(TEXT("item"),Drop.Guid);
            auto CheckPickup=[&](int32 Destination,int32 Amount,int32 MergeTo=0)
            {
                const auto BeforeObjects=Session.WorldObjects;const auto BeforeContents=Session.ContainerContents;
                Session.OpenExternalContainerGuid=Corpse.Guid;Session.CachedC2SPackets.Reset();Pickup->SetNumberField(TEXT("amount"),Amount);
                H->ExecuteInventory(*P,Pickup,TEXT("loot"));
                TestEqual(TEXT("Available pack emits exactly one transfer"),Session.CachedC2SPackets.Num(),1);
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                    TestEqual(TEXT("Loot preserves whole-stack/split/merge wire action"),R.ReadUInt32(),MergeTo?ACEGameAction::StackableMerge:Amount<Session.WorldObjects[Drop.Guid].StackSize?ACEGameAction::StackableSplitToContainer:ACEGameAction::PutItemInContainer);
                    TestEqual(TEXT("Loot identifies the original corpse item"),R.ReadUInt32(),uint32(Drop.Guid));
                    TestEqual(TEXT("Loot routes to the available pack or compatible stack"),R.ReadUInt32(),uint32(MergeTo?MergeTo:Destination));
                    if(MergeTo)TestEqual(TEXT("Merge transfers only the requested quantity"),R.ReadInt32(),Amount);
                }
                ConfirmPut(Drop.Guid,Destination);
                Session.WorldObjects=BeforeObjects;Session.ContainerContents=BeforeContents;
            };
            for(int32 Destination:{882,883})
            {
                CheckPickup(Destination,1);
                Filler.Guid=Destination+10;Filler.ContainerId=Destination;Session.WorldObjects.Add(Filler.Guid,Filler);
            }
            Session.OpenExternalContainerGuid=Corpse.Guid;Session.CachedC2SPackets.Reset();P->Running=true;P->ActivityFailure.Reset();
            H->ExecuteInventory(*P,Pickup,TEXT("loot"));
            TestTrue(TEXT("Full inventory leaves UCM running"),P->Running);
            TestTrue(TEXT("All packs full reports a capacity pause rather than waiting for rejection"),P->ActivityFailure&&P->ActivityFailure->GetBoolField(TEXT("inventory_full")));
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);const uint32 Action=R.ReadUInt32();
                TestTrue(TEXT("Full packs never send pickup, split, or cancel combat"),Action!=ACEGameAction::PutItemInContainer&&Action!=ACEGameAction::StackableSplitToContainer&&Action!=ACEGameAction::CancelAttack);
            }
            P->ActivityFailure.Reset();
            auto& Source=Session.WorldObjects[Drop.Guid];Source.WeenieClassId=273;Source.StackSize=20;Source.MaxStackSize=100;
            auto& Merge=Session.WorldObjects[893];Merge.WeenieClassId=273;Merge.StackSize=50;Merge.MaxStackSize=100;
            CheckPickup(883,20,893); // A compatible stack is usable even with every slot full.
            CheckPickup(883,5,893);  // Partial loot uses the same destination search.
            Session.TradeSelfItems.Add(883);TestEqual(TEXT("Packs offered in trade cannot receive automated loot"),C->FindPickupContainer(Drop.Guid,0),0);Session.TradeSelfItems.Reset();
            Session.WorldObjects[882].ItemsCapacity=-1;Session.WorldObjects[893].StackSize=100;CheckPickup(882,20);
            CheckPickup(882,5);
        }
        // Finish the prior synthetic pickup/equipment operations before testing
        // a fresh non-corpse interaction's stance transition.
        Session.CancelEquipmentSwap();Session.bUseBusy=false;H->PendingSpell=0;
        Corpse.ObjectDescriptionFlags=0;Session.WorldObjects.Add(870,Corpse);Session.PendingCombatMode=0;Session.ApplyServerCombatMode(8);
        P->Running=true;P->Player=Session.PlayerGuid;P->Server=C->GetServerName();
        H->ExecuteInventory(*P,Intent,TEXT("use_world"));
        TestEqual(TEXT("Ordinary world-use requests peace before approaching"),Session.PlayerVitals.CombatMode,1);
        TestTrue(TEXT("World-use waits for server stance confirmation"),P->CombatTransitionIntent.IsValid());
        P->CombatTransitionIntent.Reset();Session.ApplyServerCombatMode(1);
        H->TrackActionWait(*P,TEXT("equip"),871,0x100000);P->NextAction=P->ActionSentAt+2;
        H->RefreshActionWait(*P);TestFalse(TEXT("Equipment request does not complete before wield update"),P->WaitAction.IsEmpty());
        Session.WorldObjects[871].WielderId=Session.PlayerGuid;Session.WorldObjects[871].CurrentWieldedLocation=0x100000;
        H->RefreshActionWait(*P);TestTrue(TEXT("Wield confirmation releases equipment delay"),P->WaitAction.IsEmpty()&&P->NextAction<=P->ActionSentAt+.25);
        H->TrackActionWait(*P,TEXT("identify"),871);P->NextAction=P->ActionSentAt+.5;
        FACEAppraisalInfo Reply;Reply.ObjectGuid=870;H->ObserveAppraisal(Reply);
        TestFalse(TEXT("Different appraisal cannot wake item decision"),P->WaitAction.IsEmpty());
        Reply.ObjectGuid=871;H->ObserveAppraisal(Reply);
        TestTrue(TEXT("Requested appraisal wakes decision, including a failed appraisal"),P->WaitAction.IsEmpty()&&P->NextAction<=P->ActionSentAt+.25);
        H->Appraisals.Remove(870);H->Appraisals.Remove(871);H->Appraisals.Remove(123456);
        H->TrackActionWait(*P,TEXT("use"));P->NextAction=P->ActionSentAt+3;
        H->RefreshActionWait(*P);TestFalse(TEXT("Consumable waits for UseDone"),P->WaitAction.IsEmpty());
        H->ObserveUseDone(0);H->RefreshActionWait(*P);
        TestTrue(TEXT("Consumable completion releases three-second fallback"),P->WaitAction.IsEmpty()&&P->NextAction<=P->ActionSentAt+.25);
        P->NextAction=0;
    }
    // Exercise AC's +Y-forward pose and AC-space turn integration, not a
    // synthetic Unreal X-forward pawn (which masked both steering errors).
    auto* PC=World->SpawnActor<AACEPlayerController>();auto* Pawn=World->SpawnActor<APawn>();
    auto* Root=NewObject<USceneComponent>(Pawn);Pawn->SetRootComponent(Root);Root->RegisterComponent();PC->Possess(Pawn);
    {
        H->MovementOwner.Empty();
        const uint32 PreviousStance=Session.GetCurrentStance();Session.SetCurrentStance(ACEMotion::StanceMagic);
        ON_SCOPE_EXIT{Session.SetCurrentStance(PreviousStance);};
        FACEWorldObject Player;Player.Guid=Session.PlayerGuid;Player.Name=TEXT("Caster");Session.WorldObjects.Add(Player.Guid,Player);
        auto Begin=[&]()
        {
            H->PendingSpell=123;H->PendingSpellAt=FPlatformTime::Seconds();H->PendingSpellOwner=TEXT("ucm");
            H->PendingSpellConfirmation=TEXT("You cast Focus Self I on yourself");
            H->PendingSpellConfirmed=false;H->PendingSpellFizzled=false;H->FastCastOwner=TEXT("ucm");
            H->FastCastStarted=false;H->FastCastMovementApplied=false;
            H->ObserveMetaChat(TEXT("words"),TEXT("Other"),ACEChatMessageType::Spellcasting);
            TestFalse(TEXT("Another caster cannot start backward input"),H->FastCastStarted);
            H->ObserveMetaChat(TEXT("words"),TEXT("Caster"),ACEChatMessageType::Spellcasting);
        };
        auto Drive=[&]() {float F=0,R=0,T=0;H->ApplyMovement(PC,F,R,T,false,false,false,FVector::ForwardVector);return F;};
        Begin();TestEqual(TEXT("Local words never add a backward locomotion axis"),Drive(),0.f);
        TestTrue(TEXT("Local words consume one synchronous fast-buff tap"),H->FastCastMovementApplied&&H->FastCastOwner.IsEmpty());
        H->ObserveMetaChat(TEXT("You cast Focus Self I on yourself"),TEXT("Other"),ACEChatMessageType::Magic);
        H->ObserveMetaChat(TEXT("You cast Focus Self II on yourself"),TEXT(""),ACEChatMessageType::Magic);
        TestFalse(TEXT("Wrong sender or spell cannot complete our buff"),H->PendingSpellConfirmed);
        H->ObserveMetaChat(TEXT("You cast Focus Self I on yourself, refreshing Focus Self I"),TEXT(""),ACEChatMessageType::Magic);
        TestEqual(TEXT("Spell result releases backward input before delayed UseDone"),Drive(),0.f);
        TestEqual(TEXT("Result retains the request until its server acknowledgment"),H->PendingSpell,123);
        P->NextAction=FPlatformTime::Seconds()+3.5;P->NextDecision=FPlatformTime::Seconds()+.25;
        H->ObserveUseDone(0);TestTrue(TEXT("Acknowledged matching result confirms the buff"),H->LastSpellConfirmed);
        TestTrue(TEXT("Completed fast buff is ready immediately, without a half-second floor"),P->NextAction<=FPlatformTime::Seconds()&&P->NextDecision==0);
        Begin();H->ObserveMetaChat(TEXT("You cast Focus Self I on yourself"),TEXT(""),ACEChatMessageType::Magic);
        TestEqual(TEXT("Coalesced words and result cancel recoil without locomotion"),Drive(),0.f);
        TestTrue(TEXT("Coalesced result still consumes its tap"),H->FastCastMovementApplied);
        TestEqual(TEXT("Coalesced result never leaves backward held"),Drive(),0.f);H->ObserveUseDone(0);
        Begin();H->ObserveMetaChat(TEXT("Your spell fizzled."),TEXT(""),ACEChatMessageType::Magic);
        TestEqual(TEXT("Fizzle releases movement immediately"),Drive(),0.f);H->ObserveUseDone(0);
        TestFalse(TEXT("Success UseDone after a fizzle cannot confirm a forced buff"),H->LastSpellConfirmed);
        Begin();TestEqual(TEXT("Unconfirmed cast taps without movement"),Drive(),0.f);
        TestTrue(TEXT("Missing result cannot leave backward held"),H->FastCastMovementApplied&&H->FastCastOwner.IsEmpty());
        H->FastCastStartedAt=FPlatformTime::Seconds()-.13;
        TestEqual(TEXT("Missing result cannot restart backward locomotion"),Drive(),0.f);
        TestEqual(TEXT("Movement timeout does not discard the pending spell"),H->PendingSpell,123);
        H->ObserveMetaChat(TEXT("words"),TEXT("Caster"),ACEChatMessageType::Spellcasting);
        TestEqual(TEXT("Repeated spell words cannot restart an expired bump"),Drive(),0.f);
        H->ObserveUseDone(0);TestFalse(TEXT("Missing spell result cannot confirm a forced buff"),H->LastSpellConfirmed);
        Begin();H->FastCastStartedAt=FPlatformTime::Seconds()-.13;
        TestEqual(TEXT("A blocked or delayed frame never starts a stale bump"),Drive(),0.f);
        H->ObserveUseDone(0);
    }
    {
        auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
        FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
        if(!TestTrue(TEXT("Retail DAT loads for redirected bane confirmations"),Dat->LoadDatDirectory(DatDirectory)))return false;
        {
            const auto SavedSkills=Session.PlayerVitals.Skills;
            FACEWorldObject Scroll;Scroll.Guid=0x71009f00;Scroll.ItemType=ACEItemType::Writable;Scroll.ContainerId=Session.PlayerGuid;
            ON_SCOPE_EXIT {Session.PlayerVitals.Skills=SavedSkills;Session.WorldObjects.Remove(Scroll.Guid);H->InventoryRevision=MAX_uint64;};
            for(uint32 ExpectedPower:{1u,250u,300u,400u})
            {
                Scroll.SpellDID=0;
                for(uint32 Id=1;Id<7000;++Id)
                {
                    uint32 School=0,Power=0,Category=0,Flags=0;double Duration=0;
                    if(Dat->TryGetPluginSpellInfo(Id,School,Power,Category,Flags,Duration)&&School==1&&Power==ExpectedPower){Scroll.SpellDID=Id;break;}
                }
                if(!TestTrue(*FString::Printf(TEXT("Retail spell found for scroll power %u"),ExpectedPower),Scroll.SpellDID!=0))return false;
                Session.WorldObjects.Add(Scroll.Guid,Scroll);H->InventoryRevision=MAX_uint64;
                FACESkillInfo Skill;Skill.SkillId=34;Skill.AdvancementClass=1;Skill.Current=500;
                Session.PlayerVitals.Skills={Skill};
                for(int Case=0;Case<3;++Case)
                {
                    if(Case>0){Session.PlayerVitals.Skills[0].AdvancementClass=2;Session.PlayerVitals.Skills[0].Current=Case==1?199:200;}
                    auto Snapshot=MakeShared<FJsonObject>();Snapshot->SetObjectField(TEXT("position"),MakeShared<FJsonObject>());H->ExtendSnapshot(Snapshot);
                    bool Found=false;
                    for(const auto& Record:Snapshot->GetArrayField(TEXT("inventory")))
                    {
                        const auto Item=Record->AsObject();if(Item->GetNumberField(TEXT("id"))!=double(uint32(Scroll.Guid)))continue;
                        Found=true;TestEqual(TEXT("UCM learning eligibility follows retail tiers and live skill updates"),Item->GetBoolField(TEXT("scroll_can_learn")),ExpectedPower!=250||Case==2);
                    }
                    TestTrue(TEXT("Owned scroll reaches automation snapshot"),Found);
                }
            }
        }
        FACEWorldObject Armor;Armor.Guid=0x71009000;Armor.Name=TEXT("Copper Helm");Armor.ItemType=ACEItemType::Armor;Armor.WielderId=Session.PlayerGuid;
        FACEWorldObject Shirt=Armor;Shirt.Guid++;Shirt.Name=TEXT("Linen Shirt");Shirt.ItemType=ACEItemType::Clothing;
        FACEWorldObject Carried=Armor;Carried.Guid+=2;Carried.Name=TEXT("Carried Helm");Carried.WielderId=0;Carried.ContainerId=Session.PlayerGuid;
        FACEWorldObject Other=Armor;Other.Guid+=3;Other.Name=TEXT("Other Helm");Other.WielderId=Session.PlayerGuid+1;
        for(const auto& Item:{Armor,Shirt,Carried,Other})Session.WorldObjects.Add(Item.Guid,Item);
        ON_SCOPE_EXIT{for(const auto& Item:{Armor,Shirt,Carried,Other})Session.WorldObjects.Remove(Item.Guid);H->ItemBuffs.Empty();};
        FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
        for(const FString Name:{TEXT("Blade Bane I"),TEXT("Impenetrability I")})
        {
            uint32 Spell=0,School=0,Power=0,Category=0,Flags=0;double Duration=0;
            for(uint32 Id=1;Id<7000;++Id){FString Candidate;uint32 Icon=0;if(Dat->TryGetSpellInfo(Id,Candidate,Icon)&&Candidate==Name){Spell=Id;break;}}
            if(!TestTrue(TEXT("Retail bane/impen spell found"),Spell&&Dat->TryGetPluginSpellInfo(Spell,School,Power,Category,Flags,Duration)))return false;
            auto Begin=[&](int32 Target)
            {
                H->ItemBuffs.Empty();H->PendingSpell=Spell;H->PendingSpellTarget=Target;
                H->PendingSpellAt=FPlatformTime::Seconds();H->PendingSpellConfirmed=false;H->PendingSpellFizzled=false;
                H->PrepareSpellConfirmation(Name);
            };
            auto Chat=[&](const FString& Target,const FString& Suffix=TEXT(""),const FString& Sender=TEXT(""))
            {H->ObserveMetaChat(TEXT("You cast ")+Name+TEXT(" on ")+Target+Suffix,Sender,ACEChatMessageType::Magic);};
            for(const FString& Suffix:TArray<FString>{TEXT(""),TEXT("."),TEXT(", refreshing ")+Name,TEXT(", surpassing ")+Name,TEXT(", but it is surpassed by ")+Name})
            {
                Begin(Session.PlayerGuid);
                Chat(Carried.Name);Chat(Other.Name);Chat(Armor.Name+TEXT(" Replica"));Chat(Armor.Name,TEXT(""),TEXT("Other caster"));
                H->ObserveMetaChat(TEXT("You cast Wrong Spell on ")+Armor.Name,TEXT(""),ACEChatMessageType::Magic);
                TestFalse(TEXT("Unrelated item/spell/sender cannot confirm a self bane"),H->PendingSpellConfirmed);
                Chat(Armor.Name,Suffix);Chat(Shirt.Name,Suffix);
                TestTrue(TEXT("Worn item result confirms the redirected spell"),H->PendingSpellConfirmed);
                H->ObserveUseDone(0);
                TestTrue(TEXT("Acknowledged bane succeeds"),H->LastSpellConfirmed);
                TestEqual(TEXT("Multiple armor results retain one player-targeted family timer"),H->ItemBuffs.Num(),1);
                if(H->ItemBuffs.Num())TestEqual(TEXT("Timer belongs to the requested player"),H->ItemBuffs[0]->AsObject()->GetNumberField(TEXT("target")),double(uint32(Session.PlayerGuid)));
            }
            // Feed the real host timer into UCM, including a manual forced cycle.
            for(bool Forced:{false,true})
            {
                FACEPluginVM VM;if(!TestTrue(*Error,VM.Load(Source,Error)))return false;
                TSharedPtr<FJsonObject> State=MakeShared<FJsonObject>();
                FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT(R"({"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"combat_mode":8,"components_required":false,"inventory":[{"id":99,"type":32768,"equipped":true}],"spells":[]})")),State);
                State->SetNumberField(TEXT("player"),uint32(Session.PlayerGuid));State->SetNumberField(TEXT("time"),FPlatformTime::Seconds());
                State->SetNumberField(TEXT("action_serial"),H->ActionSerial);
                auto Settings=MakeShared<FJsonObject>();Settings->SetBoolField(TEXT("buffing"),true);Settings->SetBoolField(TEXT("recovery"),false);Settings->SetStringField(TEXT("combat"),TEXT("off"));
                auto Info=MakeShared<FJsonObject>();Info->SetNumberField(TEXT("id"),Spell);Info->SetNumberField(TEXT("category"),Category);Info->SetNumberField(TEXT("school"),School);Info->SetNumberField(TEXT("power"),Power);Info->SetNumberField(TEXT("duration"),Duration);Info->SetNumberField(TEXT("skill"),600);Info->SetNumberField(TEXT("target_type"),6);Info->SetBoolField(TEXT("beneficial"),true);
                State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Info)});
                if(Forced){State->SetNumberField(TEXT("force_buff_request"),1);State->SetArrayField(TEXT("item_buffs"),H->ItemBuffs);}
                TSharedPtr<FJsonObject> Intent;TestTrue(*Error,VM.Step(State,Settings,Intent,Error));
                if(!TestTrue(TEXT("Buff cycle issues initial bane"),Intent&&Intent->HasField(TEXT("spell"))))return false;
                Begin(Session.PlayerGuid);Chat(Armor.Name);H->ObserveUseDone(0);
                State->SetNumberField(TEXT("action_serial"),H->ActionSerial);State->SetNumberField(TEXT("last_spell"),Spell);State->SetBoolField(TEXT("last_spell_confirmed"),H->LastSpellConfirmed);State->SetNumberField(TEXT("action_error"),0);State->SetArrayField(TEXT("item_buffs"),H->ItemBuffs);
                TestTrue(*Error,VM.Step(State,Settings,Intent,Error));
                TestFalse(TEXT("Confirmed bane advances instead of repeating"),Intent->HasField(TEXT("spell")));
                if(Forced)TestEqual(TEXT("Force Buff completes after item result"),Intent->GetStringField(TEXT("action")),FString(TEXT("force_buff_done")));
                // A new process has no Lua completion history. Only restored
                // server-confirmed timers must prevent an unnecessary cycle.
                H->DataPlayer=Session.PlayerGuid;H->DataServer=C->GetServerName();H->ItemBuffDeaths=Session.PlayerVitals.NumDeaths;
                H->SaveItemBuffTimers();H->ItemBuffs.Empty();H->LoadItemBuffTimers();
                State->SetArrayField(TEXT("item_buffs"),H->ItemBuffs);State->SetNumberField(TEXT("force_buff_request"),0);
                FACEPluginVM Relog;TestTrue(*Error,Relog.Load(Source,Error));
                TestTrue(*Error,Relog.Step(State,Settings,Intent,Error));
                TestFalse(TEXT("Fresh UCM after login does not repeat an unexpired bane"),Intent->HasField(TEXT("spell")));
                State->SetNumberField(TEXT("force_buff_request"),0);State->SetNumberField(TEXT("time"),FPlatformTime::Seconds()+Duration+1);
                TestTrue(*Error,VM.Step(State,Settings,Intent,Error));TestTrue(TEXT("Bane becomes eligible again when its timer expires"),Intent->HasField(TEXT("spell")));
            }
            Begin(Session.PlayerGuid);H->ObserveUseDone(0);TestTrue(TEXT("UseDone alone cannot manufacture a bane timer"),H->ItemBuffs.IsEmpty());
            Begin(Session.PlayerGuid);Chat(Armor.Name);H->ObserveMetaChat(TEXT("Your spell fizzled."),TEXT(""),ACEChatMessageType::Magic);H->ObserveUseDone(0);
            TestTrue(TEXT("Fizzle does not retain a bane timer"),H->ItemBuffs.IsEmpty());
            Begin(Session.PlayerGuid);Chat(Armor.Name);H->ObserveUseDone(1);TestTrue(TEXT("Failed UseDone cannot retain a bane timer"),H->ItemBuffs.IsEmpty());
            Begin(Armor.Guid);Chat(Shirt.Name);TestFalse(TEXT("An explicit item buff cannot confirm against a different worn item"),H->PendingSpellConfirmed);
            Chat(Armor.Name);H->ObserveUseDone(0);TestTrue(TEXT("Explicit item result still confirms"),H->LastSpellConfirmed);
            const int32 OldDurationAug=Session.PlayerVitals.StatQualityInts.FindRef(238);
            for(int32 Augments:{0,1,5})
            {
                Session.PlayerVitals.StatQualityInts.Add(238,Augments);
                Begin(Session.PlayerGuid);Chat(Armor.Name);H->ObserveUseDone(0);
                TestEqual(TEXT("Confirmed item buff timer includes retail spell-duration augmentations"),
                    H->ItemBuffs[0]->AsObject()->GetNumberField(TEXT("duration")),Duration*(1.+.2*Augments));
            }
            Session.PlayerVitals.StatQualityInts.Add(238,OldDurationAug);
            // Use actual confirmed item timers across a new login/VM. Character
            // buffs remain authoritative server enchantments, not cached guesses.
            H->DataPlayer=Session.PlayerGuid;H->DataServer=C->GetServerName();
            H->ItemBuffDeaths=Session.PlayerVitals.NumDeaths;
            Begin(Armor.Guid);Chat(Armor.Name);H->ObserveUseDone(0);
            auto RelogObjects=MoveTemp(Session.WorldObjects);
            H->SaveItemBuffTimers();H->ItemBuffs.Empty();H->LoadItemBuffTimers();
            TestEqual(TEXT("Owned item timer survives logout cleanup and delayed inventory creates"),H->ItemBuffs.Num(),1);
            Session.WorldObjects=MoveTemp(RelogObjects);
            Begin(Session.PlayerGuid);Chat(Armor.Name);H->ObserveUseDone(0);
            const double OriginalExpiry=H->ItemBuffs[0]->AsObject()->GetNumberField(TEXT("expires"));
            H->SaveItemBuffTimers();H->ItemBuffs.Empty();H->LoadItemBuffTimers();
            TestEqual(TEXT("Confirmed armor timer survives relog"),H->ItemBuffs.Num(),1);
            if(H->ItemBuffs.Num())TestTrue(TEXT("Relog retains remaining time without extending the spell"),
                FMath::Abs(H->ItemBuffs[0]->AsObject()->GetNumberField(TEXT("expires"))-OriginalExpiry)<.01);
            auto NewArmor=Armor;NewArmor.Guid=979;Session.WorldObjects.Add(NewArmor.Guid,NewArmor);
            H->ValidateItemBuffTimers();TestEqual(TEXT("New armor preserves the already buffed equipment"),H->ItemBuffs.Num(),1);
            TestEqual(TEXT("Only new armor is queued for a direct item buff"),H->ItemBuffs[0]->AsObject()->GetArrayField(TEXT("missing_equipment"))[0]->AsNumber(),double(NewArmor.Guid));
            Session.WorldObjects[NewArmor.Guid].WielderId=0;Session.WorldObjects[NewArmor.Guid].ContainerId=Session.PlayerGuid;
            H->ValidateItemBuffTimers();
            TestEqual(TEXT("Equipping a wand cannot forget the uncovered shield or armor"),H->ItemBuffs[0]->AsObject()->GetArrayField(TEXT("missing_equipment")).Num(),1);
            FACEAppraisalInfo Unenchantable;Unenchantable.ObjectGuid=NewArmor.Guid;Unenchantable.bSuccess=true;
            Unenchantable.IntProperties.Add(36,9999);H->ObserveAppraisal(Unenchantable);
            H->ValidateItemBuffTimers();
            TestTrue(TEXT("A late Unenchantable appraisal prunes a carried item's pending bane"),H->ItemBuffs[0]->AsObject()->GetArrayField(TEXT("missing_equipment")).IsEmpty());
            Session.WorldObjects[NewArmor.Guid].WielderId=Session.PlayerGuid;Session.WorldObjects[NewArmor.Guid].ContainerId=0;
            H->ValidateItemBuffTimers();
            TestTrue(TEXT("Reequipping Covenant gear does not queue it again"),H->ItemBuffs[0]->AsObject()->GetArrayField(TEXT("missing_equipment")).IsEmpty());
            Unenchantable.IntProperties[36]=9998;H->ObserveAppraisal(Unenchantable);H->ValidateItemBuffTimers();
            TestEqual(TEXT("Enchantable resistant gear still receives its missing buff"),H->ItemBuffs[0]->AsObject()->GetArrayField(TEXT("missing_equipment")).Num(),1);
            H->Appraisals.Remove(NewArmor.Guid);
            Session.WorldObjects.Remove(NewArmor.Guid);
            H->LoadItemBuffTimers();++Session.PlayerVitals.NumDeaths;H->ValidateItemBuffTimers();
            TestTrue(TEXT("Death invalidates saved item-buff estimates"),H->ItemBuffs.IsEmpty());
            H->LoadItemBuffTimers();TestTrue(TEXT("Invalidated timers cannot reappear on next login"),H->ItemBuffs.IsEmpty());
            --Session.PlayerVitals.NumDeaths;H->ItemBuffDeaths=Session.PlayerVitals.NumDeaths;
        }
    }
    {
        auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
        for(const FString Name:{TEXT("Heal Self I"),TEXT("Revitalize Self I"),TEXT("Mana Boost Self I"),TEXT("Heal Other I")})
        {
            uint32 Spell=0;for(uint32 Id=1;Id<7000;++Id){FString Candidate;uint32 Icon=0;if(Dat->TryGetSpellInfo(Id,Candidate,Icon)&&Candidate==Name){Spell=Id;break;}}
            if(!TestTrue(TEXT("Retail recovery spell exists"),Spell!=0))return false;
            const bool Self=!Name.Contains(TEXT("Other"));
            FACEWorldObject Friend;Friend.Guid=0x71009010;Friend.Name=TEXT("Friend");Session.WorldObjects.Add(Friend.Guid,Friend);
            const FString Vital=Name.StartsWith(TEXT("Heal"))?TEXT("health"):Name.StartsWith(TEXT("Revitalize"))?TEXT("stamina"):TEXT("mana");
            const FString Message=Self?TEXT("You cast ")+Name+TEXT(" and restore 42 points of your ")+Vital+TEXT(".")
                :TEXT("With ")+Name+TEXT(" you restore 42 points of ")+Vital+TEXT(" to Friend.");
            auto Begin=[&]()
            {
                H->PendingSpell=Spell;H->PendingSpellTarget=Self?Session.PlayerGuid:Friend.Guid;H->PendingSpellAt=FPlatformTime::Seconds();
                H->PendingSpellOwner=P->Id;H->PendingSpellConfirmed=false;H->PendingSpellFizzled=false;H->PrepareSpellConfirmation(Name);
            };
            Begin();H->ObserveMetaChat(Message,TEXT("Other caster"),ACEChatMessageType::Magic);
            H->ObserveMetaChat(Message.Replace(*Name,TEXT("Unrelated Spell")),TEXT(""),ACEChatMessageType::Magic);
            H->ObserveMetaChat(Message.Replace(TEXT("42"),TEXT("invalid")),TEXT(""),ACEChatMessageType::Magic);
            if(!Self)H->ObserveMetaChat(Message.Replace(TEXT("Friend."),TEXT("Someone Else.")),TEXT(""),ACEChatMessageType::Magic);
            TestFalse(TEXT("Recovery confirmation rejects unrelated or malformed messages"),H->PendingSpellConfirmed);
            H->ObserveMetaChat(Message,TEXT(""),ACEChatMessageType::Magic);H->ObserveUseDone(0);
            TestTrue(TEXT("VT recovery message confirms the cast"),H->LastSpellConfirmed);
            TestTrue(TEXT("Recovery result releases the host cast delay immediately"),P->NextAction<=FPlatformTime::Seconds());
            Begin();H->ObserveMetaChat(Message,TEXT(""),ACEChatMessageType::Magic);H->ObserveUseDone(1);
            TestFalse(TEXT("Recovery error still overrides chat success"),H->LastSpellConfirmed);
            Session.WorldObjects.Remove(Friend.Guid);
        }
    }
    {
        // Real DAT spells and loopback packets exercise result-driven buff
        // dispatch, including servers which reject casts during recoil.
        const auto SavedPlugin=*P;const auto SavedVitals=Session.PlayerVitals;const auto SavedSpells=Session.KnownSpells;
        const uint32 SavedStance=Session.GetCurrentStance();
        auto* BuffPC=World->SpawnActor<AACEPlayerController>();auto* BuffPawn=World->SpawnActor<APawn>();
        BuffPC->Client=C;
        auto* BuffRoot=NewObject<USceneComponent>(BuffPawn);BuffPawn->SetRootComponent(BuffRoot);BuffRoot->RegisterComponent();BuffPC->Possess(BuffPawn);
        auto* Local=NewObject<ULocalPlayer>(GEngine);GI->AddLocalPlayer(Local,FPlatformUserId::CreateFromInternalId(0));BuffPC->SetPlayer(Local);
        auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("Fast buff receiver"),false);
        auto Address=Sockets->CreateInternetAddr();bool Valid=false;Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
        if(!TestTrue(TEXT("Fast buff test binds loopback"),Receiver&&Receiver->Bind(*Address)))return false;
        Receiver->GetAddress(*Address);Session.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("Fast buff sender"),false);
        Session.ServerC2SAddr=Address;Session.IssacClient=MakeUnique<FACEIsaac>(123u);
        ON_SCOPE_EXIT
        {
            *P=SavedPlugin;Session.PlayerVitals=SavedVitals;Session.KnownSpells=SavedSpells;Session.SetCurrentStance(SavedStance);
            H->PendingSpell=0;H->PendingSpellOwner.Empty();H->PendingBuffResultDriven=false;H->BuffRetryAt=0;
            H->ConfirmedBuffAckAt=0;H->ConfirmedBuffOwner.Empty();H->DeferredBuffIntent.Reset();H->DeferredBuffOwner.Empty();H->FastCastOwner.Empty();H->ItemBuffs.Empty();
            FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);H->DecisionWakeTicker.Reset();
            Session.SocketC2S->Close();Sockets->DestroySocket(Session.SocketC2S);Session.SocketC2S=nullptr;
            Session.ServerC2SAddr.Reset();Session.IssacClient.Reset();Receiver->Close();Sockets->DestroySocket(Receiver);Session.CachedC2SPackets.Reset();
            GI->RemoveLocalPlayer(Local);BuffPC->Destroy();BuffPawn->Destroy();
        };
        {
            FACEWorldObject Scroll;Scroll.Guid=979;Scroll.ContainerId=Session.PlayerGuid;Scroll.ItemType=8192;Scroll.SpellDID=6150;
            Session.WorldObjects.Add(Scroll.Guid,Scroll);P->Permissions.Add(TEXT("loot"));
            auto Read=MakeShared<FJsonObject>();Read->SetStringField(TEXT("action"),TEXT("read"));Read->SetNumberField(TEXT("item"),Scroll.Guid);
            auto UseCount=[&](){int32 Count=0;for(const auto& Packet:Session.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.CanRead(4)&&R.ReadUInt32()==ACEGameAction::Use)++Count;}return Count;};
            for(int Mode:{2,4,8})
            {
                P->CombatTransitionIntent.Reset();P->NextAction=0;Session.ClearPendingUse();Session.PendingCombatMode=0;
                Session.ApplyServerCombatMode(Mode);Session.CachedC2SPackets.Reset();
                H->Execute(*P,Read);
                TestEqual(TEXT("Scroll use waits for peace acknowledgment in every combat mode"),UseCount(),0);
                TestTrue(TEXT("Read intent survives the stance transition"),P->CombatTransitionIntent.IsValid());
                Session.ApplyServerCombatMode(1);H->UpdateCombatTransition(*P);
                TestEqual(TEXT("Confirmed peace sends exactly one scroll Use"),UseCount(),1);
                TestTrue(TEXT("Scroll reading keeps the real server use gate"),Session.IsUseBusy());
                TestEqual(TEXT("Read follows UseDone rather than only a fixed timer"),P->WaitAction,FString(TEXT("use")));
                H->Execute(*P,Read);TestEqual(TEXT("Pending read cannot be duplicated"),UseCount(),1);
            }
            Session.ClearPendingUse();Session.WorldObjects.Remove(Scroll.Guid);Session.CachedC2SPackets.Reset();P->WaitAction.Empty();P->NextAction=0;
        }
        auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();int32 Focus=0,Self=0;
        for(uint32 Id=1;Id<7000&&(!Focus||!Self);++Id)
        {FString Name;uint32 Icon;if(Dat->TryGetSpellInfo(Id,Name,Icon)){if(Name==TEXT("Focus Self I"))Focus=Id;if(Name==TEXT("Willpower Self I"))Self=Id;}}
        if(!TestTrue(TEXT("Retail focus and self buffs found"),Focus&&Self))return false;
        P->CombatTransitionIntent.Reset();Session.PendingCombatMode=0;Session.KnownSpells={Focus,Self};Session.ApplyServerCombatMode(8);Session.PlayerVitals.Health=100;
        Session.SetCurrentStance(ACEMotion::StanceMagic); // the accompanying motion update, independent of CombatMode
        Session.bUseBusy=false;Session.PendingEquipmentGuid=0;P->Running=true;P->Player=Session.PlayerGuid;P->Server=C->GetServerName();
        P->Permissions.Add(TEXT("cast"));P->Permissions.Add(TEXT("navigation"));P->WaitAction.Empty();P->NextAction=0;
        auto Intent=[&](int32 Spell)
        {
            auto I=MakeShared<FJsonObject>();I->SetStringField(TEXT("action"),TEXT("cast"));I->SetStringField(TEXT("activity"),TEXT("buffs"));
            I->SetNumberField(TEXT("spell"),Spell);I->SetNumberField(TEXT("target"),uint32(Session.PlayerGuid));I->SetBoolField(TEXT("fast_cast"),true);return I;
        };
        auto Result=[&](int32 Spell)
        {FString Name;uint32 Icon;Dat->TryGetSpellInfo(Spell,Name,Icon);H->ObserveMetaChat(TEXT("You cast ")+Name+TEXT(" on yourself."),TEXT(""),ACEChatMessageType::Magic);};
        auto Ack=[&](uint32 Error){FACEBinaryWriter Packet;Packet.WriteUInt32(Error);FACEBinaryReader R(Packet.GetData());Session.HandleUseDone(R);};
        auto Drive=[&](){float F=0,R=0,T=0;H->ApplyMovement(BuffPC,F,R,T,false,false,false,FVector::ForwardVector);return F;};
        auto CastCount=[&](){int32 Count=0;for(const auto& Packet:Session.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.CanRead(4)&&R.ReadUInt32()==ACEGameAction::CastTargetedSpell)++Count;}return Count;};
        {
            // ACE defaults Attackable to true even for inventory items. A new
            // shield needs a direct bane after the wand replaces it; its old
            // world position must never be used as a monster sight target.
            int32 Bane=0;
            for(uint32 Id=1;Id<7000;++Id)
            {FString Name;uint32 Icon;if(Dat->TryGetSpellInfo(Id,Name,Icon)&&Name==TEXT("Olthoi's Bane")){Bane=Id;break;}}
            if(!TestTrue(TEXT("Reported retail Olthoi's Bane is available"),Bane!=0))return false;
            Session.KnownSpells.Add(Bane);Session.KnownSpells.Add(27); // Flame Bolt I
            FACEWorldObject Shield;Shield.Guid=0x7100a001;Shield.Name=TEXT("Tower Shield");Shield.ItemType=ACEItemType::Armor;
            Shield.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;Shield.ValidLocations=ACEEquipMask::Shield;
            Shield.Position=Session.PlayerPosition;Shield.Position.Location.X+=10;
            const FVector Here=Session.PlayerPosition.ToUnrealLocation(),There=Shield.Position.ToUnrealLocation();
            auto* Wall=World->SpawnActor<AActor>();auto* Obstacle=NewObject<UBoxComponent>(Wall);
            Wall->SetRootComponent(Obstacle);Obstacle->SetBoxExtent(FVector(10,300,300));
            Obstacle->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Obstacle->SetCollisionResponseToAllChannels(ECR_Block);
            Obstacle->RegisterComponent();Wall->SetActorLocation((Here+There)*.5+FVector(0,0,200));
            ON_SCOPE_EXIT
            {
                Wall->Destroy();Session.WorldObjects.Remove(Shield.Guid);Session.WorldObjects.Remove(Shield.Guid+1);
                Session.KnownSpells={Focus,Self};Session.CachedC2SPackets.Reset();H->ItemBuffs.Empty();
                H->PendingSpell=0;H->PendingSpellOwner.Empty();H->PendingBuffResultDriven=false;
                H->ConfirmedBuffAckAt=0;H->ConfirmedBuffOwner.Empty();P->NextAction=0;
            };
            for(bool Worn:{true,false})
            {
                Shield.WielderId=Worn?Session.PlayerGuid:0;Shield.ContainerId=Worn?0:Session.PlayerGuid;
                Shield.CurrentWieldedLocation=Worn?ACEEquipMask::Shield:0;
                Session.WorldObjects.Add(Shield.Guid,Shield);
                TestTrue(TEXT("Reported shield carries ACE's default Attackable flag"),Shield.IsAttackable());
                TestFalse(TEXT("Shield's stale world position is occluded in the regression fixture"),H->HasClearCombatSight(Shield));
                auto Buff=Intent(Bane);Buff->SetNumberField(TEXT("target"),uint32(Shield.Guid));Buff->SetBoolField(TEXT("fast_cast"),false);
                P->NextAction=0;const int32 Before=CastCount();H->Execute(*P,Buff);
                TestEqual(TEXT("Shield bane sends despite an occluded stale inventory position"),CastCount(),Before+1);
                TestEqual(TEXT("Shield buff tracks the actual item recipient"),H->PendingSpellTarget,Shield.Guid);
                if(H->PendingSpell==Bane)
                {
                    H->ObserveMetaChat(TEXT("You cast Olthoi's Bane on Tower Shield, refreshing Olthoi's Bane"),TEXT(""),ACEChatMessageType::Magic);
                    Ack(0);
                    TestTrue(TEXT("Shield confirmation releases casting and the acknowledgment gate"),H->LastSpellConfirmed&&!H->PendingSpell&&!H->ConfirmedBuffAckAt);
                    TestTrue(TEXT("Shield receives its own confirmed bane timer"),H->ItemBuffs.ContainsByPredicate([&](const auto& E)
                    {return E->AsObject()->GetNumberField(TEXT("target"))==double(uint32(Shield.Guid))&&E->AsObject()->GetNumberField(TEXT("spell"))==Bane;}));
                }
            }
            FACEWorldObject Monster=Shield;Monster.Guid++;Monster.Name=TEXT("Occluded monster");Monster.ItemType=ACEItemType::Creature;
            Monster.WielderId=Monster.ContainerId=Monster.CurrentWieldedLocation=0;Monster.bHasPosition=true;Session.WorldObjects.Add(Monster.Guid,Monster);
            auto AttackSpell=Intent(27);AttackSpell->SetStringField(TEXT("activity"),TEXT("combat"));AttackSpell->SetNumberField(TEXT("target"),uint32(Monster.Guid));
            P->NextAction=0;const int32 Before=CastCount();H->Execute(*P,AttackSpell);
            TestEqual(TEXT("Real hostile creatures still require line of sight"),CastCount(),Before);
            TestTrue(TEXT("Occluded monster rejection does not start a cast"),!H->PendingSpell&&P->Status.Contains(TEXT("obstruction")));
            // A walking rat can turn a corner between sparse F748 positions.
            // Keep the packet in the open while its actual body is behind the wall.
            auto* Rat=World->SpawnActor<AACEWorldEntityActor>();
            Rat->InitializeFromObject(Monster,100.f,false);
            ON_SCOPE_EXIT {Rat->Destroy();};
            Session.WorldObjects[Monster.Guid].Position=Session.PlayerPosition;
            Session.WorldObjects[Monster.Guid].Position.Location.Y+=10;
            const auto& Stale=Session.WorldObjects[Monster.Guid];
            TestFalse(TEXT("Combat sight follows the rat behind the corner, not its stale network position"),H->HasClearCombatSight(Stale));
            auto TargetSight=[&]()
            {
                auto Snapshot=H->Snapshot();
                for(const auto& V:Snapshot->GetArrayField(TEXT("targets")))
                    if(V->AsObject()->GetNumberField(TEXT("id"))==uint32(Monster.Guid))return V->AsObject()->GetBoolField(TEXT("line_of_sight"));
                AddError(TEXT("Corner fixture target is missing"));return true;
            };
            TestFalse(TEXT("Policy snapshot excludes the rat that moved behind the corner"),TargetSight());
            P->NextAction=0;H->Execute(*P,AttackSpell);
            TestEqual(TEXT("Stale-position attack sends no spell packet"),CastCount(),Before);
            if(H->PendingSpell) {Ack(0);H->PendingSpell=0;H->PendingSpellOwner.Empty();}
            Rat->SetActorLocation(Stale.Position.ToUnrealLocation());
            TestTrue(TEXT("Rat becomes eligible as soon as its live body clears the wall"),TargetSight());
            // The target can move after the Lua decision, before dispatch.
            Rat->SetActorLocation(Monster.Position.ToUnrealLocation());
            const int32 BeforeDispatch=CastCount();P->NextAction=0;H->Execute(*P,AttackSpell);
            TestEqual(TEXT("Final dispatch rechecks live obstruction"),CastCount(),BeforeDispatch);
            if(H->PendingSpell) {Ack(0);H->PendingSpell=0;H->PendingSpellOwner.Empty();}
            Wall->SetActorLocation((Here+There)*.5+FVector(0,305,200));
            TestFalse(TEXT("Policy excludes a target with a grazing projectile path"),TargetSight());
            for(int Attempt=0;Attempt<3;++Attempt)
            {
                P->NextAction=0;H->Execute(*P,AttackSpell);
                TestEqual(TEXT("Repeated grazing-corner intents never send a spell"),CastCount(),BeforeDispatch);
                TestEqual(TEXT("Blocked shots never start awaiting spell confirmation"),H->PendingSpell,0);
            }
            Wall->SetActorLocation((Here+There)*.5+FVector(0,340,200));
            TestTrue(TEXT("Policy restores the target after adequate corner clearance"),TargetSight());
            P->NextAction=0;H->Execute(*P,AttackSpell);
            TestEqual(TEXT("Clear path sends exactly one cast"),CastCount(),BeforeDispatch+1);
            Ack(0);H->PendingSpell=0;H->PendingSpellOwner.Empty();
        }
        struct FTapPacket {uint32 Command,Hold,Stance;TArray<uint8> PositionAndStamps;};
        auto MovementPackets=[&]()
        {
            TArray<FTapPacket> Result;TArray<uint32> Keys;Session.CachedC2SPackets.GetKeys(Keys);Keys.Sort();
            for(uint32 Key:Keys)
            {
                FACEBinaryReader R(Session.CachedC2SPackets[Key].Payload);R.Skip(24);
                if(!R.CanRead(4)||R.ReadUInt32()!=ACEGameAction::MoveToState)continue;
                const uint32 Flags=R.ReadUInt32();FTapPacket Packet;Packet.Hold=R.ReadUInt32();Packet.Stance=R.ReadUInt32();
                Packet.Command=(Flags&ACERawMotionFlags::ForwardCommand)?R.ReadUInt32():ACEMotion::Ready;
                if(Flags&ACERawMotionFlags::ForwardHoldKey)R.ReadUInt32();
                if(Flags&ACERawMotionFlags::ForwardSpeed)R.ReadFloat();
                Packet.PositionAndStamps=R.ReadBytes(R.Remaining());Result.Add(MoveTemp(Packet));
            }
            return Result;
        };
        const int32 InitialMoves=MovementPackets().Num();
        {
            FACEWorldObject Weapon;Weapon.Guid=978;Weapon.ContainerId=Session.PlayerGuid;Weapon.ItemType=ACEItemType::MeleeWeapon;
            Session.WorldObjects.Add(Weapon.Guid,Weapon);
            Session.SendPutItemInContainer(Weapon.Guid,Session.PlayerGuid,0);
            const int32 Before=CastCount();H->Execute(*P,Intent(Focus));
            TestTrue(TEXT("A manual weapon move waits for containment before UCM sends a competing action"),
                Session.IsInventoryTransferPending()&&CastCount()==Before&&!H->PendingSpell);
            ConfirmPut(Weapon.Guid,Session.PlayerGuid);
            TestFalse(TEXT("Server receipt promptly releases manual inventory arbitration"),H->InventoryInteractionPending());
            TestTrue(TEXT("Weapon remains owned after its move"),C->IsOwnedInventoryItem(Session.WorldObjects[Weapon.Guid]));
            Session.WorldObjects.Remove(Weapon.Guid);
        }
        H->Execute(*P,Intent(Focus));TestTrue(TEXT("Buff dispatch enables result-driven pipeline"),H->PendingBuffResultDriven&&H->PendingSpell==Focus);
        H->ObserveMetaChat(TEXT("words"),TEXT("Caster"),ACEChatMessageType::Spellcasting);Result(Focus);
        TestEqual(TEXT("Coalesced buff result waits for the cancellation movement edge"),H->PendingSpell,Focus);
        TestEqual(TEXT("Result-driven cast has no backward locomotion frame"),Drive(),0.f);
        TestTrue(TEXT("Result-driven cast consumes the tap before advancing"),H->FastCastMovementApplied);
        const auto Tap=MovementPackets();
        if(TestEqual(TEXT("One tap sends exactly two ordinary MoveToState packets"),Tap.Num(),InitialMoves+2))
        {
            const auto& Press=Tap[InitialMoves];const auto& Release=Tap[InitialMoves+1];
            TestEqual(TEXT("Tap presses the retail backward command"),Press.Command,ACEMotion::WalkBackwards);
            TestEqual(TEXT("Tap releases to Ready in the same update"),Release.Command,ACEMotion::Ready);
            TestTrue(TEXT("Tap preserves magic stance and the current hold key"),Press.Stance==ACEMotion::StanceMagic&&Release.Stance==Press.Stance&&Release.Hold==Press.Hold);
            TestTrue(TEXT("Both edges report identical position, orientation and movement stamps"),Press.PositionAndStamps==Release.PositionAndStamps);
            TestFalse(TEXT("Session finishes the tap with no held movement"),Session.bMoving);
        }
        TestTrue(TEXT("Confirmed buff advances before late UseDone"),!H->PendingSpell&&H->ConfirmedBuffAckAt&&H->LastSpellConfirmed&&P->NextAction<=FPlatformTime::Seconds());
        TestEqual(TEXT("Following frame remains stationary"),Drive(),0.f);
        TestEqual(TEXT("Following frame cannot repeat the tap packets"),MovementPackets().Num(),InitialMoves+2);
        H->Execute(*P,Intent(Self));TestEqual(TEXT("Next buff sends without waiting through recoil"),CastCount(),2);
        const auto Serial=H->ActionSerial;
        Ack(0x001D);TestTrue(TEXT("Busy during recoil preserves second buff and schedules retry"),H->PendingSpell==Self&&H->BuffRetryAt&&H->ActionSerial==Serial&&H->ConfirmedBuffAckAt);
        Ack(0);TestTrue(TEXT("Old success only drains the first buff acknowledgment"),!H->ConfirmedBuffAckAt&&H->PendingSpell==Self&&H->ActionSerial==Serial);
        H->UpdateBuffDispatch();TestEqual(TEXT("Draining recoil promptly retries the rejected buff"),CastCount(),3);
        H->UpdateBuffDispatch();TestEqual(TEXT("Accepted retry is not resent without a rejection"),CastCount(),3);
        H->ObserveMetaChat(TEXT("words"),TEXT("Caster"),ACEChatMessageType::Spellcasting);Drive();Result(Self);
        TestTrue(TEXT("Retry result confirms once and clears movement"),H->ActionSerial==Serial+1&&H->LastCompletedSpell==Self&&H->LastSpellConfirmed&&Drive()==0);
        auto FinishNotice=MakeShared<FJsonObject>();FinishNotice->SetStringField(TEXT("action"),TEXT("notice"));FinishNotice->SetStringField(TEXT("text"),TEXT("Buff pipeline complete"));
        H->Execute(*P,FinishNotice);TestTrue(TEXT("Non-buff intent waits for outstanding untagged acknowledgment"),H->DeferredBuffIntent.IsValid());
        Ack(0x001D);TestTrue(TEXT("Unrelated rejected manual use cannot drain a successful buff acknowledgment"),H->ConfirmedBuffAckAt&&H->DeferredBuffIntent.IsValid());
        Ack(0);H->NextMaintenanceAt=FPlatformTime::Seconds()+60;H->Tick(.016f);
        TestTrue(TEXT("Deferred intent runs exactly after acknowledgment"),!H->DeferredBuffIntent&&H->Notice==TEXT("Buff pipeline complete"));
        // Missing trailing UseDone must not silently discard the policy's next
        // action after it has already advanced its own state machine.
        for(bool ForceDone:{false,true})
        {
            P->NextAction=0;H->Execute(*P,Intent(Focus));Result(Focus);
            auto Queued=MakeShared<FJsonObject>(*FinishNotice);
            if(ForceDone)
            {
                H->ForceBuffRequest=1234;H->bForceBuffOnly=false;
                Queued->SetStringField(TEXT("action"),TEXT("force_buff_done"));Queued->SetNumberField(TEXT("request"),1234);
            }
            H->Execute(*P,Queued);H->Notice.Empty();
            TestTrue(TEXT("Status identifies missing final buff acknowledgment"),H->UCMCurrentStatus().Contains(TEXT("final buff acknowledgment")));
            H->ConfirmedBuffAckAt=FPlatformTime::Seconds()-31;H->UpdateBuffDispatch();
            TestTrue(TEXT("Ack timeout preserves queued action for revalidation"),H->DeferredBuffIntent.IsValid());
            H->Tick(.016f);
            TestFalse(TEXT("Queued action is consumed after timeout without restarting"),H->DeferredBuffIntent.IsValid());
            if(ForceDone)TestEqual(TEXT("Timed-out final ack cannot restart a completed Force Buff cycle"),H->ForceBuffRequest,uint32(0));
            else TestEqual(TEXT("Queued action survives missing ack"),H->Notice,FString(TEXT("Buff pipeline complete")));
        }
        // When queue support accepts a second buff, even an early second result
        // cannot create two acknowledgment debts or complete an unrelated action.
        P->NextAction=0;H->Execute(*P,Intent(Focus));Result(Focus);H->Execute(*P,Intent(Self));
        Result(Self);TestTrue(TEXT("Only one confirmed cast may be awaiting its acknowledgment"),H->ConfirmedBuffAckAt&&H->PendingSpell==Self);
        Ack(0);TestEqual(TEXT("First queued acknowledgment cannot finish the second buff"),H->PendingSpell,Self);
        Ack(0);TestTrue(TEXT("Second queued acknowledgment finishes the second buff"),!H->PendingSpell&&!H->ConfirmedBuffAckAt&&H->LastSpellConfirmed);
        // Standard ack-only completion and fizzle remain conservative.
        P->NextAction=0;H->Execute(*P,Intent(Focus));Ack(0);
        TestFalse(TEXT("UseDone alone never confirms a buff"),H->LastSpellConfirmed);
        P->NextAction=0;H->Execute(*P,Intent(Focus));H->ObserveMetaChat(TEXT("Your spell fizzled."),TEXT(""),ACEChatMessageType::Magic);Ack(0);
        TestTrue(TEXT("Fizzle creates no acknowledgment debt"),!H->ConfirmedBuffAckAt&&!H->LastSpellConfirmed&&H->FastCastOwner.IsEmpty());
        P->NextAction=0;H->Execute(*P,Intent(Focus));H->PendingSpellAt=FPlatformTime::Seconds()-6;Ack(0x001D);
        TestTrue(TEXT("Busy retry window is bounded"),!H->PendingSpell&&!H->BuffRetryAt&&H->LastActionError==0x001D);
        P->NextAction=0;H->Execute(*P,Intent(Focus));Result(Focus);H->Execute(*P,FinishNotice);H->Stop(P->Id);
        TestTrue(TEXT("Stop discards deferred actions but retains late acknowledgment guard"),!H->DeferredBuffIntent&&H->ConfirmedBuffAckAt&&!H->BuffRetryAt);
        Ack(0);TestFalse(TEXT("Late acknowledgment after Stop is drained"),H->ConfirmedBuffAckAt!=0);
        // Death wakes bypass only the old physical attack retry interval, and
        // use the same packet path for melee and missile on the next opponent.
        P->Running=true;P->Permissions.Add(TEXT("combat"));P->WaitAction.Empty();
        FACEWorldObject Enemy;Enemy.Guid=970;Enemy.Name=TEXT("Target one");Enemy.ItemType=ACEItemType::Creature;
        Enemy.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;Enemy.bHasPosition=true;Enemy.Position=Session.PlayerPosition;Enemy.Position.Location.X+=2;
        FACEWorldObject NextEnemy=Enemy;NextEnemy.Guid=971;NextEnemy.Name=TEXT("Target two");NextEnemy.Position.Location.Y+=2;
        ON_SCOPE_EXIT{Session.WorldObjects.Remove(970);Session.WorldObjects.Remove(971);H->CombatTarget=0;H->DefeatedTargets.Empty();H->LastCombatDeath.Reset();H->PhysicalAttackTarget=0;H->PhysicalAttackOwner.Empty();Session.SelectedObject.Guid=0;};
        auto AttackIntent=[&](int32 Target,int32 Mode)
        {auto I=MakeShared<FJsonObject>();I->SetStringField(TEXT("action"),TEXT("attack"));I->SetStringField(TEXT("activity"),TEXT("combat"));I->SetNumberField(TEXT("target"),Target);I->SetNumberField(TEXT("mode"),Mode);return I;};
        auto AttackCount=[&](){int32 Count=0;for(const auto& Packet:Session.CachedC2SPackets){FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.CanRead(4)){const uint32 Op=R.ReadUInt32();if(Op==ACEGameAction::TargetedMeleeAttack||Op==ACEGameAction::TargetedMissileAttack)++Count;}}return Count;};
        {
            uint32 Imperil=0;for(uint32 Id=1;Id<7000;++Id){FString Name;uint32 Icon=0;if(Dat->TryGetSpellInfo(Id,Name,Icon)&&Name==TEXT("Imperil Other I")){Imperil=Id;break;}}
            if(!TestTrue(TEXT("Proc fixture finds retail Imperil"),Imperil!=0))return false;
            const bool WasGDLE=Session.Creds.bGDLE;
            ON_SCOPE_EXIT{Session.Creds.bGDLE=WasGDLE;Session.WorldObjects.Remove(9391);H->Appraisals.Remove(9391);H->PendingDebuffCasts.Empty();H->PhysicalAttackOwner.Empty();P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));Session.CachedC2SPackets.Reset();};
            for(bool GDLE:{false,true})for(int32 Mode:{2,4})
            {
                Session.Creds.bGDLE=GDLE;Session.WorldObjects.Add(970,Enemy);H->DefeatedTargets.Empty();Session.PendingCombatMode=0;Session.ApplyServerCombatMode(Mode);
                FACEWorldObject Proc;Proc.Guid=9391;Proc.Name=TEXT("Proc weapon");Proc.ItemType=Mode==2?ACEItemType::MeleeWeapon:ACEItemType::MissileWeapon;
                Proc.WielderId=Session.PlayerGuid;Proc.ValidLocations=Proc.CurrentWieldedLocation=Mode==2?ACEEquipMask::MeleeWeapon:ACEEquipMask::MissileWeapon;
                Session.WorldObjects.Add(Proc.Guid,Proc);FACEAppraisalInfo A;A.bSuccess=true;A.SpellIds={int32(Imperil)};A.IntProperties.Add(106,500);H->Appraisals.Add(Proc.Guid,A);
                auto I=AttackIntent(970,Mode);I->SetNumberField(TEXT("item"),Proc.Guid);I->SetNumberField(TEXT("debuff_spell"),Imperil);I->SetNumberField(TEXT("power"),Mode==2?0:1);
                auto Dispatch=[&](){P->NextAction=0;P->WaitAction.Empty();H->PhysicalAttackOwner.Empty();Session.CachedC2SPackets.Reset();Session.ClearPendingUse();H->Execute(*P,I);};
                Dispatch();TestEqual(TEXT("Proc uses normal ACE/GDLE physical attack packets"),AttackCount(),1);
                TestTrue(TEXT("Sending a proc attack does not predict debuff success"),!H->PendingDebuffCasts.IsEmpty());
                H->PendingDebuffCasts.Empty();P->NextAction=0;H->Execute(*P,I);
                TestEqual(TEXT("Existing repeating attack does not need a second proc packet"),AttackCount(),1);
                TestTrue(TEXT("Existing attack still registers proc confirmation tracking"),!H->PendingDebuffCasts.IsEmpty());
                const auto Revision=H->DebuffRevision;
                H->ObserveMetaChat(TEXT("You cast Imperil Other I on Target one."),TEXT(""),ACEChatMessageType::Magic);
                TestTrue(TEXT("Server proc result confirms the tracked target and spell"),H->DebuffRevision>Revision&&H->LastDebuffTarget==970&&H->LastDebuffSpell==int32(Imperil));
                H->Debuffs.Empty();
                I->SetNumberField(TEXT("debuff_spell"),Imperil+1);Dispatch();TestEqual(TEXT("Unrelated proc spell cannot be requested"),AttackCount(),0);I->SetNumberField(TEXT("debuff_spell"),Imperil);
                P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));
                Session.WorldObjects[Proc.Guid].CurrentWieldedLocation=ACEEquipMask::Shield;Dispatch();TestEqual(TEXT("Offhand proc cannot masquerade as the main weapon"),AttackCount(),0);
                P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));
                Session.WorldObjects[Proc.Guid]=Proc;H->Appraisals[Proc.Guid].IntProperties.Add(109,10000);Dispatch();TestEqual(TEXT("Changed activation requirements block proc dispatch"),AttackCount(),0);
                P->ActivityFailure.Reset();P->Profile->RemoveField(TEXT("ucm_activity_failure"));
            }
        }
        for(int32 Mode:{2,4})
        {
            Session.WorldObjects.Add(970,Enemy);Session.WorldObjects.Add(971,NextEnemy);H->DefeatedTargets.Empty();H->LastCombatDeath.Reset();H->PhysicalAttackOwner.Empty();
            Session.PendingCombatMode=0;Session.ApplyServerCombatMode(Mode);Session.bServerAttackInProgress=false;Session.LastAttackError=0x20;P->NextAction=0;H->Execute(*P,AttackIntent(970,Mode));
            const int32 Before=AttackCount();TestTrue(TEXT("Physical attack establishes a tracked target"),H->CombatTarget==970&&H->PhysicalAttackTarget==970);
            auto Changed=Session.PlayerVitals;Changed.Health=40;H->LastCombatVitals=FIntVector(-1,-1,-1);
            Session.OnVitalsUpdated.Broadcast(Changed);
            TestTrue(TEXT("Vital changes release physical throttle for immediate recovery decision"),P->NextAction==0&&P->NextDecision==0&&H->DecisionWakeAt<=FPlatformTime::Seconds());
            H->Execute(*P,AttackIntent(970,Mode));TestEqual(TEXT("Checking recovery without needing it does not restart server attack"),AttackCount(),Before);
            const double Throttle=P->NextAction;Session.OnVitalsUpdated.Broadcast(Changed);
            TestEqual(TEXT("Identical vital updates do not continually wake policy"),P->NextAction,Throttle);
            FACEBinaryWriter Health;Health.WriteUInt32(970);Health.WriteFloat(0);FACEBinaryReader HR(Health.GetData());Session.HandleUpdateHealth(HR);
            TestTrue(TEXT("Zero-health network event bypasses remaining physical attack wait"),P->NextAction==0&&H->CombatTarget==0&&H->DefeatedTargets.Contains(970));
            if(TestTrue(TEXT("Confirmed death retains a corpse-spawn handoff"),H->LastCombatDeath.IsValid()))
            {
                TestEqual(TEXT("Handoff identifies the defeated enemy"),H->LastCombatDeath->GetNumberField(TEXT("id")),970.);
                const double DeathAt=H->LastCombatDeath->GetNumberField(TEXT("time"));
                Session.OnObjectHealth.Broadcast(970,0);
                TestEqual(TEXT("Repeated zero-health packets do not extend the wait"),H->LastCombatDeath->GetNumberField(TEXT("time")),DeathAt);
            }
            // Even after a kill wakes the policy, a replacement attack must
            // still pass physical sight at dispatch time. This wall ignores
            // camera traces just like doors; no missile or charge may escape.
            auto* Wall=World->SpawnActor<AActor>();auto* Obstacle=NewObject<UBoxComponent>(Wall);
            Wall->SetRootComponent(Obstacle);Obstacle->SetBoxExtent(FVector(20,300,300));
            Obstacle->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Obstacle->SetCollisionResponseToAllChannels(ECR_Block);
            Obstacle->SetCollisionResponseToChannel(ECC_Camera,ECR_Ignore);Obstacle->RegisterComponent();
            Wall->SetActorLocation((Session.PlayerPosition.ToUnrealLocation()+NextEnemy.Position.ToUnrealLocation())*.5+FVector(0,0,120));
            TestFalse(TEXT("Next opponent is physically behind the wall"),H->HasClearCombatSight(NextEnemy));
            H->Execute(*P,AttackIntent(971,Mode));
            TestEqual(TEXT("Post-kill plugin attack cannot bypass a wall"),AttackCount(),Before);
            Obstacle->SetCollisionEnabled(ECollisionEnabled::NoCollision);Wall->Destroy();
            P->NextAction=0; // advance past the rejected action's normal decision throttle
            H->Execute(*P,AttackIntent(971,Mode));TestEqual(TEXT("Visible next opponent attack is sent immediately after death"),AttackCount(),Before+1);
            auto State=H->Snapshot();bool DeadInSnapshot=false;for(const auto& V:State->GetArrayField(TEXT("targets")))DeadInSnapshot|=V->AsObject()->GetNumberField(TEXT("id"))==970;
            TestTrue(TEXT("Policy snapshot includes confirmed death for post-combat loot"),State->HasTypedField<EJson::Object>(TEXT("combat_death")));
            TestFalse(TEXT("Dead opponent stays excluded after selection moves to next opponent"),DeadInSnapshot);
            Session.WorldObjects.Remove(971);Session.OnObjectDeleted.Broadcast(971);
            TestTrue(TEXT("Removal without health update also releases target throttle"),H->CombatTarget==0&&P->NextAction==0);
            TestEqual(TEXT("Visibility deletion is not another confirmed kill"),H->LastCombatDeath->GetNumberField(TEXT("id")),970.);
        }
        Session.WorldObjects.Add(970,Enemy);Session.OnObjectCreated.Broadcast(Enemy);
        TestFalse(TEXT("Newly created object with reused GUID does not inherit dead filter"),H->DefeatedTargets.Contains(970));
        TestFalse(TEXT("Reused GUID clears the old corpse-spawn handoff"),H->LastCombatDeath.IsValid());
        H->CombatTarget=970;H->PendingSpell=Focus;H->PendingSpellOwner=P->Id;H->PendingSpellAt=FPlatformTime::Seconds();P->NextAction=H->PendingSpellAt+3.5;
        const double CastingGate=P->NextAction;Session.WorldObjects[970].bDying=true;FACEObjectMotionState Death;Death.ForwardCommand=ACEMotion::Dead;
        Session.OnMotionUpdate.Broadcast(970,Death);
        TestTrue(TEXT("Death motion wakes targeting but preserves outstanding spell acknowledgment"),H->CombatTarget==0&&H->PendingSpell==Focus&&P->NextAction==CastingGate);
        TestTrue(TEXT("Death motion also records a corpse-spawn handoff"),H->LastCombatDeath.IsValid());
        Ack(0);P->WaitAction=TEXT("equip");P->NextAction=FPlatformTime::Seconds()+2;H->PhysicalAttackOwner=P->Id;H->CombatTarget=970;
        const double EquipmentGate=P->NextAction;H->LastCombatVitals=FIntVector(-1,-1,-1);Session.OnVitalsUpdated.Broadcast(Session.PlayerVitals);
        TestEqual(TEXT("Recovery wake cannot bypass pending equipment"),P->NextAction,EquipmentGate);
        P->WaitAction.Empty();P->NextAction=0;
        {
            // Exercise the shipped Lua policy through Tick and network death/ID
            // callbacks, not just direct Execute calls with a prechosen target.
            const auto SavedObjects=Session.WorldObjects;const auto SavedAppraisals=H->Appraisals;
            const auto SavedRequests=H->AppraisalRequests;
            ON_SCOPE_EXIT
            {
                Session.WorldObjects=SavedObjects;H->Appraisals=SavedAppraisals;H->AppraisalRequests=SavedRequests;
                H->InventoryRevision=MAX_uint64;H->CombatTarget=0;H->PhysicalAttackOwner.Empty();H->PhysicalAttackTarget=0;
                H->PendingSpell=0;H->PendingSpellOwner.Empty();H->DefeatedTargets.Empty();
            };
            FString Source,Error;FFileHelper::LoadFileToString(Source,*(P->Directory/TEXT("main.lua")));
            int32 Bolt=0;
            for(uint32 Id=1;Id<7000&&!Bolt;++Id){FString Name;uint32 Icon;if(Dat->TryGetSpellInfo(Id,Name,Icon)&&Name==TEXT("Acid Stream I"))Bolt=Id;}
            if(!TestTrue(TEXT("Retail offensive spell found for target handoff"),Bolt!=0))return false;
            auto DeliverWake=[&](){FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);H->RunDecisionWake(.016f);};
            for(int32 Mode:{2,4,8})for(int32 AppraisalResult:{-1,0,1})for(bool Approach:{false,true})
            {
                BuffPC->EndUseApproach();
                P->VM=MakeShared<FACEPluginVM>();TestTrue(TEXT("Load shipped UCM for network handoff"),P->VM->Load(Source,Error));
                P->Profile=MakeShared<FJsonObject>();P->Profile->SetStringField(TEXT("combat"),Mode==2?TEXT("melee"):Mode==4?TEXT("missile"):TEXT("magic"));
                P->Profile->SetBoolField(TEXT("manual_combat"),true);P->Profile->SetBoolField(TEXT("buffing"),false);
                P->Profile->SetBoolField(TEXT("recovery"),false);P->Profile->SetBoolField(TEXT("looting"),false);
                P->Permissions.Add(TEXT("inventory"));P->NextAction=0;P->NextDecision=0;P->WaitAction.Empty();P->Running=true;
                Session.PendingCombatMode=0;Session.ApplyServerCombatMode(Mode);Session.bServerAttackInProgress=false;Session.LastAttackError=0x20;Session.PlayerVitals.QualityBools.Add(68,0);
                Session.PlayerVitals.Health=Session.PlayerVitals.MaxHealth=100;Session.PlayerVitals.Stamina=Session.PlayerVitals.MaxStamina=100;
                Session.PlayerVitals.Mana=Session.PlayerVitals.MaxMana=100;
                FACESkillInfo Skill;Skill.SkillId=34;Skill.AdvancementClass=2;Skill.Current=500;Session.PlayerVitals.Skills={Skill};Session.KnownSpells={Bolt};
                auto FirstEnemy=Enemy;FirstEnemy.Position=Session.PlayerPosition;FirstEnemy.Position.Location.X+=1;
                auto CloseEnemy=NextEnemy;CloseEnemy.Position=Session.PlayerPosition;CloseEnemy.Position.Location.X+=1.5;
                Session.WorldObjects.Empty();Session.WorldObjects.Add(970,FirstEnemy);Session.WorldObjects.Add(971,CloseEnemy);
                FACEWorldObject Weapon;Weapon.Guid=972;Weapon.Name=TEXT("Equipped weapon");Weapon.ItemType=Mode==2?1:Mode==4?256:32768;
                Weapon.WielderId=Session.PlayerGuid;Weapon.CurrentWieldedLocation=0x1000000;Session.WorldObjects.Add(972,Weapon);
                H->Appraisals.Empty();FACEAppraisalInfo Info;Info.bSuccess=true;Info.ObjectGuid=970;H->Appraisals.Add(970,Info);
                Info.ObjectGuid=972;Info.Damage=30;Info.DamageType=1;Info.bHasWeaponProfile=true;H->Appraisals.Add(972,Info);
                H->InventoryRevision=MAX_uint64;H->CombatTarget=0;H->PhysicalAttackTarget=0;H->PhysicalAttackOwner.Empty();H->DefeatedTargets.Empty();
                H->PendingSpell=0;Session.bUseBusy=false;Session.SelectedObject.Guid=0;
                H->NextMaintenanceAt=FPlatformTime::Seconds()+60;H->Tick(.016f);
                TestEqual(*FString::Printf(TEXT("Mode %d starts fighting through the real policy: %s"),Mode,*P->Status),H->CombatTarget,970);
                // Use the same background request budget to prepare the next mob.
                H->AppraisalRequests.Empty();H->AppraisalRequests.Add(970,FPlatformTime::Seconds());
                H->NextAppraisalScan=0;H->LastAppraisalRequest=FPlatformTime::Seconds()-2;
                H->RefreshAutomationData();
                TestTrue(TEXT("Next opponent is assessed while current fight continues"),H->AppraisalRequests.Contains(971)&&H->CombatTarget==970);
                const double RequestedAt=H->AppraisalRequests.FindRef(971);H->RefreshAutomationData();
                TestEqual(TEXT("Lookahead preserves the existing appraisal request budget"),H->AppraisalRequests.FindRef(971),RequestedAt);
                Info=FACEAppraisalInfo();Info.ObjectGuid=971;Info.bSuccess=AppraisalResult==1;
                if(AppraisalResult>=0)H->ObserveAppraisal(Info);
                const int32 Before=Mode==8?CastCount():AttackCount();
                if(Approach)
                {
                    FACEObjectMotionState Charge;Charge.MovementType=6;Charge.MoveToTargetGuid=970;Charge.MoveToDistance=.6f;
                    BuffPC->BeginServerMoveTo(Charge);
                    TestTrue(TEXT("Server combat approach is active before target dies"),BuffPC->IsUseApproachActive());
                }
                if(AppraisalResult<0)
                {
                    FACEBinaryWriter Health;Health.WriteUInt32(970);Health.WriteFloat(0);FACEBinaryReader HR(Health.GetData());Session.HandleUpdateHealth(HR);
                }
                else if(AppraisalResult==0)
                {
                    Session.WorldObjects[970].bDying=true;FACEObjectMotionState Dead;Dead.ForwardCommand=ACEMotion::Dead;
                    Session.OnMotionUpdate.Broadcast(970,Dead);
                }
                else {Session.WorldObjects.Remove(970);Session.OnObjectDeleted.Broadcast(970);}
                TestFalse(TEXT("Target death releases its server combat approach without waiting for object deletion"),BuffPC->IsUseApproachActive());
                if(Mode==8)
                {
                    DeliverWake();TestEqual(TEXT("Death does not send a second spell before UseDone"),CastCount(),Before);
                    Ack(0);
                }
                DeliverWake();
                if(AppraisalResult<0)
                {
                    TestTrue(TEXT("Unanswered monster appraisal does not hold the action gate"),P->WaitAction.IsEmpty()&&P->NextAction<=FPlatformTime::Seconds());
                    TestTrue(TEXT("Assessment schedules a short continuation instead of the idle poll"),H->DecisionWakeTicker.IsValid()&&H->DecisionWakeAt<=FPlatformTime::Seconds()+.06);
                    P->NextDecision=0;DeliverWake();
                }
                TestEqual(*FString::Printf(TEXT("Mode %d sends next attack on death/completion wake: %s"),Mode,*P->Status),Mode==8?CastCount():AttackCount(),Before+1);
                TestEqual(TEXT("Real policy selects the next living enemy"),H->CombatTarget,971);
                if(Mode==8)Ack(0);
            }
            // Never let a late death of the old target cancel another world
            // interaction, a pickup or an actual untagged UseDone wait.
            auto Charge=[&](int32 Target){BuffPC->EndUseApproach();FACEObjectMotionState M;M.MovementType=6;M.MoveToTargetGuid=Target;M.MoveToDistance=.6f;BuffPC->BeginServerMoveTo(M);};
            Charge(971);BuffPC->EndCombatApproachTo(970);
            TestTrue(TEXT("Old target death leaves a different approach intact"),BuffPC->IsUseApproachActive());
            Charge(970);Session.bUseBusy=true;BuffPC->EndCombatApproachTo(970);
            TestTrue(TEXT("Target death cannot acknowledge a pending world use"),BuffPC->IsUseApproachActive());Session.bUseBusy=false;
            Charge(970);BuffPC->bApproachPickupIntoInventory=true;BuffPC->EndCombatApproachTo(970);
            TestTrue(TEXT("Target death leaves inventory pickup synchronization intact"),BuffPC->IsUseApproachActive());
            Charge(970);BuffPC->bAwaitingUseDone=true;BuffPC->EndCombatApproachTo(970);
            TestTrue(TEXT("Target death leaves controller UseDone wait intact"),BuffPC->IsUseApproachActive());BuffPC->EndUseApproach();
            for(int32 Mode:{2,4,8})
            {
                Session.WorldObjects.Add(970,Enemy);H->DefeatedTargets.Empty();H->CombatTarget=0;
                H->PhysicalAttackOwner.Empty();H->PhysicalAttackTarget=0;H->PendingSpell=0;P->NextAction=0;P->WaitAction.Empty();
                Session.bServerAttackInProgress=false;Session.PendingCombatMode=0;Session.ApplyServerCombatMode(1);
                auto Request=AttackIntent(970,Mode);
                if(Mode==8){Request->SetStringField(TEXT("action"),TEXT("cast"));Request->SetNumberField(TEXT("spell"),Bolt);}
                const int32 Before=Mode==8?CastCount():AttackCount();H->Execute(*P,Request);
                TestTrue(TEXT("New target can be queued while entering combat stance"),P->CombatTransitionIntent.IsValid());
                Session.WorldObjects[970].bDying=true;FACEObjectMotionState Dead;Dead.ForwardCommand=ACEMotion::Dead;
                Session.OnMotionUpdate.Broadcast(970,Dead);
                TestFalse(TEXT("Death drops an unsent target action even before CombatTarget is installed"),P->CombatTransitionIntent.IsValid());
                TestTrue(TEXT("Unsent dead-target action yields immediately to recovery/target policy"),P->NextAction==0&&P->NextDecision==0);
                Session.ApplyServerCombatMode(Mode);H->UpdateCombatTransition(*P);
                TestEqual(TEXT("Late stance acknowledgment cannot dispatch an obsolete attack"),Mode==8?CastCount():AttackCount(),Before);
            }
            P->Profile->SetBoolField(TEXT("recovery"),true);
            P->CombatTransitionIntent=AttackIntent(971,2);P->CombatTransitionIntent->SetStringField(TEXT("activity"),TEXT("combat"));
            P->NextAction=FPlatformTime::Seconds()+5;P->NextDecision=P->NextAction;
            auto Hurt=Session.PlayerVitals;Hurt.Health=20;Hurt.MaxHealth=100;H->LastCombatVitals=FIntVector(100,100,100);
            H->ObserveCombatVitals(Hurt);
            TestFalse(TEXT("Damage during stance preparation drops the unsent combat decision"),P->CombatTransitionIntent.IsValid());
            TestTrue(TEXT("Recovery can reevaluate immediately after damage"),P->NextAction==0&&P->NextDecision==0);
            H->CombatTarget=971;H->PendingSpell=0;P->WaitAction=TEXT("identify");P->WaitItem=971;P->NextAction=FPlatformTime::Seconds()-1;
            H->RefreshActionWait(*P);P->NextDecision=FPlatformTime::Seconds()+.25;
            FTSTicker::GetCoreTicker().RemoveTicker(H->DecisionWakeTicker);H->DecisionWakeTicker.Reset();
            FACEAppraisalInfo Late;Late.ObjectGuid=971;Late.bSuccess=true;H->ObserveAppraisal(Late);
            TestTrue(TEXT("Appraisal after ID timeout wakes current target without a polling delay"),P->NextDecision==0&&H->DecisionWakeTicker.IsValid()&&H->DecisionWakeAt<=FPlatformTime::Seconds());
            P->WaitAction=TEXT("equip");P->NextAction=FPlatformTime::Seconds()+2;const double Gate=P->NextAction;
            H->ObserveAppraisal(Late);TestEqual(TEXT("Late ID cannot release a newer equipment action"),P->NextAction,Gate);
            P->WaitAction.Empty();P->NextAction=0;
        }
        // Delayed ACE stance and attack acknowledgments must precede actions,
        // even when the optimistic UI is already displaying the requested mode.
        {
            auto ModeReply=[&](int32 Mode)
            {
                FACEBinaryWriter W;W.WriteUInt32(ACEOpcode::PrivateUpdatePropertyInt);W.WriteUInt8(1);
                W.WriteUInt32(40);W.WriteInt32(Mode);Session.HandleGameMessage(W.GetData());
            };
            auto AttackReply=[&](uint32 Error)
            {
                FACEBinaryWriter W;W.WriteUInt32(Session.PlayerGuid);W.WriteUInt32(1);
                W.WriteUInt32(ACEGameEvent::AttackDone);W.WriteUInt32(Error);
                FACEBinaryReader R(W.GetData());Session.HandleGameEvent(R);
            };
            auto Count=[&](uint32 Opcode)
            {
                int32 N=0;for(const auto& Packet:Session.CachedC2SPackets)
                {FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);if(R.CanRead(4)&&R.ReadUInt32()==Opcode)++N;}return N;
            };
            P->Running=true;P->NextAction=0;P->WaitAction.Empty();P->ActivityFailure.Reset();
            P->Permissions.Add(TEXT("inventory"));Session.KnownSpells={Focus,Self};
            H->PhysicalAttackOwner.Empty();H->PhysicalAttackTarget=0;H->PendingSpell=0;
            H->ConfirmedBuffAckAt=0;H->DeferredBuffIntent.Reset();Session.bUseBusy=false;
            Session.bServerAttackInProgress=false;Session.PendingCombatMode=0;ModeReply(1);
            const int32 BeforeCast=CastCount(),BeforeMode=Count(ACEGameAction::ChangeCombatMode);
            H->Execute(*P,Intent(Focus));
            TestEqual(TEXT("Stance request is sent before a peace-mode buff"),Count(ACEGameAction::ChangeCombatMode),BeforeMode+1);
            TestEqual(TEXT("Optimistic magic stance does not dispatch the spell"),CastCount(),BeforeCast);
            TestTrue(TEXT("Prepared buff remains queued without claiming a cast acknowledgment"),P->CombatTransitionIntent.IsValid()&&!H->PendingSpell);
            ModeReply(1);H->UpdateCombatTransition(*P);
            TestEqual(TEXT("Transient peace confirmation cannot release a queued buff"),CastCount(),BeforeCast);
            TestEqual(TEXT("Waiting does not spam combat mode requests"),Count(ACEGameAction::ChangeCombatMode),BeforeMode+1);
            ModeReply(8);H->UpdateCombatTransition(*P);
            TestEqual(TEXT("Magic acknowledgment releases exactly one queued buff"),CastCount(),BeforeCast+1);
            Ack(0);
            // A fast-buff retry must also refuse a subsequent server peace mode.
            P->NextAction=0;H->Execute(*P,Intent(Self));H->BuffRetryAt=FPlatformTime::Seconds()-1;
            ModeReply(1);const int32 BeforeRetry=CastCount();H->UpdateBuffDispatch();
            TestEqual(TEXT("Fast retry cannot cast while server is in peace"),CastCount(),BeforeRetry);
            TestEqual(TEXT("Policy can retry the interrupted buff through normal stance preparation"),H->PendingSpell,0);
            H->FastCastOwner.Empty();Session.bUseBusy=false;
            // Keep main/offhand gear intact through every hit of a dual-wield
            // swing and through AttackDone(0), which only starts another repeat.
            FACEWorldObject Weapon;Weapon.Guid=973;Weapon.Name=TEXT("Test offhand");Weapon.ItemType=ACEItemType::MeleeWeapon;
            Weapon.WielderId=Session.PlayerGuid;Weapon.CurrentWieldedLocation=ACEEquipMask::Shield;
            Session.WorldObjects.Add(973,Weapon);ON_SCOPE_EXIT{Session.WorldObjects.Remove(973);};
            const int32 PreviousCapacity=Session.WorldObjects[Session.PlayerGuid].ItemsCapacity;
            Session.WorldObjects[Session.PlayerGuid].ItemsCapacity=1;
            FACEWorldObject Filler;Filler.Guid=976;Filler.ContainerId=Session.PlayerGuid;Session.WorldObjects.Add(Filler.Guid,Filler);
            FACEWorldObject Pack;Pack.Guid=977;Pack.ContainerId=Session.PlayerGuid;Pack.ItemType=ACEItemType::Container;Pack.ItemsCapacity=24;
            Session.WorldObjects.Add(Pack.Guid,Pack);
            ON_SCOPE_EXIT{Session.WorldObjects.Remove(976);Session.WorldObjects.Remove(977);Session.ContainerContents.Remove(977);Session.WorldObjects[Session.PlayerGuid].ItemsCapacity=PreviousCapacity;};
            auto Remove=MakeShared<FJsonObject>();Remove->SetStringField(TEXT("action"),TEXT("equip"));
            Remove->SetStringField(TEXT("activity"),TEXT("recovery"));Remove->SetNumberField(TEXT("item"),973);Remove->SetBoolField(TEXT("unequip"),true);
            ModeReply(2);Session.bServerAttackInProgress=true;Session.LastAttackError=0;
            H->PhysicalAttackOwner=P->Id;H->PhysicalAttackTarget=970;P->NextAction=0;
            const int32 BeforeRemove=Count(ACEGameAction::PutItemInContainer),BeforeCancel=Count(ACEGameAction::CancelAttack);
            H->Execute(*P,Remove);
            TestEqual(TEXT("Weapon change cancels the attack first"),Count(ACEGameAction::CancelAttack),BeforeCancel+1);
            TestEqual(TEXT("Weapon stays equipped during the current swing"),Count(ACEGameAction::PutItemInContainer),BeforeRemove);
            AttackReply(0);H->UpdateCombatTransition(*P);
            TestEqual(TEXT("Repeat AttackDone is not a cancellation acknowledgment"),Count(ACEGameAction::PutItemInContainer),BeforeRemove);
            AttackReply(0x20);H->UpdateCombatTransition(*P);
            TestEqual(TEXT("Completed swing still waits for peace acknowledgment"),Count(ACEGameAction::PutItemInContainer),BeforeRemove);
            ModeReply(1);H->UpdateCombatTransition(*P);
            TestEqual(TEXT("Owned offhand is removed only after both confirmations"),Count(ACEGameAction::PutItemInContainer),BeforeRemove+1);
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                if(R.CanRead(12)&&R.ReadUInt32()==ACEGameAction::PutItemInContainer&&R.ReadUInt32()==973)
                    TestEqual(TEXT("UCM puts the weapon in a side pack when the main pack is full"),R.ReadUInt32(),977u);
            }
            ConfirmPut(973,977);
            Session.bUseBusy=false;P->WaitAction.Empty();P->NextAction=0;
            H->Execute(*P,Intent(Focus));P->CombatTransitionStarted=FPlatformTime::Seconds()-16;
            H->UpdateCombatTransition(*P);
            TestTrue(TEXT("Stance timeout pauses only its activity and leaves UCM running"),P->Running&&P->ActivityFailure.IsValid()&&!P->CombatTransitionIntent);
            P->ActivityFailure.Reset();Session.PendingCombatMode=0;ModeReply(1);P->NextAction=0;
            H->Execute(*P,Intent(Focus));const int32 BeforeStop=CastCount();H->Stop(P->Id);
            ModeReply(8);H->UpdateCombatTransition(*P);
            TestTrue(TEXT("Stop discards queued combat action"),!P->CombatTransitionIntent&&CastCount()==BeforeStop);
            {
                // GDLE accepts a player-requested mode silently. Exercise the
                // full buff dispatch without an ACE-only CombatMode response.
                Session.Creds.bGDLE=true;ON_SCOPE_EXIT{Session.Creds.bGDLE=false;};
                P->Running=true;P->NextAction=0;P->WaitAction.Empty();P->ActivityFailure.Reset();
                H->ConfirmedBuffAckAt=0;H->DeferredBuffIntent.Reset();H->BuffRetryAt=0;
                H->PendingSpell=0;H->PhysicalAttackOwner.Empty();Session.bUseBusy=false;
                Session.bServerAttackInProgress=false;Session.PendingCombatMode=0;ModeReply(1);
                const uint32 FirstAction=Session.NextGameActionSequence;
                const int32 BeforeGDLECast=CastCount();
                H->Execute(*P,Intent(Focus));
                TestEqual(TEXT("GDLE can buff without an unsolicited combat-mode property"),CastCount(),BeforeGDLECast+1);
                TestFalse(TEXT("GDLE buff does not retain a doomed stance wait"),P->CombatTransitionIntent.IsValid());
                TArray<TPair<uint32,uint32>> Sent;
                for(const auto& Packet:Session.CachedC2SPackets)
                {
                    FACEBinaryReader R(Packet.Value.Payload);R.Skip(20);
                    if(!R.CanRead(8))continue;
                    const uint32 Sequence=R.ReadUInt32(),Opcode=R.ReadUInt32();
                    if(Sequence>=FirstAction)Sent.Emplace(Sequence,Opcode);
                }
                Sent.Sort([](const auto& A,const auto& B){return A.Key<B.Key;});
                if(TestTrue(TEXT("GDLE buff sends its mode and spell actions"),Sent.Num()>=2))
                {
                    TestEqual(TEXT("GDLE changes mode before casting"),Sent[0].Value,ACEGameAction::ChangeCombatMode);
                    TestEqual(TEXT("GDLE cast follows the mode in server action order"),Sent[1].Value,ACEGameAction::CastTargetedSpell);
                }
                Ack(0);P->NextAction=0;
                const int32 BeforeSecond=CastCount(),BeforeSecondMode=Count(ACEGameAction::ChangeCombatMode);
                H->Execute(*P,Intent(Self));
                TestEqual(TEXT("Next GDLE buff uses the already-active magic stance"),CastCount(),BeforeSecond+1);
                TestEqual(TEXT("GDLE does not repeat stance requests between buffs"),Count(ACEGameAction::ChangeCombatMode),BeforeSecondMode);
                Ack(0);P->NextAction=0;
                ModeReply(1); // Explicit GDLE correction remains authoritative.
                const int32 BeforeCorrectedMode=Count(ACEGameAction::ChangeCombatMode);
                H->Execute(*P,Intent(Focus));
                TestEqual(TEXT("A server peace correction requires magic again"),Count(ACEGameAction::ChangeCombatMode),BeforeCorrectedMode+1);
                Ack(0);P->NextAction=0;P->Running=false;
            }
            // ManaChargesWhenOff has its own restricted VM and session identity.
            // It must retain the same transition safety without requiring Start.
            FACEWorldObject Stone;Stone.Guid=974;Stone.ContainerId=Session.PlayerGuid;
            Stone.ItemType=ACEItemType::ManaStone;Stone.ItemUseable=8;Session.WorldObjects.Add(974,Stone);
            ON_SCOPE_EXIT{Session.WorldObjects.Remove(974);H->IdleManaSession.Reset();H->IdleManaPlayer=0;H->IdleManaOwner.Empty();};
            P->Enabled=true;P->Profile->SetBoolField(TEXT("mana_charges_when_off"),true);
            H->IdleManaSession=C->GetSession();H->IdleManaPlayer=Session.PlayerGuid;H->IdleManaOwner=P->Id;
            auto Recharge=MakeShared<FJsonObject>();Recharge->SetStringField(TEXT("action"),TEXT("use_item"));Recharge->SetNumberField(TEXT("item"),974);
            P->NextAction=0;H->Execute(*P,Recharge);const int32 BeforeUse=Count(ACEGameAction::Use);
            TestTrue(TEXT("Idle recharge can wait for peace without starting UCM"),P->CombatTransitionIntent.IsValid()&&!P->Running);
            ModeReply(1);H->UpdateCombatTransition(*P);
            TestEqual(TEXT("Idle recharge uses its stone after confirmed stance"),Count(ACEGameAction::Use),BeforeUse+1);
            Ack(0);Session.bUseBusy=false;P->WaitAction.Empty();P->NextAction=0;
            Session.WorldObjects[974].ItemUseable=0x00100008;
            const int32 BeforeTargetedCharge=Count(ACEGameAction::UseWithTarget);
            H->Execute(*P,Recharge);
            TestEqual(TEXT("Targeted mana charges also work while UCM is stopped"),Count(ACEGameAction::UseWithTarget),BeforeTargetedCharge+1);
            bool PlayerTarget=false;
            for(const auto& Packet:Session.CachedC2SPackets)
            {
                FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);
                if(R.CanRead(12)&&R.ReadUInt32()==ACEGameAction::UseWithTarget&&R.ReadUInt32()==974)
                    PlayerTarget=R.ReadUInt32()==uint32(Session.PlayerGuid);
            }
            TestTrue(TEXT("Suit recharge targets the player, not one armor item"),PlayerTarget);
            Ack(0);Session.bUseBusy=false;P->WaitAction.Empty();P->NextAction=0;
            Session.WorldObjects[974].ItemUseable=8;
            ModeReply(8);Recharge->SetStringField(TEXT("activity"),TEXT("item_mana"));
            H->Execute(*P,Recharge);P->CombatTransitionStarted=FPlatformTime::Seconds()-16;
            H->UpdateCombatTransition(*P);
            TestFalse(TEXT("Temporary stopped-service stance failure does not permanently disable mana recharge"),H->IdleManaFailed);
            TestTrue(TEXT("Stopped recharge retains a bounded activity failure"),P->ActivityFailure.IsValid());
            // The actual idle dispatcher must deliver the host failure to its
            // restricted stock policy, even for an API 2 plugin that is stopped.
            const int32 OldApi=P->Api;P->Api=2;const auto OldVitals=Session.PlayerVitals;
            Session.PlayerVitals.Health=Session.PlayerVitals.MaxHealth=100;
            H->IdleManaVM.Reset();H->IdleManaProfileHash=0;H->NextIdleManaDecision=0;
            H->TickIdleEquipmentMana();
            TestFalse(TEXT("Stopped dispatcher consumes the failure notification"),P->ActivityFailure.IsValid());
            TestTrue(TEXT("Stopped dispatcher shows the mana retry status"),P->Status.Contains(TEXT("Equipment mana paused")));
            TestTrue(TEXT("Recharge failure does not start combat, buffing or navigation"),!P->Running&&!P->CombatTransitionIntent);
            P->Api=OldApi;Session.PlayerVitals=OldVitals;H->IdleManaVM.Reset();H->IdleManaFailed=false;
            H->PendingManaRefresh=false;H->PendingResourceRefresh.Empty();Session.PendingCombatMode=0;ModeReply(1);
            P->Profile->RemoveField(TEXT("mana_charges_when_off"));Session.bUseBusy=false;P->WaitAction.Empty();
        }
    }
    for(float Speed:{600.f,2000.f})for(float Arrival:{17.5f,70.f})for(float Rate:{15.f,30.f,90.f,144.f})for(bool VR:{false,true})for(float CameraYaw:{0.f,90.f,210.f})
    {
        H->MoveArrivalRadius=Arrival;
        FVector Location=Origin;FACEPosition Pose=OutdoorPosition;Pose.RotationW=1;Pose.RotationXYZ=FVector::ZeroVector;
        for(const FVector Offset:{FVector(1000,0,0),FVector(1000,1000,0),FVector(0,1000,0),FVector(0,0,0)})
        {
            const FVector Goal=Origin+Offset;
            H->MoveTarget=OutdoorPosition;H->MoveTarget.Location=FACEPosition::AceVectorToUnreal(Offset,.01f);
            H->MovementOwner=TEXT("ucm");H->MoveExpires=FPlatformTime::Seconds()+100;H->LastProgress=FPlatformTime::Seconds();H->LastMovePosition=Location;H->bMoveArrived=false;
            for(int Frame=0;Frame<int(Rate*20)&&FVector::Dist2D(Location,Goal)>=Arrival;++Frame)
            {
                Pose.Location=FACEPosition::AceVectorToUnreal(Location-Origin,.01f);
                Session.PlayerPosition=Pose;Pawn->SetActorLocationAndRotation(Location,Pose.ToUnrealQuat());
                // VR uses body facing; a desktop camera direction must be ignored.
                const FVector Facing=VR?Pose.GetAceForwardVector():FRotator(0,CameraYaw,0).Vector();
                float F=0,R=0,T=0;H->ApplyMovement(PC,F,R,T,false,false,VR,Facing,Speed/Rate);
                const FQuat Turn(FVector::UpVector,-1.5f*1.5f*T/Rate);
                const FQuat Ac=(Turn*Pose.GetAcQuat()).GetNormalized();Pose.RotationW=Ac.W;Pose.RotationXYZ=FVector(Ac.X,Ac.Y,Ac.Z);
                Location+=(Pose.GetAceForwardVector()*F+Pose.GetAceRightVector()*R)*Speed/Rate;
            }
            TestTrue(*FString::Printf(TEXT("%s route reaches each corner within %.1f cm at %.0f FPS, camera %.0f"),VR?TEXT("VR"):TEXT("Desktop"),Arrival,Rate,CameraYaw),FVector::Dist2D(Location,Goal)<Arrival);
        }
    }
    {
        auto Move=MakeShared<FJsonObject>();Move->SetStringField(TEXT("action"),TEXT("move"));
        Move->SetNumberField(TEXT("cell"),uint32(Session.PlayerPosition.CellId));
        Move->SetNumberField(TEXT("x"),Session.PlayerPosition.Location.X);
        Move->SetNumberField(TEXT("y"),Session.PlayerPosition.Location.Y);
        Move->SetNumberField(TEXT("z"),Session.PlayerPosition.Location.Z);
        P->NextAction=0;Session.bUseBusy=false;H->Execute(*P,Move);
        TestTrue(TEXT("Steering leaves no action delay that can block arrival decisions"),P->NextAction<=FPlatformTime::Seconds());
        TestEqual(TEXT("Steering retains movement ownership"),H->MovementOwner,P->Id);
        const FACEPosition Before=H->MoveTarget;Move->SetNumberField(TEXT("x"),Before.Location.X+1);
        P->NextAction=FPlatformTime::Seconds()+2;H->Execute(*P,Move);
        TestEqual(TEXT("Steering still respects an outstanding action throttle"),H->MoveTarget.Location.X,Before.Location.X);
        P->NextAction=0;
    }
    H->MovementOwner=TEXT("ucm");H->MoveTarget=Session.PlayerPosition;H->MoveExpires=FPlatformTime::Seconds()+20;
    H->LastProgress=FPlatformTime::Seconds();H->bMoveArrived=false;P->NextDecision=FPlatformTime::Seconds()+.25;
    float ArriveF=1,ArriveR=1,ArriveT=1;
    H->ApplyMovement(PC,ArriveF,ArriveR,ArriveT,false,false,false,FVector::ForwardVector);
    TestTrue(TEXT("Arrival stops all axes and wakes policy without a breadcrumb pause"),ArriveF==0&&ArriveR==0&&ArriveT==0&&P->NextDecision==0);
    P->NextDecision=FPlatformTime::Seconds()+.25;const double NextArrivalDecision=P->NextDecision;
    H->ApplyMovement(PC,ArriveF,ArriveR,ArriveT,false,false,false,FVector::ForwardVector);
    TestEqual(TEXT("Idle arrival does not force repeated expensive policy snapshots"),P->NextDecision,NextArrivalDecision);
    Session.PlayerPosition.Location.X+=1;
    H->ApplyMovement(PC,ArriveF,ArriveR,ArriveT,false,false,false,FVector::ForwardVector);
    TestTrue(TEXT("Position correction after arrival cannot turn back toward the old waypoint"),ArriveF==0&&ArriveR==0&&ArriveT==0);
    Session.PlayerPosition.Location.X-=1;
    H->MovementOwner=TEXT("ucm");H->MoveExpires=FPlatformTime::Seconds()+20;
    H->LastMovePosition=Session.PlayerPosition.ToUnrealLocation();H->LastProgress=FPlatformTime::Seconds()-6;
    const uint32 BeforeBlocked=H->MovementBlockedSerial;float BlockF=1,BlockR=1,BlockT=1;
    H->ApplyMovement(PC,BlockF,BlockR,BlockT,false,false,false,FVector::ForwardVector);
    TestEqual(TEXT("Blocked movement asks policy to recover"),H->MovementBlockedSerial,BeforeBlocked+1);
    TestTrue(TEXT("Blocked movement clears drive input before recovery"),H->MovementOwner.IsEmpty()&&BlockF==0&&BlockR==0&&BlockT==0);
    {
        const auto OriginalProfile=P->Profile;
        P->Profile=MakeShared<FJsonObject>(*OriginalProfile);P->Running=true;
        P->Profile->SetBoolField(TEXT("navigation"),true);P->Profile->SetBoolField(TEXT("recovery"),true);P->Profile->SetStringField(TEXT("combat"),TEXT("auto"));
        const auto OriginalVM=P->VM;
        H->MovementOwner=TEXT("ucm");H->UseApproachOwner=TEXT("ucm");H->MoveExpires=FPlatformTime::Seconds()+20;
        auto Pause=MakeShared<FJsonObject>();Pause->SetStringField(TEXT("action"),TEXT("pause_navigation"));Pause->SetStringField(TEXT("status"),TEXT("Navigation paused: blocked door"));
        H->Execute(*P,Pause);
        TestTrue(TEXT("Route failure retains running UCM and its VM"),P->Running&&P->VM==OriginalVM);
        TestFalse(TEXT("Route failure disables shared navigation toggle"),P->Profile->GetBoolField(TEXT("navigation")));
        TestTrue(TEXT("Route failure preserves enabled recovery"),P->Profile->GetBoolField(TEXT("recovery")));
        TestEqual(TEXT("Route failure preserves combat"),P->Profile->GetStringField(TEXT("combat")),FString(TEXT("auto")));
        TestTrue(TEXT("Route failure clears drive and approach ownership"),H->MovementOwner.IsEmpty()&&H->UseApproachOwner.IsEmpty()&&H->MoveExpires==0);
        P->Profile=OriginalProfile;
    }
    for(const TCHAR* Action:{TEXT("salvage"),TEXT("sell"),TEXT("buy"),TEXT("split_note"),TEXT("combine_salvage"),TEXT("store_item")})
    {
        P->Running=true;P->ActivityFailure.Reset();const auto OriginalVM=P->VM;
        Session.CachedC2SPackets.Reset();
        auto Invalid=MakeShared<FJsonObject>();Invalid->SetStringField(TEXT("action"),Action);Invalid->SetNumberField(TEXT("item"),1234567);
        TestTrue(TEXT("Invalid inventory intent is handled"),H->ExecuteInventory(*P,Invalid,Action));
        TestTrue(*FString::Printf(TEXT("Rejected %s retains UCM and VM"),Action),P->Running&&P->VM==OriginalVM);
        TestTrue(TEXT("Host rejection is returned to activity policy"),P->ActivityFailure.IsValid());
        TestTrue(TEXT("Rejected inventory action sends neither destructive request nor attack cancellation"),Session.CachedC2SPackets.IsEmpty());
    }
    P->ActivityFailure.Reset();
    for(const TCHAR* Activity:{TEXT("buffs"),TEXT("combat"),TEXT("recovery2")})
    {
        P->Running=true;const auto OriginalVM=P->VM;
        H->PendingSpell=123;H->PendingSpellOwner=P->Id;H->PendingSpellActivity=Activity;H->PendingSpellAt=FPlatformTime::Seconds()-31;
        const uint32 BeforeSerial=H->ActionSerial;
        H->FastCastOwner=P->Id;H->FastCastStarted=true;
        H->CheckPendingSpellTimeout();
        TestTrue(TEXT("Cast timeout retains running UCM and its policy"),P->Running&&P->VM==OriginalVM);
        TestEqual(TEXT("Timed-out cast no longer blocks decisions"),H->PendingSpell,0);
        TestEqual(TEXT("Policy observes a completed failed attempt after timeout"),H->ActionSerial,BeforeSerial+1);
        TestFalse(TEXT("Cast timeout releases fast-cast locomotion"),H->FastCastStarted);
        TestEqual(TEXT("Cast timeout identifies the spell that ended"),H->LastCompletedSpell,123);
        TestTrue(TEXT("Cast timeout reports its specific activity"),P->ActivityFailure&&P->ActivityFailure->GetStringField(TEXT("activity"))==Activity);
        P->ActivityFailure.Reset();
    }
    {
        const auto OriginalProfile=P->Profile,OriginalLinks=P->ProfileLinks,OriginalBaseline=P->ProfileBaseline;
        P->Profile=MakeShared<FJsonObject>();P->ProfileLinks.Reset();P->ProfileBaseline.Reset();
        FACEWorldObject Friend;Friend.Guid=989;Friend.Name=TEXT("Follow friend");Friend.bIsPlayer=true;Friend.bHasPosition=true;
        Friend.ItemType=ACEItemType::Creature;Friend.Position=Session.PlayerPosition;Session.WorldObjects.Add(Friend.Guid,Friend);
        C->SelectObject(Friend.Guid);
        const bool FollowSaved=H->SetUCMFollowSelected(true);
        TestTrue(*(FString(TEXT("Follow selects another player: "))+H->Notice),FollowSaved);
        TestTrue(TEXT("Follow enables navigation"),H->IsUCMFollowEnabled());
        P->Profile->SetBoolField(TEXT("follow_corners"),true);P->Running=true;
        H->PendingSpell=123;H->PendingSpellOwner=P->Id;H->PendingSpellAt=FPlatformTime::Seconds();
        for(int32 SampleIndex=0;SampleIndex<3;++SampleIndex)
        {
            auto Position=Friend.Position;Position.Location.X+=SampleIndex<2?SampleIndex*5:5;Position.Location.Y+=SampleIndex==2?5:0;
            H->ObserveFollowPosition(Friend.Guid,Position);
        }
        TestEqual(TEXT("Follow records network footsteps during a pending cast"),H->FollowMovementSamples.Num(),3);
        TestEqual(TEXT("Pending footsteps reach the policy snapshot"),H->Snapshot()->GetArrayField(TEXT("follow_motion")).Num(),3);
        TestTrue(TEXT("Current status names the pending cast"),H->UCMCurrentStatus().Contains(TEXT("awaiting result")));
        H->PendingSpell=0;H->PendingSpellOwner.Empty();
        H->MovementOwner=P->Id;H->MoveExpires=FPlatformTime::Seconds()+10;
        H->SetUCMFollowSelected(false);
        TestFalse(TEXT("Stopping follow does not start an old route"),P->Profile->GetBoolField(TEXT("navigation")));
        TestTrue(TEXT("Stopping follow releases its movement and history"),H->MovementOwner.IsEmpty()&&H->FollowMovementSamples.IsEmpty());
        Friend.bIsPlayer=false;Session.WorldObjects[Friend.Guid]=Friend;
        TestFalse(TEXT("Follow rejects selected non-players"),H->SetUCMFollowSelected(true));
        P->Running=false;P->Status=TEXT("Ready");
        auto Micro=H->MakeUCMMicroPanel();Micro->SlatePrepass();const FVector2D Before=Micro->GetDesiredSize();
        P->Status=FString::ChrN(500,TEXT('W'));Micro->Invalidate(EInvalidateWidgetReason::Layout);Micro->SlatePrepass();
        TestTrue(TEXT("Long live status cannot resize Micro or its drag bar"),Before.Equals(Micro->GetDesiredSize(),.01));
        P->Status=TEXT("Following: Follow friend");
        if(FApp::CanEverRender())
        {
            FWidgetRenderer Renderer(false,true);auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(260,650),TF_Bilinear,true);
            Renderer.DrawWidget(Target,Micro,FVector2D(260,650),.016f);FlushRenderingCommands();
            TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(260,650,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("UCMMicroControls.png")));Target->ReleaseResource();
        }
        P->Profile=OriginalProfile;P->ProfileLinks=OriginalLinks;P->ProfileBaseline=OriginalBaseline;Session.WorldObjects.Remove(Friend.Guid);
    }
    H->MovementOwner.Empty();Session.PlayerPosition=OutdoorPosition;PC->UnPossess();Pawn->Destroy();PC->Destroy();
    if(FParse::Param(FCommandLine::Get(),TEXT("WaypointRender")))
    {
        auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
        if(TestTrue(TEXT("Waypoint uses retail DAT geometry"),Dat->LoadDatDirectory(ACETestDatDirectory())))
        {
            auto Waypoint=H->Find(TEXT("waypoint"));Waypoint->Enabled=true;Waypoint->Profile=MakeShared<FJsonObject>();
            Waypoint->Profile->SetNumberField(TEXT("east"),-99.);Waypoint->Profile->SetNumberField(TEXT("north"),-48.5);
            for(int Scene=0;Scene<5;++Scene)
            {
                const bool Dungeon=Scene==2;Waypoint->Profile->SetBoolField(TEXT("dungeon"),Dungeon);
                Waypoint->Profile->SetBoolField(TEXT("heading_up"),Scene==3);
                Session.PlayerPosition.CellId= Dungeon?0x01430171:0x7D640014;
                Session.PlayerPosition.Location= Dungeon?FVector(49,-75,0):FVector(60,78,12);
                Session.PlayerPosition.RotationW=FMath::Sqrt(.5);Session.PlayerPosition.RotationXYZ=FVector(0,0,-FMath::Sqrt(.5));
                const FVector2D Size=Scene==0?FVector2D(290,190):FVector2D(780,760);
                auto Widget=H->MakeWaypointPanel(Scene!=0,Scene==0);FWidgetRenderer Renderer(false,true);
                auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
                const double Begin=FPlatformTime::Seconds();
                for(int Pass=0;Pass<(Scene==3?4800:Scene==4?2400:Dungeon?600:5);++Pass)
                {
                    Renderer.DrawWidget(Target,Widget,Size,.016f);FlushRenderingCommands();
                    if(Scene>=3&&Pass==2)
                    {
                        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>)> FindMap=[&](TSharedRef<SWidget> W)->TSharedPtr<SWidget>
                        {if(W->GetTypeAsString()==TEXT("SWaypointMap"))return W;auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=FindMap(Children->GetChildAt(I)))return Found;return nullptr;};
                        if(auto Map=FindMap(Widget))
                        {
                            const auto& G=Map->GetCachedGeometry();const FVector2D At=G.LocalToAbsolute(G.GetLocalSize()*.5f);
                            const float Wheel=FMath::Loge(Scene==3?16.f:48.f)/FMath::Loge(1.25f);
                            TestTrue(TEXT("Map accepts zoom while rotated"),Map->OnMouseWheel(G,FPointerEvent(0,At,At,{},EKeys::Invalid,Wheel,FModifierKeysState())).IsEventHandled());
                        }
                        else AddError(TEXT("Map widget not found for zoom fixture"));
                    }
                    if(Scene>=3)FPlatformProcess::Sleep(.005f);
                }
                AddInfo(FString::Printf(TEXT("Waypoint %s render/load fixture: %.2f ms"),Dungeon?TEXT("dungeon"):TEXT("world/arrow"),(FPlatformTime::Seconds()-Begin)*1000));
                TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
                TestEqual(TEXT("DAT navigation view renders"),Pixels.Num(),int(Size.X*Size.Y));
                TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(int(Size.X),int(Size.Y),Pixels,Png);
                FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("Waypoint-Live-%d.png"),Scene)));Target->ReleaseResource();
            }
        }
    }
    return true;
}
#endif
