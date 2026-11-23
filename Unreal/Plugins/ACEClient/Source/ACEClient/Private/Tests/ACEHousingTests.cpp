#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "RenderingThread.h"
#include "ImageUtils.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUIGameplayBinder.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanelSlot.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "UI/ACERetailTextBlock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEHousingTest,"ACE.RetailParity.Housing",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEHousingTest::RunTest(const FString&)
{
	TGuardValue<uint64> FrameGuard(GFrameCounter,GFrameCounter);
	auto* GI=NewObject<UGameInstance>();
	auto* Client=NewObject<UACEClientSubsystem>(GI); Client->Session=MakeShared<FACESession>();
	auto& S=*Client->Session; S.PlayerGuid=1234; S.State=EACESessionState::InWorld;
	TArray<FString> AvailableMessages;
	const auto AvailableHandle=S.OnChatMessage.AddLambda([&](const FString& Text,const FString&,int32){AvailableMessages.Add(Text);});
	auto Available=[&](uint32 Type,uint32 Count,int32 Total)
	{
		FACEBinaryWriter W; W.WriteUInt32(1234); W.WriteUInt32(1); W.WriteUInt32(0x0271);
		W.WriteUInt32(Type); W.WriteUInt32(Count);
		for(uint32 I=0;I<Count;++I)W.WriteUInt32(0x7F7F0001);
		W.WriteInt32(Total);return W.GetData();
	};
	for(uint32 Type=1;Type<=4;++Type)
	{
		AvailableMessages.Reset();auto B=Available(Type,1,1);FACEBinaryReader R(B);S.HandleGameEvent(R);
		TestEqual(TEXT("Available houses response consumed"),R.Remaining(),0);
		TestEqual(TEXT("Retail apartments print only the count; other types include coordinates"),AvailableMessages.Num(),Type==4?1:2);
		if(Type!=4 && AvailableMessages.Num()>1)TestEqual(TEXT("Retail outdoor-cell coordinate conversion"),AvailableMessages[1],FString(TEXT("     0.3S, 0.3W")));
	}
	const auto FullAvailable=Available(2,401,501);
	for(int32 Cut=0;Cut<FullAvailable.Num();++Cut)
	{
		AvailableMessages.Reset();auto B=FullAvailable;B.SetNum(Cut);FACEBinaryReader R(B);S.HandleGameEvent(R);
		TestTrue(TEXT("Truncated house lists never emit partial chat"),AvailableMessages.IsEmpty());
	}
	AvailableMessages.Reset();FACEBinaryReader AvailableReader(FullAvailable);S.HandleGameEvent(AvailableReader);
	TestEqual(TEXT("Retail output caps location spam at 400 plus count and notice"),AvailableMessages.Num(),402);
	S.OnChatMessage.Remove(AvailableHandle);
	FACEWorldObject Self; Self.Guid=1234; Self.Name=TEXT("Housing regression"); Self.ItemsCapacity=120;Self.ContainersCapacity=7;S.WorldObjects.Add(1234,Self);
	// Independent fixture from HouseProfile::Pack / CM_House::RecvNotice_HouseProfile.
	auto Profile=[&](int32 Owner,int32 Type=1)
	{
		FACEBinaryWriter W;
		for(uint32 V:{4321u,77u,uint32(Owner),0u,1u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0u,uint32(Type)}) W.WriteUInt32(V);
		W.WriteString16L(Owner?TEXT("House owner"):TEXT(""));
		W.WriteUInt32(2);
		W.WriteInt32(1000); W.WriteInt32(0); W.WriteUInt32(273); W.WriteString16L(TEXT("Pyreal")); W.WriteString16L(TEXT("Pyreals"));
		W.WriteInt32(2); W.WriteInt32(0); W.WriteUInt32(111); W.WriteString16L(TEXT("Writ")); W.WriteString16L(TEXT("Writs"));
		W.WriteUInt32(1);
		W.WriteInt32(1000); W.WriteInt32(100); W.WriteUInt32(273); W.WriteString16L(TEXT("Pyreal")); W.WriteString16L(TEXT("Pyreals"));
		return W.GetData();
	};
	auto Bytes=Profile(0);
	FACEBinaryReader First(Bytes); S.HandleHouseProfile(First);
	TestEqual(TEXT("Profile fully consumed"),First.Remaining(),0);
	TestEqual(TEXT("Signed no-maximum restriction retained"),S.HouseOffer.MaxLevel,-1);
	TestEqual(TEXT("Both server purchase requirements retained"),S.HouseOffer.Buy.Num(),2);
	const uint64 Revision=S.HouseOfferRevision;
	for(int32 Cut=0;Cut<Bytes.Num();++Cut)
	{
		TArray<uint8> Truncated=Bytes; Truncated.SetNum(Cut);
		FACEBinaryReader R(Truncated); S.HandleHouseProfile(R);
		TestEqual(TEXT("Truncation cannot publish a partial offer"),S.HouseOfferRevision,Revision);
	}
	auto Load=[&](int32 Owner,int32 Type=1){auto B=Profile(Owner,Type);FACEBinaryReader R(B);S.HandleHouseProfile(R);};
	auto Add=[&](int32 Guid,int32 Wcid,int32 Count,int32 Container=1234)
	{
		FACEWorldObject I; I.Guid=Guid; I.WeenieClassId=Wcid; I.StackSize=Count; I.ContainerId=Container;
		I.Name=Wcid==273?TEXT("Pyreal"):TEXT("Writ"); I.ItemType=ACEItemType::Misc; I.IconId=0x060011CB;
		S.WorldObjects.Add(Guid,I);
		FACEContainerItemRef Ref; Ref.ItemGuid=Guid; S.ContainerContents.FindOrAdd(Container).Add(Ref);
	};
	Add(200,273,1000); Add(201,111,2); Add(202,111,2,9000); Add(203,273,1000);
	S.WorldObjects[203].WielderId=1234;
	TestFalse(TEXT("Foreign payment rejected"),S.StageHousePayment(202,false));
	TestFalse(TEXT("Equipped payment rejected"),S.StageHousePayment(203,false));
	TestFalse(TEXT("Empty purchase disabled"),S.CanPayHouse(false));
	TestTrue(TEXT("Currency accepted"),S.StageHousePayment(200,false));
	TestFalse(TEXT("Partial purchase disabled"),S.CanPayHouse(false));
	TestTrue(TEXT("Second requirement accepted"),S.StageHousePayment(201,false));
	TestTrue(TEXT("Complete purchase enabled"),S.CanPayHouse(false));
	TestFalse(TEXT("Duplicate offer ignored"),S.StageHousePayment(201,false));
	S.TradeSelfItems.Add(201);
	TestFalse(TEXT("An item offered in trade cannot also purchase a house"),S.CanPayHouse(false));
	S.TradeSelfItems.Reset();
	Load(0); TestTrue(TEXT("Main pack expands accepted contents"),S.StageHousePayment(1234,false));
	TestEqual(TEXT("Only qualifying owned unequipped items staged"),S.HouseBuyItems.Num(),2);
	Load(1234); S.WorldObjects[200].StackSize=10;
	TestFalse(TEXT("Owned house cannot be bought again"),S.StageHousePayment(200,false));
	TestTrue(TEXT("Partial rent accepted"),S.StageHousePayment(200,true));
	TestTrue(TEXT("Partial maintenance can be submitted"),S.CanPayHouse(true));
	Load(0); Add(204,2621,1); S.WorldObjects[204].ItemType=ACEItemType::PromissoryNote; S.WorldObjects[204].Value=10000;
	TestTrue(TEXT("Trade note accepted as pyreals"),S.StageHousePayment(204,false));
	TestEqual(TEXT("Trade note uses face value"),S.GetHousePaymentAmount(273,false),int64(10000));
	S.StageHousePayment(201,false);

	auto* Dat=NewObject<UACEDatSubsystem>(GI);
	if(!TestTrue(TEXT("Retail DAT opens"),Dat->LoadDatDirectory(ACETestDatDirectory()))) return false;
	ON_SCOPE_EXIT {Dat->Deinitialize();};
	auto* Resources=NewObject<UACEUIResourceResolver>(); Resources->Initialize(Dat);
	auto* Manager=NewObject<UACEUIElementManager>(); Manager->Initialize();
	auto* Layout=NewObject<UACEUILayoutResolver>(); Layout->Initialize(Dat,Manager);
	if(!TestTrue(TEXT("Retail gameplay layout loads"),Layout->LoadLayout(0x21000005))) return false;
	auto* Canvas=NewObject<UACEUICanvasWidget>(); Canvas->Initialize(); Canvas->InitializeCanvas(Manager); Canvas->SetResourceResolver(Resources);
	const auto Slate=Canvas->TakeWidget();
	auto* Binder=NewObject<UACEUIGameplayBinder>(); Binder->Initialize(Client,Manager,Canvas,nullptr); Canvas->SetGameplayBinder(Binder);
	ON_SCOPE_EXIT {Binder->Shutdown();Canvas->SetGameplayBinder(nullptr);Manager->Shutdown();};
	FWidgetRenderer Renderer(true,true);
	auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(1600,900),TF_Bilinear,true);
	const FString Art=FPaths::ProjectSavedDir()/TEXT("Automation/Housing"); IFileManager::Get().MakeDirectory(*Art,true);
	auto Draw=[&](const TCHAR* Name,float Scale=1.f)
	{
		const TSharedRef<SWidget> Scaled=SNew(SDPIScaler).DPIScale(Scale)[Slate];
		for(int I=0;I<4;++I){++GFrameCounter;Canvas->NativeTick(Canvas->GetCachedGeometry(),0);Renderer.DrawWidget(Target,Scaled,FVector2D(1600,900),0);FlushRenderingCommands();}
		TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
		TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(1600,900,Pixels,PNG);FFileHelper::SaveArrayToFile(PNG,*(Art/(FString(Name)+TEXT(".png"))));
	};
	Manager->ApplyEdgeAnchoredLayout(1600,900); Binder->RefreshHouseOffer(); Draw(TEXT("Buy"));
	TestEqual(TEXT("Profile opens the house lord"),Binder->OpenHouseLord,4321);
	TestTrue(TEXT("Actual Slumlord DAT subtree shown"),bool(Manager->FindElementByName(TEXT("Slumlord"))->bVisible));
	TestFalse(TEXT("Vendor chrome stays hidden"),bool(Manager->FindElementByName(TEXT("Vendor"))->bVisible));
	TestTrue(TEXT("Buy tab visible"),bool(Manager->FindElementByName(TEXT("HouseBuyPage"))->bVisible));
	TestFalse(TEXT("Maintenance tab hidden"),bool(Manager->FindElementByName(TEXT("HouseMaintenancePage"))->bVisible));
	TestTrue(TEXT("Payment icons use actual item list"),Binder->HouseOfferSlots.Num()>0);
	TestTrue(TEXT("Complete requirements enable Buy"),Manager->FindElementByName(TEXT("HouseBuy_Button"))->bActivatable);
	Binder->OnElementActivated(Manager->FindElementByName(TEXT("HouseBuy_Button")));
	TestTrue(TEXT("Landscape purchase requires retail confirmation"),Binder->bHousePaymentConfirm);
	Binder->FinishServerConfirmation(false);
	TestFalse(TEXT("Declining clears confirmation"),Binder->bHousePaymentConfirm);
	TestEqual(TEXT("Declining retains staged payments"),S.HouseBuyItems.Num(),2);

	auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	auto Address=Sockets->CreateInternetAddr();bool Valid=false;Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
	auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("House receiver"),false);
	if(!Receiver)return false;
	ON_SCOPE_EXIT {Receiver->Close();Sockets->DestroySocket(Receiver);};
	if(!TestTrue(TEXT("Loopback binds"),Valid && Receiver->Bind(*Address)))return false;
	Receiver->GetAddress(*Address);
	S.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("House sender"),false);
	if(!S.SocketC2S)return false;
	ON_SCOPE_EXIT {S.SocketC2S->Close();Sockets->DestroySocket(S.SocketC2S);S.SocketC2S=nullptr;};
	S.ServerC2SAddr=Address; S.IssacClient=MakeUnique<FACEIsaac>(123);
	TestTrue(TEXT("Chat dispatch recognizes house guest list"),Binder->TryDispatchChatCommand(TEXT("@house guest list")));
	const auto* GuestRequest=S.CachedC2SPackets.Find(S.NextPacketSequence-1);
	if(TestNotNull(TEXT("Guest-list command sends a request"),GuestRequest))
	{
		FACEBinaryReader R(GuestRequest->Payload);R.Skip(14);
		TestEqual(TEXT("Guest-list request uses retail weenie queue"),R.ReadUInt16(),uint16(ACEQueue::WeenieQueue));
		R.Skip(8);TestEqual(TEXT("Guest-list command opcode"),R.ReadUInt32(),0x024du);
		TestEqual(TEXT("Guest-list request has no extra payload"),R.Remaining(),0);
	}
	Binder->OnElementActivated(Manager->FindElementByName(TEXT("HouseBuy_Button"))); Binder->FinishServerConfirmation(true);
	const auto* Packet=S.CachedC2SPackets.Find(S.NextPacketSequence-1);
	if(TestNotNull(TEXT("Confirmed purchase sends action"),Packet))
	{
		FACEBinaryReader R(Packet->Payload); R.Skip(14); TestEqual(TEXT("Retail weenie queue"),R.ReadUInt16(),uint16(ACEQueue::WeenieQueue));
		R.Skip(8); TestEqual(TEXT("BuyHouse opcode"),R.ReadUInt32(),0x21Cu);
		TestEqual(TEXT("Lord GUID"),R.ReadUInt32(),4321u); TestEqual(TEXT("Two payment IDs"),R.ReadUInt32(),2u);
		TestEqual(TEXT("Trade note ID"),R.ReadUInt32(),204u);TestEqual(TEXT("Writ ID"),R.ReadUInt32(),201u);
		TestEqual(TEXT("No invented payment fields"),R.Remaining(),0);
	}
	TestTrue(TEXT("Only local staged cart clears"),S.HouseBuyItems.IsEmpty());
	TestTrue(TEXT("Server remains authoritative over items"),S.WorldObjects.Contains(204));
	Load(1234);Binder->RefreshHouseOffer();S.StageHousePayment(200,true);Draw(TEXT("Maintenance"));
	TestTrue(TEXT("Owned house opens maintenance"),Binder->bHouseRentTab);
	TestTrue(TEXT("Partial maintenance shows paid and staged count"),Binder->HouseOfferLabels[3]->GetText().ToString().Contains(TEXT("110/1000 Pyreals")));
	Binder->OnElementActivated(Manager->FindElementByName(TEXT("HouseMaintenance_Button")));
	TestFalse(TEXT("Owner needs no proxy confirmation"),Binder->bHousePaymentConfirm);
	const auto* RentPacket=S.CachedC2SPackets.Find(S.NextPacketSequence-1);
	if(TestNotNull(TEXT("Rent sends packet"),RentPacket)){FACEBinaryReader R(RentPacket->Payload);R.Skip(24);TestEqual(TEXT("RentHouse opcode"),R.ReadUInt32(),0x221u);}
	// Splits must arrive from the server before their new object IDs can be paid.
	Binder->StageHouseItem(200,4);
	TestEqual(TEXT("Partial payment tracks a pending split"),Binder->HousePaymentSplit.SourceGuid,200);
	TestTrue(TEXT("Original full stack is never staged during split"),S.HouseRentItems.IsEmpty());
	Binder->RefreshHouseOffer();TestTrue(TEXT("Awaiting split acknowledgement keeps cart empty"),S.HouseRentItems.IsEmpty());
	constexpr int32 SplitGuid=int32(0x80000205u);
	S.WorldObjects[200].StackSize=6;Add(SplitGuid,273,4);
	Binder->RefreshHouseOffer();
	TestTrue(TEXT("Confirmed dynamic high-bit stack becomes payment"),S.HouseRentItems.Contains(SplitGuid));
	TestEqual(TEXT("Split tracking completes"),Binder->HousePaymentSplit.SourceGuid,0);
	Draw(TEXT("SplitMaintenance"));
	const auto List=Manager->FindElementByName(TEXT("HouseMaintenanceItemsList"));
	const FVector2D In=Canvas->LayoutToViewport(FVector2D(List->GetScreenOrigin())+FVector2D(16,16));
	TestTrue(TEXT("Staged item is clickable at its visible position"),Binder->TryBeginInventoryDrag(In));
	Binder->bInvDragActive=true;Binder->TryFinishInventoryDrag(FVector2D(0,0));
	TestTrue(TEXT("Dragging out removes only the payment offer"),S.HouseRentItems.IsEmpty());
	TestTrue(TEXT("Dragging out preserves physical item"),S.WorldObjects.Contains(SplitGuid));
	// Dropping a main-pack reference stages matching contents instead of moving the player.
	Binder->InvDragGuid=1234;Binder->bInvDragPending=true;Binder->bInvDragActive=true;
	Binder->TryFinishInventoryDrag(In);
	TestTrue(TEXT("Main pack drag stages payments"),S.HouseRentItems.Contains(200));
	Load(9999);Binder->RefreshHouseOffer();S.StageHousePayment(200,true);Binder->RefreshHouseOffer();
	Binder->OnElementActivated(Manager->FindElementByName(TEXT("HouseMaintenance_Button")));
	TestTrue(TEXT("Proxy maintenance requires confirmation"),Binder->bHousePaymentConfirm);Binder->FinishServerConfirmation(false);
	FACEBinaryWriter Failed;Failed.WriteUInt32(1234);Failed.WriteUInt32(1);Failed.WriteUInt32(0x259);Failed.WriteUInt32(2);
	FACEBinaryReader Failure(Failed.GetData());S.HandleGameEvent(Failure);
	const auto* Query=S.CachedC2SPackets.Find(S.NextPacketSequence-1);
	if(TestNotNull(TEXT("Failed transaction refreshes the offer"),Query))
	{FACEBinaryReader R(Query->Payload);R.Skip(24);TestEqual(TEXT("QueryHouse action"),R.ReadUInt32(),0x258u);TestEqual(TEXT("Queries active lord"),R.ReadUInt32(),4321u);}
	Load(0,4);Binder->RefreshHouseOffer();S.StageHousePayment(204,false);S.StageHousePayment(201,false);Binder->RefreshHouseOffer();
	Binder->OnElementActivated(Manager->FindElementByName(TEXT("HouseBuy_Button")));
	TestFalse(TEXT("Apartment purchase bypasses landscape cooldown prompt"),Binder->bHousePaymentConfirm);
	TestTrue(TEXT("Apartment payment was submitted"),S.HouseBuyItems.IsEmpty());
	Binder->ShowVendorPanel(9876);Binder->RefreshHouseOffer();
	TestEqual(TEXT("Opening another environment panel closes house context"),Binder->OpenHouseLord,0);
	Binder->CloseHouseOffer();
	TestFalse(TEXT("Closing collapses native housing panel"),bool(Manager->FindElementByName(TEXT("Slumlord"))->bVisible));
	TestEqual(TEXT("Closing clears server offer context"),S.GetHouseProfile().LordGuid,0);
	Binder->HideVendorPanel();Load(0);S.WorldObjects[200].StackSize=1000;Binder->RefreshHouseOffer();
	Binder->ShowPanelPage(TEXT("InventoryPanel_Field"));Binder->RefreshInventoryOverlays();Draw(TEXT("InventoryBeforeDrop"));
	const int32 ItemIndex=Binder->InventorySlotGuids.IndexOfByKey(200);
	if(TestTrue(TEXT("Payment item is displayed in inventory"),Binder->InventorySlots.IsValidIndex(ItemIndex)))
	{
		const auto Geometry=Canvas->GetCachedGeometry();
		const auto ItemGeometry=Binder->InventorySlots[ItemIndex]->GetCachedGeometry();
		const auto From=ItemGeometry.LocalToAbsolute(ItemGeometry.GetLocalSize()*.5f);
		const auto Destination=Manager->FindElementByName(TEXT("HouseBuyItemsList"));
		const auto To=Geometry.LocalToAbsolute(Canvas->LayoutToViewport(FVector2D(Destination->GetScreenOrigin())+FVector2D(16,16)));
		Canvas->NativeOnMouseButtonDown(Geometry,FPointerEvent(0,From,From,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
		TestEqual(TEXT("Real mouse press starts dragging currency"),Binder->InvDragGuid,200);
		Canvas->NativeOnMouseMove(Geometry,FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
		TestTrue(TEXT("Currency drag becomes active"),Binder->bInvDragActive);
		TestTrue(TEXT("Crystal displays accepted drop marker"),Binder->PaperDollDragTargetIcon && Binder->PaperDollDragTargetIcon->IsVisible());
		Canvas->NativeOnMouseButtonUp(Geometry,FPointerEvent(0,To,To,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
		TestTrue(TEXT("Mouse release into crystal stages currency"),S.HouseBuyItems.Contains(200));
	}
	// Exercise the visible hit targets after scaling, narrowing and moving the
	// shared crystal window. Do not bypass mouse-down or force bInvDragActive.
	Add(300,1,1);S.WorldObjects[300].ItemType=ACEItemType::Container;S.WorldObjects[300].ItemsCapacity=24;
	Add(301,273,200,300);Add(302,111,2,300);Add(303,999,1);
	Add(304,2621,3,300);S.WorldObjects[304].ItemType=ACEItemType::PromissoryNote;
	S.WorldObjects[304].Name=TEXT("Trade Note (250,000)");S.WorldObjects[304].Value=750000;
	// ACE can send owned bag contents through ObjectCreate/ContainerId without
	// a ViewContents list. Inventory displays these items and housing must too.
	S.ContainerContents.Remove(300);
	const auto Env=Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field"));
	Env->AuthoredWidth=610;Env->UserResizeW=-200;Env->UserDragX=-60;Env->RecomputeLayoutOffset();
	UACEUIElementManager::ApplyFloatyResizeLayout(Env);
	for(const bool Rent : {false,true})
	{
		Load(Rent?1234:0);S.HouseOffer.OwnerName=TEXT("An unusually long house owner's character name");Binder->RefreshHouseOffer();
		Binder->SelectedPackGuid=1234;Binder->RefreshInventoryOverlays();Draw(Rent?TEXT("NarrowMaintenance"):TEXT("NarrowBuy"),1.25f);
		const auto Close=Manager->FindElementByName(TEXT("CloseSlumlordButton"));
		TestTrue(TEXT("Resized crystal close button remains inside the window"),Close->GetScreenOrigin().X+Close->Width<=Env->GetScreenOrigin().X+Env->Width);
		TestTrue(TEXT("Resized crystal close button remains clickable"),Canvas->IsElementExposedAt(Close,Canvas->LayoutToViewport(FVector2D(Close->GetScreenOrigin())+FVector2D(10,10))));
		const auto* CloseArt=Canvas->ImageWidgets.Find(Close->InstanceId);
		TestTrue(TEXT("Resized crystal close button is painted at its anchored position"),CloseArt && (*CloseArt)->IsVisible());
		const auto Destination=Manager->FindElementByName(Rent?TEXT("HouseMaintenanceItemsList"):TEXT("HouseBuyItemsList"));
		const auto Geometry=Canvas->GetCachedGeometry();
		const auto To=Geometry.LocalToAbsolute(Canvas->LayoutToViewport(FVector2D(Destination->GetScreenOrigin())+FVector2D(16,16)));
		auto Drag=[&](int32 Guid,bool Accept,bool Pack=false)
		{
			Binder->SelectedPackGuid=1234;Binder->RefreshInventoryOverlays();Draw(TEXT("BeforeScaledDrag"),1.25f);
			const int32 Index=Pack?Binder->PackSlotGuids.IndexOfByKey(Guid):Binder->InventorySlotGuids.IndexOfByKey(Guid);
			const auto& Slots=Pack?Binder->PackSlots:Binder->InventorySlots;
			if(!TestTrue(TEXT("Drag fixture is visible"),Slots.IsValidIndex(Index)))return;
			const auto ItemGeometry=Slots[Index]->GetCachedGeometry();
			const auto From=ItemGeometry.LocalToAbsolute(ItemGeometry.GetLocalSize()*.5f);
			Canvas->NativeOnMouseButtonDown(Geometry,FPointerEvent(0,From,From,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
			TestEqual(TEXT("Scaled press selects intended item"),Binder->InvDragGuid,Guid);
			Canvas->NativeOnMouseMove(Geometry,FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
			if(TestNotNull(TEXT("Crystal drag feedback exists"),Binder->PaperDollDragTargetIcon.Get()))
			{
				TestTrue(TEXT("Crystal drag feedback is visible"),Binder->PaperDollDragTargetIcon->IsVisible());
				TestTrue(TEXT("Crystal uses retail accept/reject artwork"),Binder->PaperDollDragTargetIcon->Background.GetResourceObject()
					==Resources->ResolveIconTexture(Accept?0x060011F9:0x060011F8));
			}
			Canvas->NativeOnMouseButtonUp(Geometry,FPointerEvent(0,To,To,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
			TestEqual(TEXT("Scaled drop honors payment eligibility"),S.GetHousePaymentItems(Rent).Contains(Guid),Accept && !Pack);
		};
		Drag(303,false);Drag(200,true);
		// Server movement/deletion removes stale offers and prevents submitting them.
		S.WorldObjects[200].ContainerId=0;Binder->RefreshHouseOffer();
		TestFalse(TEXT("Moved payment is pruned from cart"),S.GetHousePaymentItems(Rent).Contains(200));S.WorldObjects[200].ContainerId=1234;
		TestTrue(TEXT("Backpack with eligible payments accepts a drop"),S.CanStageHousePayment(300,Rent));
		Drag(300,true,true);
		TestTrue(TEXT("Nested currency staged"),S.GetHousePaymentItems(Rent).Contains(301));
		TestEqual(TEXT("Backpack only stages requested item types"),S.GetHousePaymentItems(Rent).Contains(302),!Rent);
		TestTrue(TEXT("Whole bag parses MMD notes without ViewContents"),S.GetHousePaymentItems(Rent).Contains(304));
		TestEqual(TEXT("MMD stack contributes its whole pyreal value"),S.GetHousePaymentAmount(273,Rent),int64(750200));
		TestTrue(TEXT("Matching bag contents enable payment"),S.CanPayHouse(Rent));
		TestEqual(TEXT("Staging leaves the note in its physical bag"),S.WorldObjects[304].ContainerId,300);
		const auto Owner=Manager->FindElementByName(Rent?TEXT("HouseMaintenance_HouseOwner"):TEXT("HouseBuy_HouseOwner"));
		const auto Button=Manager->FindElementByName(Rent?TEXT("HouseMaintenance_Button"):TEXT("HouseBuy_Button"));
		TestTrue(TEXT("Long owner label stays clear of payment button"),Owner->GetScreenOrigin().X+Owner->Width<=Button->GetScreenOrigin().X);
		Draw(Rent?TEXT("NarrowMaintenancePaid"):TEXT("NarrowBuyPaid"),1.25f);
		Load(Rent?1234:0);Binder->RefreshHouseOffer();
		S.WorldObjects[304].ContainerId=1234;
		Drag(304,true);
		TestEqual(TEXT("Direct MMD drag stages full stack value"),S.GetHousePaymentAmount(273,Rent),int64(750000));
		S.WorldObjects[304].ContainerId=300;
		Load(Rent?1234:0);Binder->RefreshHouseOffer();
		(Rent?S.HouseOffer.Rent:S.HouseOffer.Buy)[0].Required=800000;
		TArray<FACEContainerItemRef> MainContents;
		S.ContainerContents.RemoveAndCopyValue(1234,MainContents);
		Drag(1234,true,true);
		TestTrue(TEXT("Main backpack expands loose currency without a contents list"),S.GetHousePaymentItems(Rent).Contains(200));
		TestTrue(TEXT("Main backpack also scans notes inside carried bags"),S.GetHousePaymentItems(Rent).Contains(304));
		TestFalse(TEXT("Main backpack never offers unrelated items"),S.GetHousePaymentItems(Rent).Contains(303));
		S.ContainerContents.Add(1234,MoveTemp(MainContents));
	}
	Env->UserResizeW=0;Env->UserDragX=0;Env->RecomputeLayoutOffset();UACEUIElementManager::ApplyFloatyResizeLayout(Env);
	S.House.bQueried=true;S.House.bOwned=true;S.House.HouseType=1;
	S.House.Buy=S.HouseOffer.Buy;S.House.Rent={{1000,100,273,TEXT("Pyreal"),TEXT("Pyreals")}};
	S.House.BuyTime=1700000000;S.House.RentTime=1750000000;
	S.House.Position.CellId=static_cast<int32>(0xA9A10100u);S.House.Position.Location=FVector(84,132,0);
	Binder->ShowPanelPage(TEXT("WorldPanel_Field"));Binder->SyncWorldPanelTab(TEXT("HousePage"));Draw(TEXT("HousingTab"));
	const auto HouseLocationPage=Manager->FindElementByName(TEXT("HousePage"));
	const auto MapPage=Manager->FindElementByName(TEXT("MapPage"));
	const auto HousePin=Manager->FindElementUnder(TEXT("MapPage"),TEXT("Map_HousePosition_Icon"));
	if(!TestTrue(TEXT("Retail house/map elements exist"),HouseLocationPage&&MapPage&&HousePin))return false;
	auto LocationLine=[&]() -> FString
	{
		for(const UTextBlock* Text:Binder->HouseTextRows)
			if(Text->GetVisibility()!=ESlateVisibility::Collapsed && Text->GetText().ToString().StartsWith(TEXT("Location: ")))
				return Text->GetText().ToString();
		return FString();
	};
	for(int32 Type:{1,2,3})
	{
		S.House.HouseType=Type;Binder->RefreshWorldHousePage(HouseLocationPage);Binder->RefreshWorldMapPage(MapPage);
		TestEqual(TEXT("Landscape house inside a building shows its retail outdoor coordinates"),LocationLine(),FString(TEXT("Location: 27.4N, 33.6E")));
		TestTrue(TEXT("Building-cell house retains its surface map pin"),bool(HousePin->bVisible));
	}
	const FIntPoint IndoorPin(HousePin->X,HousePin->Y);
	S.House.Position.CellId=static_cast<int32>(0xA9A1001Eu);Binder->RefreshWorldHousePage(HouseLocationPage);Binder->RefreshWorldMapPage(MapPage);
	TestEqual(TEXT("Outdoor and building cells give the same house location"),LocationLine(),FString(TEXT("Location: 27.4N, 33.6E")));
	TestEqual(TEXT("Outdoor and building cells share the house map pin"),FIntPoint(HousePin->X,HousePin->Y),IndoorPin);
	S.House.Position.CellId=0x565C0100;S.House.Position.Location=FVector(24,168,0);Binder->RefreshWorldHousePage(HouseLocationPage);
	TestEqual(TEXT("House coordinates retain southern and western hemispheres"),LocationLine(),FString(TEXT("Location: 27.6S, 33.0W")));
	S.House.HouseType=4;Binder->RefreshWorldHousePage(HouseLocationPage);Binder->RefreshWorldMapPage(MapPage);
	TestTrue(TEXT("Apartments do not invent a surface location or map pin"),LocationLine().IsEmpty()&&!HousePin->bVisible);
	S.House.HouseType=1;
	for(uint32 Cell:{0u,0xA9A10080u,0xFFFF0100u})
	{
		S.House.Position.CellId=static_cast<int32>(Cell);Binder->RefreshWorldHousePage(HouseLocationPage);Binder->RefreshWorldMapPage(MapPage);
		TestTrue(TEXT("Invalid or out-of-map housing coordinates hide stale location and pin"),LocationLine().IsEmpty()&&!HousePin->bVisible);
	}
	S.House.Position.CellId=static_cast<int32>(0xA9A10100u);S.House.Position.Location=FVector(84,132,0);
	Binder->RefreshWorldHousePage(HouseLocationPage);Draw(TEXT("HousingLocation135"),1.35f);
	TestEqual(TEXT("Retail housing paragraph font"),Binder->HouseTextElements[0]->FontId,0x40000000u);
	TestEqual(TEXT("Retail housing paragraphs are centered"),Binder->HouseTextElements[0]->TextHorizontalJustification,1u);
	TestEqual(TEXT("Retail housing paragraph spacing"),Binder->HouseTextElements[0]->TextMargins.Bottom,8.f);
	TestTrue(TEXT("Unpaid maintenance uses retail yellow"),Binder->HouseTextRows.ContainsByPredicate([](const UTextBlock* Text)
		{return Text->GetText().ToString().StartsWith(TEXT("Warning!")) && Text->GetColorAndOpacity().GetSpecifiedColor()==FLinearColor::Yellow;}));
	FACEBinaryWriter PaidRent; PaidRent.WriteUInt32(1); PaidRent.WriteInt32(1000); PaidRent.WriteInt32(1000); PaidRent.WriteUInt32(273);
	PaidRent.WriteString16L(TEXT("Pyreal")); PaidRent.WriteString16L(TEXT("Pyreals"));
	FACEBinaryReader PaidReader(PaidRent.GetData()); S.HandleHouseUpdateRentPayment(PaidReader);
	Binder->RefreshWorldOverlays();
	TestTrue(TEXT("Incremental payment updates the visible rent amounts"),Binder->HouseTextRows.ContainsByPredicate([](const UTextBlock* Text)
		{return Text->GetText().ToString()==TEXT("Rent:\n1000/1000 Pyreals");}));
	TestTrue(TEXT("Incremental payment changes the maintenance warning to paid"),Binder->HouseTextRows.ContainsByPredicate([](const UTextBlock* Text)
		{return Text->GetText().ToString().StartsWith(TEXT("The maintenance has already")) && Text->GetColorAndOpacity().GetSpecifiedColor()==FLinearColor::Green;}));
	FACEBinaryWriter Period; Period.WriteUInt32(1752592000); FACEBinaryReader PeriodReader(Period.GetData()); S.HandleHouseUpdateRentTime(PeriodReader);
	Binder->RefreshWorldOverlays();Draw(TEXT("HousingNewPeriod"));
	TestTrue(TEXT("New rent period redraws zero paid amounts"),Binder->HouseTextRows.ContainsByPredicate([](const UTextBlock* Text)
		{return Text->GetText().ToString()==TEXT("Rent:\n0/1000 Pyreals");}));
	TestTrue(TEXT("New rent period restores the unpaid warning"),Binder->HouseTextRows.ContainsByPredicate([](const UTextBlock* Text)
		{return Text->GetText().ToString().StartsWith(TEXT("Warning!")) && Text->GetColorAndOpacity().GetSpecifiedColor()==FLinearColor::Yellow;}));
	S.House.bMaintenanceFree=true;Binder->RefreshWorldOverlays();Draw(TEXT("HousingPaid"),1.25f);
	TestTrue(TEXT("Paid maintenance uses retail green"),Binder->HouseTextRows.ContainsByPredicate([](const UTextBlock* Text)
		{return Text->GetText().ToString().StartsWith(TEXT("The maintenance has already")) && Text->GetColorAndOpacity().GetSpecifiedColor()==FLinearColor::Green;}));
	S.PlayerVitals.StatQualityInts.Add(199,int32(FDateTime::UtcNow().ToUnixTimestamp()));Binder->RefreshWorldOverlays();
	TestTrue(TEXT("Housing shows landscape purchase cooldown"),Binder->HouseTextRows.ContainsByPredicate([](const UTextBlock* Text)
		{return Text->GetText().ToString().StartsWith(TEXT("You may buy another landscape house at"));}));
	const auto HousePage=Manager->FindElementByName(TEXT("HousePage"));
	const auto HouseList=Manager->FindElementUnder(TEXT("HousePage"),TEXT("HouseTextListBox"));
	HousePage->Height=150;Binder->RefreshWorldHousePage(HousePage);
	TestTrue(TEXT("Small housing panel has scrollable paragraphs"),Binder->HouseMaxScrollOffset>0);
	TestTrue(TEXT("Scaled housing list accepts its visible mouse position"),Binder->IsPointerOverHouseList(Canvas->LayoutToViewport(FVector2D(HouseList->GetScreenOrigin())+FVector2D(16,16))));
	for(int32 I=0;I<40;++I) Binder->ScrollHouseList(-1);
	TestEqual(TEXT("Housing scroll reaches the final paragraph"),Binder->HouseScrollOffset,Binder->HouseMaxScrollOffset);
	TestFalse(TEXT("Scrolled-off first paragraph is hidden"),bool(Binder->HouseTextElements[0]->bVisible));
	TestTrue(TEXT("Purchase cooldown remains reachable in short windows"),bool(Binder->HouseTextElements.Last()->bVisible));
	Load(0,4);S.HouseOffer.Buy.Reset();Binder->RefreshHouseOffer();
	TestTrue(TEXT("Zero-cost dwelling remains purchasable like retail"),S.CanPayHouse(false));
	return !HasAnyErrors();
}
#endif
