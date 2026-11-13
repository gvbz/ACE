#include "ACESession.h"
#include "ACEOlthoi.h"
#include "ACECombatTargeting.h"
#include "VR/ACEVRLocomotion.h"
#include "ACEInventoryRules.h"
#include "ACEProfiling.h"
#include "ACERetailChat.h"
#include "ACECharacterOptions.h"
#include "ACECombatStance.h"
#include "Protocol/ACEHash32.h"
#include "Protocol/ACEObjectCreateParser.h"
#include "Protocol/ACECombatChat.h"
#include "Protocol/ACELoginErrors.h"
#include "ACEOpcodes.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "AddressInfoTypes.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<float> CVarNetworkPumpMs(TEXT("ace.Network.PumpBudgetMs"),.5f,
	TEXT("Budget for each socket receive pass and the ordered gameplay pass, in milliseconds. A packet finishes atomically; 0 disables the time bound."));
static TAutoConsoleVariable<int32> CVarNetworkPumpPackets(TEXT("ace.Network.PumpMaxPackets"),128,
	TEXT("Maximum packets per socket receive pass and per ordered gameplay pass."));
static TAutoConsoleVariable<float> CVarNetworkCatchUpMs(TEXT("ace.Network.CatchUpBudgetMs"),8.f,
	TEXT("Total network catch-up allowance when dispatch is at least 100 ms behind. Socket catch-up is limited to 2 ms per socket. 0 disables catch-up; packet caps still apply."));

FACESession::FACESession() = default;

FACESession::~FACESession()
{
	Disconnect();
}

void FACESession::SetState(EACESessionState NewState)
{
	if (State == NewState)
	{
		return;
	}
	if (NewState != EACESessionState::InWorld) CancelPendingUse(TEXT("left world"));
	if (NewState == EACESessionState::Disconnected || NewState == EACESessionState::Failed)
	{
		bCharacterCreationPending = false;
		bHasServerTime = false;
		PortalYearTicksAtConnect = PortalYearTicksRealtime = 0.0;
	}
	State = NewState;
	if (State == EACESessionState::InWorld && bFellowshipUpdatesRequested)
		SendFellowshipUpdateRequest(true);
	OnStateChanged.Broadcast(State);
}

void FACESession::Log(const FString& Msg)
{
	UE_LOG(LogTemp, Log, TEXT("[ACE] %s"), *Msg);
	OnLog.Broadcast(Msg);
}

bool FACESession::Connect(const FACELoginCredentials& Credentials)
{
	Disconnect();

	Creds = Credentials;
	AccountName = Credentials.Account;
	SetState(EACESessionState::Connecting);

	if (!CreateSockets())
	{
		ConnectionError = TEXT("Could not open the network connection.");
		SetState(EACESessionState::Failed);
		Log(TEXT("Failed to create UDP sockets"));
		return false;
	}

	PacketTimeOrigin = FPlatformTime::Seconds();
	SendLoginRequest();
	LoginRequestAt = FPlatformTime::Seconds();
	SetState(EACESessionState::AwaitConnectRequest);
	Log(FString::Printf(TEXT("LoginRequest sent to %s:%d as '%s'"), *Creds.Host, Creds.Port, *Creds.Account));
	return true;
}

void FACESession::Disconnect()
{
	ResetBookEditing();
	Book = FACEBookInfo();
	PendingBookPage = INDEX_NONE;
	BookPageRequestedAt = 0.0;
	ManualAppraisalGuid = 0;
	bBackgroundAppraisalContext = false;
	PendingInventoryTransfers.Reset();
	GiveRecipientGuid = 0;
	Confirmations.Reset();
	ServerPopups.Reset();
	LastServerPacketAt = 0.0;
	LastServerDatagramAt = LastReceivePortKeepaliveAt = 0.0;
	LoginRequestAt = 0.0;
	bRecoverLostConnection = false;
	PreHandshakeDatagrams.Reset();
	ConnectionError.Reset();
	VRCapabilities = VRSequence = VRPoseSequence = 0; VRPoses.Reset(); VRSpellProfiles.Reset();
	bVRDiscoveryRequested = false;
	bLocalVRFeedback = false; LastVRPoseSent = -100.;
	VRRecoverySequence = VRRecoveryTeleport = 0; VRRecoveryReadyAt = 0; VRRecoveryDuration = 0;
	VRCastSequence=VRCastEpoch=VRCastPhase=VRAimCastSequence=0; VRAimUntil=VRCastReadyAt=VRNextAim=0; VRCastDuration=0;
	VRMissileWeapon = 0; VRMissileSpeed = 0.f;
	// Tell the server to drop us immediately so the account is not left online.
	// Must happen before CloseSockets / Isaac reset while we can still encrypt.
	if (SocketC2S && IssacClient && State != EACESessionState::Disconnected && State != EACESessionState::Failed)
	{
		SendDisconnectPacket();
	}

	bHasServerTime = false;
	PortalYearTicksAtConnect = PortalYearTicksRealtime = 0.0;
	bLogOffPending = false;
	LogOffPortalStartTime = 0.;
	LogOffTimeout = 0.f;
	LogOffRetransmitTimer = 0.f;
	CloseSockets();
	PacketTimeOrigin = 0.0;
	IssacClient.Reset();
	IssacServer.Reset();
	Characters.Reset();
	PreferredCharacterId = 0;
	ResetReceivedMessages();
	ChessBoardGuid = 0; ChessJoinPendingUntil = 0.0;
	WorldObjects.Reset();
	PlayerPositionQualities.Reset();
	bAfkMode=false;
	EquipmentRemovalEvidence.Reset();
	ObjectVisibilityDeadlines.Reset();
	ClosedContainerItemDeadlines.Reset();
	NextClosedContainerExpiryAt = 0.;
	PendingObjectPhysicsEvents.Reset();
	PendingObjectPhysicsBytes = 0;
	LastObjectPhysicsExpiryAt = 0.;
	ContainerContents.Reset();
	LoginEquipment.Reset();
	OpenExternalContainerGuid = 0;
	OpenVendorGuid = 0;
	VendorMerchandise.Reset();
	VendorBuyRate = 1.f;
	VendorItemTypes = MAX_uint32;
	VendorMinValue = VendorMaxValue = -1;
	VendorSellRate = 1.f;
	TradePartnerGuid = 0;
	TradeInitiatorGuid = 0;
	bTradeSelfAccepted = bTradePartnerAccepted = false;
	TradeStamp = 0.0;
	TradeSelfItems.Reset();
	TradeRemovedItems.Reset();
	TradePartnerItems.Reset();
	bUseBusy = false;
	ResetCursorWaits();
	CancelEquipmentSwap();
	CombatEventRevision = 0;
	bServerAttackInProgress = false;
	LastAttackError = 0;
	LastAttackerGuid = 0;
	LastAttackerTimeSeconds = 0.0;
	DisplayTitleId = 0;
	CharacterTitleIds.Reset();
	Fellowship = FACEFellowshipInfo();
	Allegiance = FACEAllegianceInfo();
	Friends.Reset();
	Squelches.Reset(); GlobalSquelchMask=0;
	Contracts.Reset();
	House = FACEHouseInfo();
	CloseHouseProfile();
	CloseBarber();
	LastTellSenderGuid = 0;
	LastTellSenderName.Reset();
	LastPatronTellSenderName.Reset(); LastMonarchTellSenderName.Reset();
	KnownSpells.Reset();
	SpellBars.Reset();
	SpellbookFilters = 0x3FFFu;
	InstanceSeq = ServerControlSeq = TeleportSeq = ForcePositionSeq = 0;
	++SpellDataRevision;
	ActiveSpellBar = 0;
	ShortcutObjects.Reset();
	bHasPlayerEncumbrance = false;
	PlayerEncumbranceVal = 0;
	ActiveEnchantments.Reset();
	VitaeCpPool = 0;
	DeathLevel = 0;
	LinkStatus = FACELinkStatus();
	PingRequestSentAt = 0.0;
	EchoTimeOrigin = 0.0;
	PendingEchoTimes.Reset();
	for (auto& Bucket : LinkTraffic) Bucket = FLinkTrafficBucket();
	PlayerGuid = 0;
	bLocalPlayerIsAdmin = false;
	CurrentStance = ACEMotion::StanceNonCombat;
	CurrentForwardCommand = ACEMotion::Ready;
	bEnteredWorldSent = false;
	PendingEnterCharacterId = 0;
	bLoginCompleteSent = false;
	bMoving = false;
	bForcePositionReporting = false;
	bHaveReportedPosition = false;
	bPositionReportingSuspended = false;
	bAutoPosContact = bLastReportedContact = true;
	AutoPosTimer = 0.f;
	NextPacketSequence = 2;
	NextFragmentSequence = 1;
	NextGameActionSequence = 1;
	LastReceivedPacketSequence = 1;
	CachedC2SPackets.Reset();
	OutOfOrderS2CPackets.Reset();
	PendingS2CDispatch.Empty();
	PendingS2CDispatchCount = 0;
	bNetworkSocketBacklog = bNetworkDispatchLimited = false;
	LastNetworkReceivedPackets = LastNetworkDispatchedWork = 0;
	LastNetworkReceiveMs = LastNetworkDispatchMs = LastNetworkDispatchBudgetMs = 0;
	LastTimeSyncPacketSequence = 0;
	RequestedS2CPackets.Reset();
	RejectedS2CPackets.Reset();
	LastRequestForRetransmitTime = 0.0;
	ConnectResponseRetryTimer = 0.f;
	CharacterListWaitTimer = 0.f;
	PendingCharacterMutation = 0;
	CharacterManagementError.Reset();
	ConnectResponseRetriesSent = 0;
	SetState(EACESessionState::Disconnected);
}

void FACESession::SendCharacterLogOff()
{
	SendGameMessage(ACEOpcode::CharacterLogOff, {}, ACEQueue::UIQueue, true);
	Log(TEXT("CharacterLogOff (0xF653) sent"));
}

void FACESession::SendDisconnectPacket()
{
	// Empty encrypted Disconnect header — NetworkSession.ProcessPacket → Terminate → DropSession.
	SendRawPacket(EACEPacketHeaderFlags::Disconnect | EACEPacketHeaderFlags::EncryptedChecksum,
		{}, {}, false, true, ClientId);
	Log(TEXT("Disconnect packet sent"));
}

void FACESession::RequestLogOff()
{
	if (State == EACESessionState::Disconnected || State == EACESessionState::Failed)
	{
		return;
	}

	if (State == EACESessionState::InWorld || State == EACESessionState::EnteringWorld)
	{
		if (bLogOffPending)
		{
			return;
		}
		// Leave combat before logout so the server action queue is not blocked on stance.
		if (State == EACESessionState::InWorld && PlayerVitals.CombatMode != static_cast<int32>(ACECombatMode::NonCombat))
		{
			SendChangeCombatMode(ACECombatMode::NonCombat);
		}
		SendStopMovement();
		FlushAutonomousPosition(bAutoPosContact);
		bLogOffPending = true;
		// CPlayerSystem::RequestLogOff / gmSmartBoxUI::UseTime. The server
		// drives the character motion; the client drives the portal viewport.
		const auto* Self = WorldObjects.Find(PlayerGuid);
		const bool bPK = Self ? Self->IsPkOrPkLite() : ACEPlayerKillerStatus::BlocksPlayers(PlayerVitals.PlayerKillerStatus);
		LogOffPortalStartTime = FPlatformTime::Seconds() + 3. + (bPK ? 20. : 0.);
		LogOffTimeout = 45.f; // logout anim + landblock remove + 6s SendFinalLogOffMessages
		LogOffRetransmitTimer = 0.f;
		SendCharacterLogOff();
		return;
	}

	// Character select / auth — close the account session entirely.
	Disconnect();
}

void FACESession::ClearWorldState()
{
	ResetBookEditing();
	Confirmations.Reset();
	ServerPopups.Reset();
	Book = FACEBookInfo();
	PendingBookPage = INDEX_NONE;
	BookPageRequestedAt = 0.0;
	ManualAppraisalGuid = 0;
	bBackgroundAppraisalContext = false;
	ObjectVisibilityDeadlines.Reset();
	ClosedContainerItemDeadlines.Reset();
	NextClosedContainerExpiryAt = 0.;
	PendingObjectPhysicsEvents.Reset();
	PendingObjectPhysicsBytes = 0;
	LastObjectPhysicsExpiryAt = 0.;
	VRCapabilities = VRSequence = VRPoseSequence = 0; VRPoses.Reset(); VRSpellProfiles.Reset();
	bVRDiscoveryRequested = false;
	bLocalVRFeedback = false; LastVRPoseSent = -100.;
	VRRecoverySequence = VRRecoveryTeleport = 0; VRRecoveryReadyAt = 0; VRRecoveryDuration = 0;
	VRCastSequence=VRCastEpoch=VRCastPhase=VRAimCastSequence=0; VRAimUntil=VRCastReadyAt=VRNextAim=0; VRCastDuration=0;
	VRMissileWeapon = 0; VRMissileSpeed = 0.f;
	TArray<int32> Guids;
	WorldObjects.GetKeys(Guids);
	for (const int32 Guid : Guids)
	{
		OnObjectDeleted.Broadcast(Guid);
	}
	ChessBoardGuid = 0; ChessJoinPendingUntil = 0.0;
	WorldObjects.Reset();
	PlayerPositionQualities.Reset();
	bAfkMode=false;
	EquipmentRemovalEvidence.Reset();
	ContainerContents.Reset();
	LoginEquipment.Reset();
	OpenExternalContainerGuid = 0;
	OpenVendorGuid = 0;
	VendorMerchandise.Reset();
	VendorBuyRate = 1.f;
	VendorItemTypes = MAX_uint32;
	VendorMinValue = VendorMaxValue = -1;
	VendorSellRate = 1.f;
	TradePartnerGuid = 0;
	TradeInitiatorGuid = 0;
	bTradeSelfAccepted = bTradePartnerAccepted = false;
	TradeStamp = 0.0;
	TradeSelfItems.Reset();
	TradeRemovedItems.Reset();
	TradePartnerItems.Reset();
	bUseBusy = false;
	ResetCursorWaits();
	CancelEquipmentSwap();
	CombatEventRevision = 0;
	bServerAttackInProgress = false;
	LastAttackError = 0;
	LastAttackerGuid = 0;
	LastAttackerTimeSeconds = 0.0;
	DisplayTitleId = 0;
	CharacterTitleIds.Reset();
	Fellowship = FACEFellowshipInfo();
	Allegiance = FACEAllegianceInfo();
	Friends.Reset();
	Squelches.Reset(); GlobalSquelchMask=0;
	Contracts.Reset();
	House = FACEHouseInfo();
	CloseHouseProfile();
	CloseBarber();
	LastTellSenderGuid = 0;
	LastTellSenderName.Reset();
	LastPatronTellSenderName.Reset(); LastMonarchTellSenderName.Reset();
	KnownSpells.Reset();
	SpellBars.Reset();
	SpellbookFilters = 0x3FFFu;
	InstanceSeq = ServerControlSeq = TeleportSeq = ForcePositionSeq = 0;
	++SpellDataRevision;
	ActiveSpellBar = 0;
	ShortcutObjects.Reset();
	bHasPlayerEncumbrance = false;
	PlayerEncumbranceVal = 0;
	ActiveEnchantments.Reset();
	VitaeCpPool = 0;
	DeathLevel = 0;
	LinkStatus = FACELinkStatus();
	PingRequestSentAt = 0.0;
	EchoTimeOrigin = 0.0;
	PendingEchoTimes.Reset();
	for (auto& Bucket : LinkTraffic) Bucket = FLinkTrafficBucket();
	PlayerGuid = 0;
	bLocalPlayerIsAdmin = false;
	PlayerPosition = FACEPosition();
	PlayerVitals = FACEPlayerVitals();
	SelectedObject = FACESelectedObject();
	SelectedManaQueryGuid = 0;
	PreviousSelectedObjectGuid = 0;
	GiveRecipientGuid = 0;
	CurrentStance = ACEMotion::StanceNonCombat;
	CurrentForwardCommand = ACEMotion::Ready;
	bEnteredWorldSent = false;
	PendingEnterCharacterId = 0;
	bLoginCompleteSent = false;
	bMoving = false;
	bForcePositionReporting = false;
	bHaveReportedPosition = false;
	bPositionReportingSuspended = false;
	bAutoPosContact = bLastReportedContact = true;
	AutoPosTimer = 0.f;
	ResetReceivedMessages();
}

bool FACESession::CreateSockets()
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		return false;
	}

	ServerC2SAddr = Sockets->CreateInternetAddr();
	bool bValid = false;
	ServerC2SAddr->SetIp(*Creds.Host, bValid);
	if (!bValid)
	{
		FIPv4Address Ip;
		if (FIPv4Address::Parse(Creds.Host, Ip))
		{
			ServerC2SAddr->SetIp(Ip.Value);
			bValid = true;
		}
		else
		{
			const FAddressInfoResult Info = Sockets->GetAddressInfo(
				*Creds.Host,
				nullptr,
				EAddressInfoFlags::Default,
				NAME_None,
				SOCKTYPE_Datagram);
			if (Info.Results.Num() > 0)
			{
				ServerC2SAddr = Info.Results[0].Address;
				bValid = true;
			}
		}
	}
	if (!bValid)
	{
		Log(TEXT("Invalid host address"));
		return false;
	}
	ServerC2SAddr->SetPort(Creds.Port);

	ServerS2CAddr = ServerC2SAddr->Clone();
	ServerS2CAddr->SetPort(Creds.Port + 1);

	SocketC2S = FUdpSocketBuilder(TEXT("ACE_C2S"))
		.AsNonBlocking()
		.WithReceiveBufferSize(1024 * 1024)
		.AsReusable()
		.BoundToPort(0)
		.Build();

	SocketS2C = FUdpSocketBuilder(TEXT("ACE_S2C"))
		.AsNonBlocking()
		.WithReceiveBufferSize(1024 * 1024)
		.AsReusable()
		.BoundToPort(0)
		.Build();

	return SocketC2S != nullptr && SocketS2C != nullptr;
}

void FACESession::CloseSockets()
{
	if (SocketC2S)
	{
		SocketC2S->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(SocketC2S);
		SocketC2S = nullptr;
	}
	if (SocketS2C)
	{
		SocketS2C->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(SocketS2C);
		SocketS2C = nullptr;
	}
}

bool FACESession::HasConnectionTimedOut(double Now) const
{
	return State >= EACESessionState::CharacterSelect && State <= EACESessionState::InWorld
		&& LastServerPacketAt > 0.0 && Now - LastServerPacketAt > 20.0;
}

void FACESession::Tick(float DeltaSeconds)
{
	ACE_PROFILE_SCOPE(Network);
	CheckBookWriteTimeout();
	for(auto It=PendingInventoryTransfers.CreateIterator();It;++It)
		if(State!=EACESessionState::InWorld||FPlatformTime::Seconds()>=It.Value())It.RemoveCurrent();
	if (PendingEquipmentGuid && FPlatformTime::Seconds() >= PendingEquipmentUntil)
	{
		CancelEquipmentSwap();
		EmitChatMessage(TEXT("Equipment change timed out. Please try again."), TEXT(""), ACEChatMessageType::TransientInfo);
	}

	if (PendingCharacterMutation && FPlatformTime::Seconds() - CharacterMutationSentAt > 20.0)
	{
		// Refresh the authoritative roster after a lost reply. Never retry a
		// destructive slot-based action against a potentially changed list.
		const FACELoginCredentials RetryCredentials = Creds;
		Connect(RetryCredentials);
		return;
	}
	// Check wall time before draining queued packets: after headset suspension,
	// old packets must not make a dead session appear alive again. Reauthenticate
	// once and stop at character selection; never automatically enter a character.
	if (bRecoverLostConnection || HasConnectionTimedOut(FPlatformTime::Seconds()))
	{
		const FACELoginCredentials RetryCredentials = Creds;
		const double Now = FPlatformTime::Seconds();
		Log(FString::Printf(TEXT("Connection lost (%s): authenticated silence=%.1fs, datagram silence=%.1fs, receive-port keepalive age=%.1fs, server sequence=%u, next client sequence=%u, reordered=%d, cached=%d. Reconnecting to character selection."),
			bRecoverLostConnection ? *ConnectionError : TEXT("receive timeout"),
			LastServerPacketAt > 0 ? Now-LastServerPacketAt : -1.0,
			LastServerDatagramAt > 0 ? Now-LastServerDatagramAt : -1.0,
			LastReceivePortKeepaliveAt > 0 ? Now-LastReceivePortKeepaliveAt : -1.0,
			LastReceivedPacketSequence, NextPacketSequence, OutOfOrderS2CPackets.Num(), CachedC2SPackets.Num()));
		Connect(RetryCredentials);
		return;
	}
	if (State == EACESessionState::Disconnected || State == EACESessionState::Failed)
	{
		return;
	}

	// Grace expired: drop pending sync. Never apply deferred NonCombat here — that was
	// SwitchCombatStyles noise; keeping optimistic CombatMode until PropertyInt confirms
	// the requested mode (or the player explicitly requests peace).
	if (PendingCombatMode != 0 && FPlatformTime::Seconds() >= PendingCombatModeUntil)
	{
		PendingCombatMode = 0;
		DeferredServerCombatMode = INDEX_NONE;
	}

	if (bLogOffPending)
	{
		LogOffTimeout -= DeltaSeconds;
		LogOffRetransmitTimer += DeltaSeconds;
		// UDP: retransmit CharacterLogOff every 2s while waiting for CharacterList.
		if (LogOffRetransmitTimer >= 2.f)
		{
			SendCharacterLogOff();
			LogOffRetransmitTimer = 0.f;
		}
		if (LogOffTimeout <= 0.f)
		{
			Log(TEXT("CharacterLogOff timed out waiting for CharacterList — sending Disconnect"));
			Disconnect();
			return;
		}
	}

	PollSockets();
	// Process ownership/reopen receipts before retiring closed-container records.
	// This runs even when UCM or world rendering is stopped.
	ExpireClosedContainerItems(FPlatformTime::Seconds());
	// Let a queued completion win over the fallback timeout.
	CheckPendingUseTimeout(FPlatformTime::Seconds());
	MaintainReceivePort(FPlatformTime::Seconds());
	if (State == EACESessionState::AwaitConnectRequest && LoginRequestAt > 0.0 && FPlatformTime::Seconds() - LoginRequestAt > 20.0)
	{
		ConnectionError = TEXT("The server did not respond. Check the server address and try connecting again.");
		Log(ConnectionError);
		SetState(EACESessionState::Failed);
		return;
	}
	RequestMissingS2CPackets(FPlatformTime::Seconds());

	if (State == EACESessionState::AwaitCharacterList)
	{
		CharacterListWaitTimer += DeltaSeconds;
		ConnectResponseRetryTimer += DeltaSeconds;

		// Retransmit ConnectResponse: ACE NetworkManager silently drops an early reply if
		// State is not yet AuthConnectResponse, and UDP to :port+1 can be lost.
		static constexpr float RetryIntervals[] = { 0.15f, 0.4f, 0.8f, 1.5f };
		if (ConnectResponseRetriesSent < UE_ARRAY_COUNT(RetryIntervals) &&
			ConnectResponseRetryTimer >= RetryIntervals[ConnectResponseRetriesSent])
		{
			SendConnectResponse();
			++ConnectResponseRetriesSent;
			Log(FString::Printf(TEXT("ConnectResponse retransmit #%d"), ConnectResponseRetriesSent));
		}

		if (CharacterListWaitTimer >= 20.f)
		{
			ConnectionError = FString::Printf(TEXT("The server did not return the character list. Check UDP ports %d/%d for %s."), Creds.Port, Creds.Port + 1, *Creds.Host);
			Log(TEXT("Timed out waiting for CharacterList — server may have missed ConnectResponse (UDP port+1 / auth race), or never sent the list"));
			SetState(EACESessionState::Failed);
			return;
		}
	}

	AckTimer += DeltaSeconds;
	// Retail-ish ACK cadence (~0.5s). Only contiguous LastReceived — never ACK past holes.
	if (bNeedAck && AckTimer >= 0.5f)
	{
		SendAckIfNeeded();
		AckTimer = 0.f;
	}

	EchoTimer += DeltaSeconds;
	if (State >= EACESessionState::CharacterSelect && EchoTimer >= 2.f)
	{
		// Periodic EchoRequest helps keep session alive / RTT
		FACEBinaryWriter Body;
		Body.WriteFloat(RecordEchoRequest(FPlatformTime::Seconds()));
		SendRawPacket(EACEPacketHeaderFlags::EchoRequest | EACEPacketHeaderFlags::EncryptedChecksum,
			Body.GetData(), {}, false, true, ClientId);
		EchoTimer = 0.f;
	}

	TickPositionReporting(DeltaSeconds);
}

void FACESession::TickPositionReporting(float DeltaSeconds)
{
	if (State == EACESessionState::InWorld && !bLogOffPending && !bPositionReportingSuspended)
	{
		AutoPosTimer = FMath::Min(AutoPosTimer + DeltaSeconds, AutonomousPositionInterval);
		// Retail CommandInterpreter::ShouldSendPositionEvent compares the actual
		// frame, not just input axes. Landing, ground settling and collision can
		// change the final pose after StopMovement has cleared those axes.
		const bool CellOrContactChanged = bHaveReportedPosition
			&& (LastReportedPosition.CellId != PlayerPosition.CellId || bLastReportedContact != bAutoPosContact);
		const bool PoseChanged = !bHaveReportedPosition || CellOrContactChanged
			|| !LastReportedPosition.Location.Equals(PlayerPosition.Location, .0002)
			|| !LastReportedPosition.RotationXYZ.Equals(PlayerPosition.RotationXYZ, .0002)
			|| !FMath::IsNearlyEqual(LastReportedPosition.RotationW, PlayerPosition.RotationW, .0002f);
		// SendVRPose now pairs each tracked sample with its feet. Do not send a
		// second timed report in the same frame; retain normal reporting if the
		// tracking stream pauses or the headset is disabled.
		const bool PoseReportsFeet=SupportsVRPoses() && FPlatformTime::Seconds()-LastVRPoseSent<.5;
		// CommandInterpreter::SendPositionEvent requires ground contact. JumpPack
		// and MoveToState describe airborne motion; rendering cannot inject extra
		// airborne placement or unchanged-position heartbeats into retail combat.
		if (!PoseReportsFeet && bAutoPosContact && (CellOrContactChanged
			|| (AutoPosTimer >= AutonomousPositionInterval && PoseChanged)))
		{
			SendAutonomousPosition(bAutoPosContact);
		}
	}
}

void FACESession::PollSockets()
{
	const double ReceiveStart = FPlatformTime::Seconds();
	const bool HadSocketBacklog = bNetworkSocketBacklog;
	bNetworkSocketBacklog = false;
	LastNetworkReceivedPackets = 0;
	uint8 Buffer[2048];
	int32 BytesRead = 0;
	TSharedRef<FInternetAddr> FromAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();

	auto Drain = [&](FSocket* Sock, bool bS2C)
	{
		if (!Sock)
		{
			return;
		}
		const double Start = FPlatformTime::Seconds();
		const double Base = FMath::Max(0.f, CVarNetworkPumpMs.GetValueOnGameThread()) * .001;
		// Preserve room for gameplay in the shared catch-up allowance.
		const double Budget = FMath::Min(NetworkPumpBudgetSeconds(HadSocketBacklog), FMath::Max(Base, .002));
		const int32 MaxPackets = FMath::Max(1, CVarNetworkPumpPackets.GetValueOnGameThread());
		int32 Packets = 0;
		while (Sock->RecvFrom(Buffer, sizeof(Buffer), BytesRead, *FromAddr) && BytesRead > 0)
		{
			// In particular, a cleartext login rejection must come from the endpoint
			// we contacted, not an unrelated sender on this ephemeral UDP socket.
			if ((ServerC2SAddr.IsValid() && *FromAddr == *ServerC2SAddr)
				|| (ServerS2CAddr.IsValid() && *FromAddr == *ServerS2CAddr))
			{
				LastServerDatagramAt = FPlatformTime::Seconds();
				HandleDatagram(Buffer, BytesRead, bS2C);
			}
			// Leave the remainder queued in the socket in its original order.
			// Continuous traffic must not monopolize an entire game frame.
			++LastNetworkReceivedPackets;
			if (++Packets >= MaxPackets || (Budget > 0 && FPlatformTime::Seconds() - Start >= Budget)) break;
		}
		uint32 PendingBytes = 0;
		bNetworkSocketBacklog |= Sock->HasPendingData(PendingBytes);
	};

	{
		// Keep transport replies responsive while gameplay is catching up. Retail
		// SharedNet reads nonblocking sockets under a time limit and hands blobs
		// to receive queues; recovering a gap cannot bypass our frame budget.
		TGuardValue<bool> DeferDispatch(bDeferOrderedDispatch, true);
		Drain(SocketC2S, false);
		Drain(SocketS2C, true);
	}
	const double DispatchStart = FPlatformTime::Seconds();
	LastNetworkReceiveMs = (DispatchStart - ReceiveStart) * 1000.0;
	// This must also run when both sockets are idle: a deferred contiguous
	// backlog needs no additional datagram to make progress.
	DrainOutOfOrderS2C(ReceiveStart);
	LastNetworkDispatchMs = (FPlatformTime::Seconds() - DispatchStart) * 1000.0;
}

double FACESession::NetworkDispatchAge(double Now) const
{
	double Oldest = Now;
	uint32 Sequence;
	if (PendingS2CDispatch.Peek(Sequence))
		if (const auto* Packet = OutOfOrderS2CPackets.Find(Sequence)) Oldest = FMath::Min(Oldest, Packet->ReceivedAt);
	int32 Owner;
	if (ReadyGameEventOwners.Peek(Owner))
		if (const auto* Stream = OrderedGameEvents.Find(Owner))
		{
			const uint32 Next = !Stream->bReceivedFirst && Stream->Pending.Contains(0) ? 0 : Stream->LastStamp+1;
			if (const auto* Event = Stream->Pending.Find(Next)) Oldest = FMath::Min(Oldest, Event->QueuedAt);
		}
	return FMath::Max(0.0, Now-Oldest);
}

double FACESession::NetworkPumpBudgetSeconds(bool bSocketBacklog) const
{
	const double Base = FMath::Max(0.f, CVarNetworkPumpMs.GetValueOnGameThread());
	if (Base == 0) return 0; // Existing unlimited diagnostic mode.
	const bool Behind = bSocketBacklog || NetworkDispatchAge(FPlatformTime::Seconds()) >= .1;
	return (Behind ? FMath::Max(Base, double(FMath::Clamp(CVarNetworkCatchUpMs.GetValueOnGameThread(), 0.f, 16.f))) : Base) * .001;
}

FString FACESession::DescribeNetworkPump() const
{
	const double Now = FPlatformTime::Seconds();
	double Oldest = Now;
	for (const auto& Pair : OutOfOrderS2CPackets) Oldest = FMath::Min(Oldest, Pair.Value.ReceivedAt);
	const uint32 Next = LastReceivedPacketSequence + 1;
	const bool WaitingForGap = !OutOfOrderS2CPackets.IsEmpty()
		&& !OutOfOrderS2CPackets.Contains(Next) && !RejectedS2CPackets.Contains(Next);
	double EventOldest = Now;
	for (const auto& Stream : OrderedGameEvents)
		for (const auto& Event : Stream.Value.Pending) EventOldest = FMath::Min(EventOldest, Event.Value.QueuedAt);
	return FString::Printf(TEXT("network receive=%.2f ms dispatch=%.2f/%.2f ms; received=%d work=%d socketPending=%d limited=%d; queued=%d events=%d dispatchAge=%.2f s eventAge=%.2f s; buffered=%d oldest=%.2f s missing=%u (last pump)"),
		LastNetworkReceiveMs, LastNetworkDispatchMs, LastNetworkDispatchBudgetMs, LastNetworkReceivedPackets, LastNetworkDispatchedWork,
		bNetworkSocketBacklog, bNetworkDispatchLimited, PendingS2CDispatchCount, PendingGameEventCount, NetworkDispatchAge(Now),
		FMath::Max(0.0, Now-EventOldest), OutOfOrderS2CPackets.Num(), FMath::Max(0.0, Now - Oldest), WaitingForGap ? Next : 0);
}

void FACESession::HandleDatagram(const uint8* Data, int32 Size, bool /*bFromS2CSocket*/)
{
	if (Size < PacketHeaderSize)
	{
		return;
	}

	FACEBinaryReader HeaderReader(Data, PacketHeaderSize);
	const uint32 Sequence = HeaderReader.ReadUInt32();
	const EACEPacketHeaderFlags Flags = static_cast<EACEPacketHeaderFlags>(HeaderReader.ReadUInt32());
	const uint32 Checksum = HeaderReader.ReadUInt32();
	const uint16 Id = HeaderReader.ReadUInt16();
	const uint16 Time = HeaderReader.ReadUInt16();
	const uint16 PayloadSize = HeaderReader.ReadUInt16();
	const uint16 Iteration = HeaderReader.ReadUInt16();

	if (PayloadSize > Size - PacketHeaderSize)
	{
		return;
	}

	const bool bEncrypted = EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::EncryptedChecksum);
	// Retail SharedNet::ProcessNewSeqNum rejects already received encrypted
	// sequences before decrypting them. Their ISAAC key was consumed on first
	// receipt; searching again advances the key window and emits false CRC errors.
	// Standalone NAKs reuse the current sequence and still need to be serviced.
	const auto SequenceFlags = Flags & ~(EACEPacketHeaderFlags::EncryptedChecksum | EACEPacketHeaderFlags::Retransmission);
	if (bEncrypted && Sequence != 0 && SequenceFlags != EACEPacketHeaderFlags::RequestRetransmit
		&& (Sequence <= LastReceivedPacketSequence || OutOfOrderS2CPackets.Contains(Sequence))) return;
	if (bEncrypted && !IssacServer && State == EACESessionState::AwaitConnectRequest)
	{
		// UDP can deliver an encrypted auth rejection before the cleartext seed
		// packet. Keep a bounded queue and verify its CRC after installing the
		// seeds; never dispatch unauthenticated content or treat it as corrupt yet.
		if (PreHandshakeDatagrams.Num() < 32 && Size <= 2048)
		{
			TArray<uint8>& Pending = PreHandshakeDatagrams.AddDefaulted_GetRef();
			Pending.Append(Data, Size);
		}
		return;
	}
	const uint32 HHash = HeaderHash32(Sequence, Flags, Id, Time, PayloadSize, Iteration);

	FACEBinaryReader Payload(Data + PacketHeaderSize, PayloadSize);
	TArray<uint8> OptionalBytes;
	float EchoClientTime = -1.f;
	float EchoResponseClientTime = -1.f;
	TArray<uint32> RetransmitSequences;
	TArray<uint32> RejectedSequences;
	uint32 ServerAckSequence = 0;
	bool bHasServerAck = false;
	uint32 NetErrorStringId = 0, NetErrorTableId = 0;
	const bool bHasNetError = EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::NetError | EACEPacketHeaderFlags::NetErrorDisconnect);
	double ServerTicks = 0.0;
	const double ReceivedAt = FPlatformTime::Seconds();

	auto Capture = [&](int32 Bytes)
	{
		if (!Payload.CanRead(Bytes))
		{
			return false;
		}
		const int32 Start = Payload.Tell();
		OptionalBytes.Append(Payload.GetData() + Start, Bytes);
		Payload.Skip(Bytes);
		return true;
	};

	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::RequestRetransmit))
	{
		if (!Payload.CanRead(4)) return;
		const int32 Start = Payload.Tell();
		const uint32 Count = Payload.ReadUInt32();
		if (Count > static_cast<uint32>(Payload.Remaining() / 4)) return;
		RetransmitSequences.Reserve(static_cast<int32>(Count));
		for (uint32 i = 0; i < Count; ++i)
		{
			RetransmitSequences.Add(Payload.ReadUInt32());
		}
		OptionalBytes.Append(Payload.GetData() + Start, 4 + static_cast<int32>(Count * 4));
	}
	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::RejectRetransmit))
	{
		if (!Payload.CanRead(4)) return;
		const int32 Start = Payload.Tell();
		const uint32 Count = Payload.ReadUInt32();
		if (Count > static_cast<uint32>(Payload.Remaining() / 4)) return;
		for (uint32 i = 0; i < Count; ++i) RejectedSequences.Add(Payload.ReadUInt32());
		OptionalBytes.Append(Payload.GetData() + Start, 4 + static_cast<int32>(Count * 4));
	}
	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::AckSequence))
	{
		if (!Payload.CanRead(4)) return;
		const int32 Start = Payload.Tell();
		ServerAckSequence = Payload.ReadUInt32();
		bHasServerAck = true;
		OptionalBytes.Append(Payload.GetData() + Start, 4);
	}
	// Retail NetError is a StringInfo pair, not a game-event error number.
	// Both optional headers occupy eight bytes and precede TimeSync on the wire.
	for (const auto ErrorFlag : {EACEPacketHeaderFlags::NetError, EACEPacketHeaderFlags::NetErrorDisconnect})
	{
		if (!EnumHasAnyFlags(Flags, ErrorFlag)) continue;
		if (!Payload.CanRead(8)) return;
		const int32 Start = Payload.Tell();
		NetErrorStringId = Payload.ReadUInt32();
		NetErrorTableId = Payload.ReadUInt32();
		OptionalBytes.Append(Payload.GetData() + Start, 8);
	}
	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::TimeSync))
	{
		if (!Payload.CanRead(8)) return;
		const int32 Start = Payload.Tell();
		ServerTicks = Payload.ReadDouble();
		OptionalBytes.Append(Payload.GetData() + Start, 8);

	}
	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::EchoRequest))
	{
		if (!Payload.CanRead(4)) return;
		const int32 Start = Payload.Tell();
		EchoClientTime = Payload.ReadFloat();
		OptionalBytes.Append(Payload.GetData() + Start, 4);
	}
	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::EchoResponse))
	{
		if (!Payload.CanRead(8)) return;
		const int32 Start = Payload.Tell();
		EchoResponseClientTime = Payload.ReadFloat();
		Payload.ReadFloat(); // server local delta (unused)
		OptionalBytes.Append(Payload.GetData() + Start, 8);
	}
	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::Flow))
	{
		if (!Capture(6)) return;
	}

	uint32 PayloadHash = FACEHash32::Calculate(OptionalBytes);
	TArray<FReceivedFragment> Fragments;

	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::BlobFragments))
	{
		while (Payload.Remaining() > 0)
		{
			if (!Payload.CanRead(FragmentHeaderSize))
			{
				return;
			}
			const int32 FragHeaderStart = Payload.Tell();
			const uint64 BlobId = Payload.ReadUInt64();
			const uint16 FragCount = Payload.ReadUInt16();
			const uint16 FragSize = Payload.ReadUInt16();
			const uint16 FragIndex = Payload.ReadUInt16();
			const uint16 FragQueue = Payload.ReadUInt16();

			const int32 DataLen = static_cast<int32>(FragSize) - FragmentHeaderSize;
			if (FragCount == 0 || FragIndex >= FragCount || DataLen <= 0
				|| DataLen > MaxFragmentDataSize || !Payload.CanRead(DataLen))
			{
				return;
			}

			const uint8* FragHeaderPtr = Payload.GetData() + FragHeaderStart;
			const uint8* FragDataPtr = Payload.GetData() + Payload.Tell();
			PayloadHash += FACEHash32::Calculate(FragHeaderPtr, FragmentHeaderSize);
			PayloadHash += FACEHash32::Calculate(FragDataPtr, DataLen);

			// Keep validated wire fragments local until CRC and packet sequencing pass.
			FReceivedFragment& Fragment = Fragments.AddDefaulted_GetRef();
			Fragment.BlobId = BlobId;
			Fragment.Count = FragCount;
			Fragment.Index = FragIndex;
			Fragment.Queue = FragQueue;
			Fragment.Data = Payload.ReadBytes(DataLen);
		}
	}
	else if (Payload.Remaining() > 0)
	{
		const int32 BodyLen = Payload.Remaining();
		PayloadHash += FACEHash32::Calculate(Payload.GetData() + Payload.Tell(), BodyLen);

		if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::ConnectRequest))
		{
			// A repeated handshake must not reset active keys, replay a rejection,
			// or move a failed/authenticated session back to character-list wait.
			if (State != EACESessionState::AwaitConnectRequest || BodyLen < 28) return;
			FACEBinaryReader BodyReader(Payload.GetData() + Payload.Tell(), BodyLen);
			// Verify CRC before handling
			bool bCrcOk = false;
			if (bEncrypted)
			{
				if (IssacServer)
				{
					const uint32 Key = (Checksum - HHash) ^ PayloadHash;
					if (IssacServer->Search(Key))
					{
						IssacServer->ConsumeKey(Key);
						bCrcOk = true;
					}
				}
			}
			else
			{
				bCrcOk = (HHash + PayloadHash) == Checksum;
			}

			if (bCrcOk)
			{
				LastServerPacketAt = ReceivedAt;
				HandleConnectRequest(BodyReader);
				if (Sequence != 0)
				{
					LastReceivedPacketSequence = Sequence;
					bNeedAck = true;
				}
				TArray<TArray<uint8>> Pending = MoveTemp(PreHandshakeDatagrams);
				PreHandshakeDatagrams.Reset();
				for (const TArray<uint8>& Datagram : Pending)
				{
					if (State == EACESessionState::Failed || State == EACESessionState::Disconnected) break;
					HandleDatagram(Datagram.GetData(), Datagram.Num(), true);
				}
			}
			return;
		}
	}

	bool bCrcOk = false;
	if (bEncrypted)
	{
		if (IssacServer)
		{
			const uint32 Key = (Checksum - HHash) ^ PayloadHash;
			if (IssacServer->Search(Key))
			{
				IssacServer->ConsumeKey(Key);
				bCrcOk = true;
			}
		}
	}
	else
	{
		bCrcOk = (HHash + PayloadHash) == Checksum;
	}

	if (!bCrcOk)
	{
		Log(TEXT("Packet CRC failed — dropped"));
		return;
	}
	if (bEncrypted) LastServerPacketAt = ReceivedAt;
	if (!RetransmitSequences.IsEmpty()) RecordLinkTraffic(ReceivedAt, 0, RetransmitSequences.Num());
	if (bHasNetError && (NetErrorStringId || NetErrorTableId))
	{
		if (State == EACESessionState::Failed || State == EACESessionState::Disconnected) return;
		// Initial GDLE rejections have no ISAAC keys yet. Once authenticated,
		// ignore delayed cleartext rejections from an earlier login attempt.
		if (!bEncrypted && State != EACESessionState::AwaitConnectRequest) return;
		const FString Reason = ACELoginErrors::Network(NetErrorStringId, NetErrorTableId);
		ConnectionError = FString::Printf(TEXT("%s (%s:%d; 0x%08X, table %u)"),
			*Reason, *Creds.Host, Creds.Port, NetErrorStringId, NetErrorTableId);
		Log(ConnectionError);
		bRecoverLostConnection = State == EACESessionState::InWorld;
		PreHandshakeDatagrams.Reset();
		SetState(EACESessionState::Failed);
		return;
	}

	// SharedNet processes transport headers as soon as the packet validates.
	// Missing gameplay data must not delay keepalive replies or RTT updates.
	if (FMath::IsFinite(EchoClientTime) && EchoClientTime >= 0.f) SendEchoResponse(EchoClientTime);
	if (FMath::IsFinite(EchoResponseClientTime) && EchoResponseClientTime >= 0.f)
		UpdateLinkStatusFromEcho(EchoResponseClientTime, ReceivedAt);

	// SharedNet::HandleNak services both checksum forms. A standalone NAK uses
	// the current sequence without consuming a slot (ACE NetworkSession too).
	if (!RetransmitSequences.IsEmpty()) HandleServerRequestRetransmit(RetransmitSequences);
	const auto NakFlags = Flags & ~(EACEPacketHeaderFlags::EncryptedChecksum | EACEPacketHeaderFlags::Retransmission);
	if (NakFlags == EACEPacketHeaderFlags::RequestRetransmit)
	{
		return;
	}
	// Retail HandleEmptyAck removes these IDs from its outstanding NAK set.
	// Our contiguous dispatcher must also release the holes, or every later
	// position/chat/update waits forever for a packet the server no longer has.
	// Never let an unsolicited rejection skip packets we have not requested.
	for (uint32 Rejected : RejectedSequences)
		if (Rejected > LastReceivedPacketSequence && RequestedS2CPackets.Contains(Rejected)
			&& !OutOfOrderS2CPackets.Contains(Rejected)) RejectedS2CPackets.Add(Rejected);

	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::Disconnect))
	{
		Log(TEXT("Server sent Disconnect"));
		bRecoverLostConnection = State == EACESessionState::InWorld;
		ConnectionError = TEXT("The connection to the server was lost.");
		SetState(EACESessionState::Failed);
		return;
	}

	// Server AckSequence optional: prune C2S send cache (order-insensitive, like ACE).
	if (bHasServerAck)
	{
		TArray<uint32> ToRemove;
		for (const auto& Pair : CachedC2SPackets)
		{
			if (Pair.Key < ServerAckSequence)
			{
				ToRemove.Add(Pair.Key);
			}
		}
		for (uint32 Seq : ToRemove)
		{
			CachedC2SPackets.Remove(Seq);
		}
	}

	const bool bAckOnly = Flags == EACEPacketHeaderFlags::AckSequence;
	if (Sequence == 0 || bAckOnly)
	{
		ProcessOrderedS2CPacket(Sequence, Flags, Fragments, ServerTicks, ReceivedAt);
		return;
	}

	// SharedNet::ProcessNewestSeqNum records gaps independently of blob delivery.
	// Keep a contiguous ACK cursor for ACE, but dispatch each received packet now.
	if (Sequence <= LastReceivedPacketSequence)
	{
		return; // duplicate / already handled
	}

	// Queue even the missing packet itself so it shares the same dispatch limit
	// as the backlog it releases. Do not overwrite a packet already retained.
	if (!OutOfOrderS2CPackets.Contains(Sequence))
	{
		if (OutOfOrderS2CPackets.Num() >= 32768)
		{
			ConnectionError = TEXT("Too many unacknowledged network packets. Please reconnect to the server.");
			Log(ConnectionError); SetState(EACESessionState::Failed); return;
		}
		FPendingS2CPacket& Pending = OutOfOrderS2CPackets.Add(Sequence);
		Pending.Flags = Flags;
		Pending.Fragments = MoveTemp(Fragments);
		Pending.ServerTicks = ServerTicks;
		Pending.ReceivedAt = ReceivedAt;
		PendingS2CDispatch.Enqueue(Sequence);
		++PendingS2CDispatchCount;
	}
	if (!bDeferOrderedDispatch) DrainOutOfOrderS2C();
	// Tick requests missing packets once after the receive/dispatch passes;
	// scanning a large backlog for every newly received packet is quadratic.
}

void FACESession::ProcessOrderedS2CPacket(uint32 Sequence, EACEPacketHeaderFlags Flags,
	const TArray<FReceivedFragment>& Fragments, double ServerTicks, double ReceivedAt)
{
	if (Sequence != 0 && Flags != EACEPacketHeaderFlags::AckSequence)
	{
		RequestedS2CPackets.Remove(Sequence);
		RejectedS2CPackets.Remove(Sequence);
		bNeedAck = true;
	}

	// Late retransmissions must not move the synchronized world clock backwards.
	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::TimeSync)
		&& (Sequence == 0 || Sequence > LastTimeSyncPacketSequence))
	{
		LastTimeSyncPacketSequence = FMath::Max(LastTimeSyncPacketSequence, Sequence);
		ApplyServerTime(ServerTicks, ReceivedAt, TEXT("TimeSync"));
	}
	for (const FReceivedFragment& Fragment : Fragments) ProcessReceivedFragment(Fragment);
}

void FACESession::RequestMissingS2CPackets(double Now)
{
	if (OutOfOrderS2CPackets.IsEmpty() || Now - LastRequestForRetransmitTime < 1.0) return;
	uint32 Highest = LastReceivedPacketSequence;
	double Oldest = Now;
	for (const auto& Pair : OutOfOrderS2CPackets)
	{
		Highest = FMath::Max(Highest, Pair.Key);
		Oldest = FMath::Min(Oldest, Pair.Value.ReceivedAt);
	}
	// Allow normal UDP reordering, then retry even a single hole when no more
	// packets arrive. Waiting for three later packets could stall the world forever.
	if (Now - Oldest < .15) return;
	TArray<uint32> Missing;
	for (uint32 Seq = LastReceivedPacketSequence + 1; Seq < Highest && Missing.Num() < 115; ++Seq)
		if (!OutOfOrderS2CPackets.Contains(Seq) && !RejectedS2CPackets.Contains(Seq)) Missing.Add(Seq);
	if (Missing.IsEmpty()) return;
	for (uint32 Sequence : Missing) RequestedS2CPackets.Add(Sequence);
	SendRequestRetransmit(Missing);
	LastRequestForRetransmitTime = Now;
}

void FACESession::DrainOutOfOrderS2C(double FrameStartedAt)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_OrderedNetworkDispatch);
	const double Start = FPlatformTime::Seconds();
	const double Budget = NetworkPumpBudgetSeconds(false);
	const double Base = FMath::Max(0.f, CVarNetworkPumpMs.GetValueOnGameThread()) * .001;
	// Catch-up is a total per-pump allowance, not eight extra milliseconds for
	// each socket plus another eight for gameplay. Atomic work still completes.
	const double Deadline = Budget > Base && FrameStartedAt > 0.0 ? FrameStartedAt + Budget : Start + Budget;
	LastNetworkDispatchBudgetMs = FMath::Max(0.0, Deadline-Start) * 1000.0;
	LastNetworkDispatchedWork = 0;
	const int32 MaxPackets = FMath::Max(1, CVarNetworkPumpPackets.GetValueOnGameThread());
	for (int32 Packets = 0; Packets < MaxPackets; ++Packets)
	{
		// Always permit one atomic packet, then yield the rest to later frames.
		if (Packets > 0 && Budget > 0 && FPlatformTime::Seconds() >= Deadline) break;
		if (State == EACESessionState::Failed) break;
		// Alternate packet and ordered-event work so neither starves the other.
		if (!ReadyGameEventOwners.IsEmpty() && ((Packets & 1) || PendingS2CDispatch.IsEmpty()))
		{
			DispatchReadyGameEvent();
			++LastNetworkDispatchedWork;
			continue;
		}
		uint32 Seq;
		if (!PendingS2CDispatch.Dequeue(Seq)) break;
		--PendingS2CDispatchCount;
		++LastNetworkDispatchedWork;
		if (FPendingS2CPacket* Pending = OutOfOrderS2CPackets.Find(Seq))
		{
			// Retain only the receipt until the ACK cursor crosses it. Never keep
			// complete gameplay payloads behind a missing UDP packet.
			FPendingS2CPacket Copy = MoveTemp(*Pending);
			Pending->bDispatched = true;
			Pending->Fragments.Reset();
			ProcessOrderedS2CPacket(Seq, Copy.Flags, Copy.Fragments, Copy.ServerTicks, Copy.ReceivedAt);
		}
	}
	bNetworkDispatchLimited = !PendingS2CDispatch.IsEmpty() || !ReadyGameEventOwners.IsEmpty();
	// Receipt pruning is bounded as well: filling a long-lived hole must not
	// walk an arbitrarily large backlog in one render frame.
	for (int32 Count = 0; Count < MaxPackets; ++Count)
	{
		const uint32 Seq = LastReceivedPacketSequence + 1;
		const FPendingS2CPacket* Receipt = OutOfOrderS2CPackets.Find(Seq);
		if (Receipt ? !Receipt->bDispatched : !RejectedS2CPackets.Contains(Seq)) break;
		OutOfOrderS2CPackets.Remove(Seq);
		RequestedS2CPackets.Remove(Seq); RejectedS2CPackets.Remove(Seq);
		LastReceivedPacketSequence = Seq; bNeedAck = true;
	}
}

void FACESession::ResetReceivedMessages()
{
	PartialFragments.Reset();
	EphemeralMessages.Reset();
	PartialMessageBytes = 0;
	LastEphemeralExpiry = 0;
	OrderedGameEvents.Reset(); ReadyGameEventOwners.Empty();
	PendingGameEventBytes = PendingGameEventCount = 0;
	PropertyStamps.Reset();
}

void FACESession::RemovePartialMessage(uint64 BlobId)
{
	if (const FPartialMessage* Partial = PartialFragments.Find(BlobId))
		PartialMessageBytes -= Partial->Bytes;
	PartialFragments.Remove(BlobId);
}

void FACESession::ExpireEphemeralMessages(double Now)
{
	// Indicator::FlushTimedOutEphInfo / ArrivedEphInfo::fTimedOut: five-second
	// sweeps of ordering-stamp history, not a timeout for reliable message data.
	if (Now - LastEphemeralExpiry < 5.) return;
	LastEphemeralExpiry = Now;
	for (auto It = EphemeralMessages.CreateIterator(); It; ++It)
		if (Now - It.Value().UpdatedAt > 5.) It.RemoveCurrent();
}

void FACESession::ProcessReceivedFragment(const FReceivedFragment& Fragment)
{
	if (State == EACESessionState::Failed) return;
	// PacketController::AddReceivedBlobToQueue accepts queues 1..11. Discard
	// only this blob, preserving other valid fragments in the same packet.
	if (Fragment.Queue == 0 || Fragment.Queue >= 12) return;
	const double Now = FPlatformTime::Seconds();
	ExpireEphemeralMessages(Now);
	if (Fragment.BlobId & 0x8000000000000000ull)
	{
		// NetBlobIDUtils: sequence includes the server byte; bits 32..47 are
		// the wrapping ordering stamp. Never join two versions of the same blob.
		const uint64 SequenceId = Fragment.BlobId & 0x00FF0000FFFFFFFFull;
		if (FEphemeralMessage* Latest = EphemeralMessages.Find(SequenceId))
		{
			if (Latest->BlobId != Fragment.BlobId)
			{
				const uint16 PreviousStamp = uint16(Latest->BlobId >> 32);
				const uint16 IncomingStamp = uint16(Fragment.BlobId >> 32);
				if (ACEPhysicsTimeStamp::IsNewer(IncomingStamp, PreviousStamp)) return;
				RemovePartialMessage(Latest->BlobId);
				Latest->BlobId = Fragment.BlobId;
				Latest->UpdatedAt = Now;
			}
		}
		else
		{
			EphemeralMessages.Add(SequenceId, {Fragment.BlobId, Now});
		}
	}
	if (Fragment.Count == 1)
	{
		// A conflicting single-fragment body must not bypass an existing assembly.
		if (PartialFragments.Contains(Fragment.BlobId)) return;
		ReceiveGameMessage(Fragment.Data);
		return;
	}
	if (const FPartialMessage* Existing = PartialFragments.Find(Fragment.BlobId))
	{
		if (Existing->Count != Fragment.Count || Existing->Queue != Fragment.Queue) return;
		if (Existing->Parts.Contains(Fragment.Index)) return;
	}
	// Modern resource guard, not a retail timeout: keep delayed reliable data
	// until completion. If bounds are exhausted, report failure rather than silently
	// discarding inventory/world updates and continuing with inconsistent state.
	if ((!PartialFragments.Contains(Fragment.BlobId) && PartialFragments.Num() >= MaxPartialMessages)
		|| PartialMessageBytes + Fragment.Data.Num() > MaxPartialMessageBytes)
	{
		ConnectionError = TEXT("Too many incomplete network messages. Please reconnect to the server.");
		Log(ConnectionError);
		ResetReceivedMessages();
		SetState(EACESessionState::Failed);
		return;
	}
	FPartialMessage& Partial = PartialFragments.FindOrAdd(Fragment.BlobId);
	Partial.Count = Fragment.Count;
	Partial.Queue = Fragment.Queue;
	Partial.Parts.Add(Fragment.Index, Fragment.Data);
	Partial.Bytes += Fragment.Data.Num();
	PartialMessageBytes += Fragment.Data.Num();
	if (Partial.Parts.Num() != Partial.Count) return;
	TArray<uint8> Message;
	for (uint16 Index=0; Index<Partial.Count; ++Index)
	{
		const auto* Part = Partial.Parts.Find(Index);
		if (!Part) return;
		Message.Append(*Part);
	}
	RemovePartialMessage(Fragment.BlobId);
	ReceiveGameMessage(Message);
}

void FACESession::SendRequestRetransmit(const TArray<uint32>& MissingSequences)
{
	if (MissingSequences.Num() == 0)
	{
		return;
	}
	FACEBinaryWriter Body;
	Body.WriteUInt32(static_cast<uint32>(MissingSequences.Num()));
	for (uint32 Seq : MissingSequences)
	{
		Body.WriteUInt32(Seq);
	}
	// Cleartext NAK — sequence does not advance (ACE FlushPackets isNak path).
	SendRawPacket(EACEPacketHeaderFlags::RequestRetransmit, Body.GetData(), {}, false, false, ClientId);
}

void FACESession::HandleServerRequestRetransmit(const TArray<uint32>& Sequences)
{
	TArray<uint32> Missing;
	for (uint32 Seq : Sequences)
	{
		if (const FCachedC2SPacket* Cached = CachedC2SPackets.Find(Seq))
		{
			// Rebuild wire packet with Retransmission flag; reuse original Isaac XOR.
			EACEPacketHeaderFlags Flags = Cached->Flags | EACEPacketHeaderFlags::Retransmission;
			TArray<uint8> Packet;
			Packet.SetNumZeroed(PacketHeaderSize);
			Packet.Append(Cached->Payload);
			const uint16 PayloadSize = static_cast<uint16>(Cached->Payload.Num());
			const uint16 Time = PacketIntervalAt(FPlatformTime::Seconds());
			constexpr uint16 Iteration = 1;
			const uint32 HHash = HeaderHash32(Seq, Flags, ClientId, Time, PayloadSize, Iteration);
			// Optional headers and fragments are hashed separately on the wire.
			// Hashing their concatenation changes the checksum at unaligned boundaries.
			const uint32 FinalChecksum = HHash + (Cached->PayloadHash ^ Cached->IsaacXor);
			auto W32 = [&](int32 Off, uint32 V)
			{
				Packet[Off] = static_cast<uint8>(V & 0xFF);
				Packet[Off + 1] = static_cast<uint8>((V >> 8) & 0xFF);
				Packet[Off + 2] = static_cast<uint8>((V >> 16) & 0xFF);
				Packet[Off + 3] = static_cast<uint8>((V >> 24) & 0xFF);
			};
			auto W16 = [&](int32 Off, uint16 V)
			{
				Packet[Off] = static_cast<uint8>(V & 0xFF);
				Packet[Off + 1] = static_cast<uint8>((V >> 8) & 0xFF);
			};
			W32(0, Seq);
			W32(4, static_cast<uint32>(Flags));
			W32(8, FinalChecksum);
			W16(12, ClientId);
			W16(14, Time);
			W16(16, PayloadSize);
			W16(18, Iteration);
			if (SocketC2S && ServerC2SAddr.IsValid())
			{
				int32 Sent = 0;
				SocketC2S->SendTo(Packet.GetData(), Packet.Num(), Sent, *ServerC2SAddr);
			}
		}
		else
		{
			Missing.Add(Seq);
		}
	}
	if (Missing.Num() > 0)
	{
		FACEBinaryWriter Body;
		Body.WriteUInt32(static_cast<uint32>(Missing.Num()));
		for (uint32 Seq : Missing)
		{
			Body.WriteUInt32(Seq);
		}
		SendRawPacket(EACEPacketHeaderFlags::RejectRetransmit, Body.GetData(), {}, false, false, ClientId);
	}
}

void FACESession::CacheOutboundPacket(uint32 Sequence, EACEPacketHeaderFlags Flags, uint32 IsaacXor, uint32 PayloadHash, const TArray<uint8>& PayloadAfterHeader)
{
	if (Sequence < 2)
	{
		return;
	}
	FCachedC2SPacket& Entry = CachedC2SPackets.FindOrAdd(Sequence);
	Entry.Flags = Flags;
	Entry.IsaacXor = IsaacXor;
	Entry.PayloadHash = PayloadHash;
	Entry.Payload = PayloadAfterHeader;
	// Bound cache — prune oldest if huge (server ACK normally clears).
	constexpr int32 MaxCached = 512;
	if (CachedC2SPackets.Num() > MaxCached)
	{
		uint32 MinSeq = MAX_uint32;
		for (const auto& Pair : CachedC2SPackets)
		{
			MinSeq = FMath::Min(MinSeq, Pair.Key);
		}
		CachedC2SPackets.Remove(MinSeq);
	}
}

uint32 FACESession::HeaderHash32(uint32 Sequence, EACEPacketHeaderFlags Flags, uint16 Id, uint16 Time, uint16 Size, uint16 Iteration)
{
	uint8 Buf[20];
	auto Write32 = [&](int32 Off, uint32 V)
	{
		Buf[Off] = V & 0xFF;
		Buf[Off + 1] = (V >> 8) & 0xFF;
		Buf[Off + 2] = (V >> 16) & 0xFF;
		Buf[Off + 3] = (V >> 24) & 0xFF;
	};
	auto Write16 = [&](int32 Off, uint16 V)
	{
		Buf[Off] = V & 0xFF;
		Buf[Off + 1] = (V >> 8) & 0xFF;
	};
	Write32(0, Sequence);
	Write32(4, static_cast<uint32>(Flags));
	Write32(8, HeaderChecksumMagic);
	Write16(12, Id);
	Write16(14, Time);
	Write16(16, Size);
	Write16(18, Iteration);
	return FACEHash32::Calculate(Buf, 20);
}

void FACESession::HandleConnectRequest(FACEBinaryReader& Body)
{
	const double ServerTime = Body.ReadDouble();
	// ACE sends Timers.PortalYearTicks (Dereth game seconds) — drives the client sky day cycle.
	ApplyServerTime(ServerTime, FPlatformTime::Seconds(), TEXT("ConnectRequest"));
	ConnectionCookie = Body.ReadUInt64();
	ClientId = static_cast<uint16>(Body.ReadUInt32());
	for (int32 i = 0; i < 4; ++i) ServerSeed[i] = Body.ReadUInt8();
	for (int32 i = 0; i < 4; ++i) ClientSeed[i] = Body.ReadUInt8();

	IssacServer = MakeUnique<FACECryptoSystem>(ServerSeed);
	IssacClient = MakeUnique<FACEIsaac>(ClientSeed);

	Log(FString::Printf(TEXT("ConnectRequest: ClientId=%u Cookie=%llu ServerTime=%f"), ClientId, ConnectionCookie, ServerTime));

	ConnectResponseRetryTimer = 0.f;
	CharacterListWaitTimer = 0.f;
	ConnectResponseRetriesSent = 0;
	SendConnectResponse();
	SetState(EACESessionState::AwaitCharacterList);
}

TArray<uint8> FACESession::BuildLoginRequestBody(const FACELoginCredentials& Creds)
{
	FACEBinaryWriter Body;
	Body.WriteString16L(TEXT("1802"));

	FACEBinaryWriter Auth;
	Auth.WriteUInt32(Creds.bGDLE ? 1 : 2); // NetAuthType::Account / AccountPassword
	Auth.WriteUInt32(0); // AuthFlags
	Auth.WriteUInt32(0); // Timestamp
	if (Creds.bGDLE)
	{
		// ThwargLauncher passes GDLE credentials via retail's -a argument.
		// Client::EvaluateCommandLineArg lowercases that entire argument (including
		// the password) before NetAuthenticator packs it. Match its ASCII casing
		// on the wire only; never alter saved credentials or ACE's case-sensitive ticket.
		FString RetailAccount = Creds.Account + TEXT(":") + Creds.Password;
		for (TCHAR& C : RetailAccount) if (C >= TEXT('A') && C <= TEXT('Z')) C += TEXT('a') - TEXT('A');
		Auth.WriteString16L(RetailAccount);
		Auth.WriteUInt32(0); // NetAuthenticator crypto-data length
		Auth.WriteUInt32(0); // NetAuthenticator extra-data length
	}
	else
	{
		Auth.WriteString16L(Creds.Account);
		Auth.WriteString16L(TEXT("")); // account override
		Auth.WriteString32L(Creds.Password);
	}

	Body.WriteUInt32(static_cast<uint32>(Auth.Num()));
	Body.WriteBytes(Auth.GetData());

	return Body.GetData();
}

void FACESession::SendLoginRequest()
{
	SendRawPacket(EACEPacketHeaderFlags::LoginRequest, BuildLoginRequestBody(Creds), {}, false, false, 0);
}

void FACESession::SendConnectResponse()
{
	FACEBinaryWriter Body;
	Body.WriteUInt64(ConnectionCookie);
	// GDLE routes the acknowledgement by its assigned recipient ID. ACE instead
	// resolves the cookie on port+1 and expects the existing zero-ID response.
	SendRawPacket(EACEPacketHeaderFlags::ConnectResponse, Body.GetData(), {}, true, false, Creds.bGDLE ? ClientId : 0);
	LastReceivePortKeepaliveAt = FPlatformTime::Seconds();
	Log(TEXT("ConnectResponse sent on port+1"));
}

void FACESession::MaintainReceivePort(double Now)
{
	if (State < EACESessionState::CharacterSelect || State > EACESessionState::InWorld
		|| !SocketS2C || !ServerS2CAddr.IsValid() || LastReceivePortKeepaliveAt <= 0.0
		|| Now - LastReceivePortKeepaliveAt < ReceivePortKeepaliveInterval) return;
	// Retail ClientFlowQueue::IncrementLocalInterval sends CICMD cmdNOP=1,
	// Param=0 from the receive socket to server port+1. Echo/ACK on port+0
	// cannot refresh this separate NAT mapping. ACE accepts this header on
	// listener 1 with ID zero; it must not consume gameplay sequence/ISAAC state.
	// Android uses the same packet every 20 seconds: cellular/hotspot NATs can
	// discard this mapping before retail's 110-second desktop interval elapses.
	FACEBinaryWriter Body;
	Body.WriteUInt32(1); Body.WriteUInt32(0);
	SendRawPacket(EACEPacketHeaderFlags::CICMDCommand, Body.GetData(), {}, true, false, 0);
	LastReceivePortKeepaliveAt = Now;
	UE_LOG(LogTemp, Verbose, TEXT("ACE: receive-port keepalive sent"));
}

uint16 FACESession::PacketIntervalAt(double Now) const
{
	// Retail FlowQueue advances interval_ once every 0.5 seconds. GDLE's
	// speed-hack check compares this counter with elapsed wall time. CPU cycles
	// are unrelated and can make ordinary bursts of item actions look accelerated.
	if (PacketTimeOrigin <= 0.0) return 0;
	const uint64 Intervals = static_cast<uint64>(FMath::Max(0.0, Now - PacketTimeOrigin) * 2.0);
	return static_cast<uint16>(Intervals & 0xFFFFu);
}

void FACESession::SendRawPacket(
	EACEPacketHeaderFlags Flags,
	const TArray<uint8>& Body,
	const TArray<TArray<uint8>>& Fragments,
	bool bToS2CPort,
	bool bEncrypted,
	uint16 HeaderId)
{
	FSocket* Sock = bToS2CPort ? SocketS2C : SocketC2S;
	TSharedPtr<FInternetAddr> Dest = bToS2CPort ? ServerS2CAddr : ServerC2SAddr;
	if (!Sock || !Dest.IsValid())
	{
		return;
	}

	if (Fragments.Num() > 0)
	{
		Flags |= EACEPacketHeaderFlags::BlobFragments;
	}
	if (bEncrypted)
	{
		Flags |= EACEPacketHeaderFlags::EncryptedChecksum;
	}

	uint32 IsaacXor = 0;
	if (bEncrypted)
	{
		if (!IssacClient)
		{
			return;
		}
		IsaacXor = IssacClient->Next();
	}

	uint32 Sequence = 0;
	const bool bIsNak = EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::RequestRetransmit);
	// Mirror ACE.Server FlushPackets: only exact AckSequence (no other flags) or NAK
	// reuse CurrentValue without consuming a sequence slot.
	if (EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::LoginRequest) ||
		EnumHasAnyFlags(Flags, EACEPacketHeaderFlags::ConnectResponse | EACEPacketHeaderFlags::CICMDCommand))
	{
		Sequence = 0;
	}
	else if (Flags == EACEPacketHeaderFlags::AckSequence || bIsNak)
	{
		Sequence = NextPacketSequence;
	}
	else
	{
		Sequence = NextPacketSequence++;
	}

	const bool bConnectionless = Flags == EACEPacketHeaderFlags::CICMDCommand;
	const uint16 Time = bConnectionless ? 0 : PacketIntervalAt(FPlatformTime::Seconds());
	const uint16 Iteration = bConnectionless ? 0 : 1;

	TArray<uint8> Packet;
	Packet.SetNumZeroed(PacketHeaderSize);

	uint32 PayloadHash = 0;
	if (Body.Num() > 0)
	{
		Packet.Append(Body);
		PayloadHash += FACEHash32::Calculate(Body);
	}
	for (const TArray<uint8>& FullFrag : Fragments)
	{
		Packet.Append(FullFrag);
		PayloadHash += FACEHash32::Calculate(FullFrag);
	}

	const uint16 PayloadSize = static_cast<uint16>(Packet.Num() - PacketHeaderSize);
	const uint32 HHash = HeaderHash32(Sequence, Flags, HeaderId, Time, PayloadSize, Iteration);
	const uint32 FinalChecksum = HHash + (PayloadHash ^ IsaacXor);

	auto W32 = [&](int32 Off, uint32 V)
	{
		Packet[Off] = static_cast<uint8>(V & 0xFF);
		Packet[Off + 1] = static_cast<uint8>((V >> 8) & 0xFF);
		Packet[Off + 2] = static_cast<uint8>((V >> 16) & 0xFF);
		Packet[Off + 3] = static_cast<uint8>((V >> 24) & 0xFF);
	};
	auto W16 = [&](int32 Off, uint16 V)
	{
		Packet[Off] = static_cast<uint8>(V & 0xFF);
		Packet[Off + 1] = static_cast<uint8>((V >> 8) & 0xFF);
	};
	W32(0, Sequence);
	W32(4, static_cast<uint32>(Flags));
	W32(8, FinalChecksum);
	W16(12, HeaderId);
	W16(14, Time);
	W16(16, PayloadSize);
	W16(18, Iteration);

	// Cache for server NAK retransmit (seq >= 2, not NAK itself).
	if (Sequence >= 2 && !bIsNak)
	{
		TArray<uint8> PayloadAfterHeader;
		PayloadAfterHeader.Append(Packet.GetData() + PacketHeaderSize, PayloadSize);
		CacheOutboundPacket(Sequence, Flags, IsaacXor, PayloadHash, PayloadAfterHeader);
	}

	int32 Sent = 0;
	Sock->SendTo(Packet.GetData(), Packet.Num(), Sent, *Dest);
	if (Sent == Packet.Num()) RecordLinkTraffic(FPlatformTime::Seconds(), 1, 0);
}

void FACESession::SendGameMessage(uint32 Opcode, const TArray<uint8>& PayloadAfterOpcode, uint16 Queue, bool bEncrypted)
{
	FACEBinaryWriter Msg;
	Msg.WriteUInt32(Opcode);
	Msg.WriteBytes(PayloadAfterOpcode);

	const TArray<uint8>& Bytes = Msg.GetData();
	const int32 TotalLen = Bytes.Num();
	const uint16 Count = static_cast<uint16>(FMath::Max(1, (TotalLen + MaxFragmentDataSize - 1) / MaxFragmentDataSize));
	const uint32 FragSeq = NextFragmentSequence++;

	for (uint16 Index = 0; Index < Count; ++Index)
	{
		const int32 Offset = Index * MaxFragmentDataSize;
		const int32 DataLen = FMath::Min(MaxFragmentDataSize, TotalLen - Offset);

		FACEBinaryWriter Frag;
		Frag.WriteUInt32(FragSeq);
		Frag.WriteUInt32(0x80000000u);
		Frag.WriteUInt16(Count);
		Frag.WriteUInt16(static_cast<uint16>(FragmentHeaderSize + DataLen));
		Frag.WriteUInt16(Index);
		Frag.WriteUInt16(Queue);
		Frag.WriteBytes(Bytes.GetData() + Offset, DataLen);
		// FlowQueue::EnqueueBlob limits the payload to 0x1D0 (464) bytes.
		// Each full 448-byte fragment therefore needs its own UDP packet.
		SendRawPacket(EACEPacketHeaderFlags::None, {}, {Frag.GetData()}, false, bEncrypted, ClientId);
	}

}

void FACESession::SendGameAction(uint32 ActionType, const TArray<uint8>& ActionPayload, uint16 Queue)
{
	FACEBinaryWriter Inner;
	Inner.WriteUInt32(NextGameActionSequence++);
	Inner.WriteUInt32(ActionType);
	Inner.WriteBytes(ActionPayload);
	SendGameMessage(ACEOpcode::GameAction, Inner.GetData(), Queue, true);
	// These retail requests increment ClientUISystem's busy count until UseDone.
	// Keep this separate from IsUseBusy: casting/shopping must not change action gates.
	if (State==EACESessionState::InWorld && (ActionType==ACEGameAction::Use || ActionType==ACEGameAction::UseWithTarget
		|| ActionType==ACEGameAction::CastTargetedSpell || ActionType==ACEGameAction::CastUntargetedSpell
		|| ActionType==ACEGameAction::Buy || ActionType==ACEGameAction::Sell))
	{
		const double Now=FPlatformTime::Seconds();
		CursorUseWaits.RemoveAll([Now](const auto& Wait){return Wait.Value<=Now;});
		CursorUseWaits.Emplace(ActionType,Now+30.);
	}
}

void FACESession::SendAckIfNeeded()
{
	if (!bNeedAck || !IssacClient)
	{
		return;
	}
	FACEBinaryWriter Body;
	Body.WriteUInt32(LastReceivedPacketSequence);
	SendRawPacket(EACEPacketHeaderFlags::AckSequence | EACEPacketHeaderFlags::EncryptedChecksum,
		Body.GetData(), {}, false, true, ClientId);
	bNeedAck = false;
}

void FACESession::SendEchoResponse(float ClientTime)
{
	FACEBinaryWriter Body;
	Body.WriteFloat(ClientTime);
	Body.WriteFloat(0.f);
	SendRawPacket(EACEPacketHeaderFlags::EchoResponse | EACEPacketHeaderFlags::EncryptedChecksum,
		Body.GetData(), {}, false, true, ClientId);
}

void FACESession::HandleGameMessage(const TArray<uint8>& MessageBytes)
{
	if (MessageBytes.Num() < 4)
	{
		return;
	}
	FACEBinaryReader Reader(MessageBytes);
	const uint32 Opcode = Reader.ReadUInt32();

	switch (Opcode)
	{
	case 0xF643:
		if (PendingCharacterMutation && bRestoringCharacter) HandleCharacterRestored(Reader);
		else HandleCharacterCreated(Reader);
		break;
	case 0xF655:
		// Delete acknowledgement has no roster. Wait for CharacterList.
		break;
	case ACEOpcode::CharacterList:
		HandleCharacterList(Reader);
		break;
	case ACEOpcode::CharacterLogOff:
		// Server ack after FinalizeLogout — CharacterList follows shortly.
		Log(TEXT("CharacterLogOff (server)"));
		break;
	case ACEOpcode::CharacterError:
	{
		if (Reader.Remaining() < 4) break;
		const uint32 ErrorCode = Reader.ReadUInt32();
		if (State == EACESessionState::CharacterSelect && PendingCharacterMutation)
		{
			CharacterManagementError = FString::Printf(TEXT("The server could not %s this character (error %u)."),
				bRestoringCharacter ? TEXT("restore") : TEXT("delete"), ErrorCode);
			PendingCharacterMutation = 0;
			break;
		}
		ConnectionError = ACELoginErrors::Character(ErrorCode);
		Log(FString::Printf(TEXT("CharacterError code=%u - login rejected by server"), ErrorCode));
		if (State == EACESessionState::EnteringWorld && PlayerGuid == 0)
		{
			PendingEnterCharacterId = 0;
			bEnteredWorldSent = false;
			SetState(EACESessionState::CharacterSelect);
			OnCharacterList.Broadcast(Characters, ServerName);
		}
		else
		{
			SetState(EACESessionState::Failed);
		}
		break;
	}
	case ACEOpcode::AccountBoot:
	{
		FString Reason = TEXT("(no reason)");
		if (Reader.Remaining() >= 2)
		{
			Reason = Reader.ReadString16L();
		}
		ConnectionError = FString::Printf(TEXT("Login rejected: %s"), *Reason.TrimStartAndEnd());
		Log(ConnectionError);
		SetState(EACESessionState::Failed);
		break;
	}
	case ACEOpcode::ServerName:
	{
		Reader.ReadUInt32(); // current connections
		Reader.ReadUInt32(); // max connections
		ServerName = Reader.ReadString16L();
		Log(FString::Printf(TEXT("ServerName: %s"), *ServerName));
		break;
	}
	case ACEOpcode::CharacterEnterWorldServerReady:
		// CPlayerSystem::LogOnCharacter waits for fReadyToEnterGame before F657.
		if (State == EACESessionState::EnteringWorld && PendingEnterCharacterId != 0 && !bEnteredWorldSent)
		{
			FACEBinaryWriter Payload;
			Payload.WriteUInt32(static_cast<uint32>(PendingEnterCharacterId));
			Payload.WriteString16L(AccountName);
			bEnteredWorldSent = true;
			SendGameMessage(ACEOpcode::CharacterEnterWorld, Payload.GetData(), ACEQueue::UIQueue, true);
			PendingEnterCharacterId = 0;
		}
		Log(TEXT("CharacterEnterWorldServerReady"));
		break;
	case ACEOpcode::ObjectCreate:
		HandleObjectCreate(Reader);
		break;
	case ACEOpcode::UpdateObject:
		// Same payload as ObjectCreate (SerializeUpdateObject). Required for house hooks
		// morphing into hooked items (Font of Jojii Setup/DefaultScript swap).
		HandleObjectCreate(Reader, true);
		break;
	case ACEOpcode::ObjDescEvent:
		HandleObjDescEvent(Reader);
		break;
	case ACEOpcode::ObjectDelete:
		HandleObjectDelete(Reader);
		break;
	case ACEOpcode::PlayerCreate:
		HandlePlayerCreate(Reader);
		break;
	case ACEOpcode::UpdatePosition:
		HandleUpdatePosition(Reader);
		break;
	case ACEOpcode::PickupEvent:
		HandlePickupEvent(Reader);
		break;
	case ACEOpcode::ParentEvent:
		HandleParentEvent(Reader);
		break;
	case ACEOpcode::UpdateMotion:
		HandleUpdateMotion(Reader);
		break;
	case ACEOpcode::VectorUpdate:
		HandleVectorUpdate(Reader);
		break;
	case ACEOpcode::SetState:
		HandleSetState(Reader);
		break;
	case ACEOpcode::Sound:
		HandleSound(Reader);
		break;
	case ACEOpcode::PlayerTeleport:
		HandlePlayerTeleport(Reader);
		break;
	case ACEOpcode::PlayScriptId:
		HandlePlayScriptId(Reader);
		break;
	case ACEOpcode::PlayEffect:
		HandlePlayEffect(Reader);
		break;
	case ACEOpcode::ServerMessage:
		HandleServerMessage(Reader);
		break;
	case ACEOpcode::PlayerKilled:
	{
		// Retail suppresses the public copy for victim/killer: they receive
		// 01AC/01AD separately. Everyone else sees the server's green death text.
		const FString Message = Reader.ReadString16L();
		if (!Reader.CanRead(8)) break;
		const int32 Victim = Reader.ReadInt32();
		const int32 Killer = Reader.ReadInt32();
		if (!Message.IsEmpty() && PlayerGuid != Victim && PlayerGuid != Killer)
			EmitChatMessage(Message, FString(), ACEChatMessageType::Broadcast);
		break;
	}
	case ACEOpcode::HearSpeech:
		HandleHearSpeech(Reader);
		break;
	case ACEOpcode::HearRangedSpeech:
		HandleHearRangedSpeech(Reader);
		break;
	case ACEOpcode::SoulEmote:
		HandleSoulEmote(Reader);
		break;
	case ACEOpcode::TurbineChat:
		HandleTurbineChat(Reader);
		break;
	case ACEOpcode::GameEvent:
		HandleGameEvent(Reader);
		break;
	case ACEOpcode::PrivateUpdateAttribute:
		HandlePrivateUpdateAttribute(Reader);
		break;
	case ACEOpcode::PrivateUpdateAttributeLevel:
		HandlePrivateUpdateAttributeLevel(Reader);
		break;
	case ACEOpcode::PrivateUpdateVital:
		HandlePrivateUpdateVital(Reader);
		break;
	case ACEOpcode::PrivateUpdateAttribute2ndLevel:
		HandlePrivateUpdateAttribute2ndLevel(Reader);
		break;
	case ACEOpcode::PrivateUpdateSkill:
		HandlePrivateUpdateSkill(Reader);
		break;
	case ACEOpcode::PrivateUpdateSkillLevel:
		HandlePrivateUpdateSkillValue(Reader, false);
		break;
	case ACEOpcode::PrivateUpdateSkillAC:
		HandlePrivateUpdateSkillValue(Reader, true);
		break;
	case ACEOpcode::PrivateUpdatePropertyInt:
		HandlePrivateUpdatePropertyInt(Reader);
		break;
	case ACEOpcode::PrivateRemovePropertyInt:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyInt, false); break;
	case ACEOpcode::PublicRemovePropertyInt:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyInt, true); break;
	case ACEOpcode::PrivateRemovePropertyInt64:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyInt64, false); break;
	case ACEOpcode::PublicRemovePropertyInt64:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyInt64, true); break;
	case ACEOpcode::PrivateRemovePropertyBool:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyBool, false); break;
	case ACEOpcode::PublicRemovePropertyBool:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyBool, true); break;
	case ACEOpcode::PrivateRemovePropertyFloat:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyFloat, false); break;
	case ACEOpcode::PublicRemovePropertyFloat:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyFloat, true); break;
	case ACEOpcode::PrivateRemovePropertyString:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyString, false); break;
	case ACEOpcode::PublicRemovePropertyString:
		HandleRemoveProperty(Reader, ACEOpcode::PrivateUpdatePropertyString, true); break;
	case ACEOpcode::PrivateUpdatePropertyInt64:
		HandlePrivateUpdatePropertyInt64(Reader);
		break;
	case ACEOpcode::PublicUpdatePropertyInt64:
		HandlePrivateUpdatePropertyInt64(Reader, true);
		break;
	case ACEOpcode::PrivateUpdatePropertyFloat:
		HandleUpdatePropertyFloat(Reader, false);
		break;
	case ACEOpcode::PublicUpdatePropertyFloat:
		HandleUpdatePropertyFloat(Reader, true);
		break;
	case ACEOpcode::PrivateUpdatePropertyString:
		HandleUpdatePropertyString(Reader, false);
		break;
	case ACEOpcode::PublicUpdatePropertyString:
		HandleUpdatePropertyString(Reader, true);
		break;
	case ACEOpcode::PublicUpdatePropertyInt:
		HandlePublicUpdatePropertyInt(Reader);
		break;
	case ACEOpcode::PrivateUpdatePropertyBool:
		HandleUpdatePropertyBool(Reader, false);
		break;
	case ACEOpcode::PublicUpdatePropertyBool:
		HandleUpdatePropertyBool(Reader, true);
		break;
	case ACEOpcode::PrivateUpdatePropertyDataID:
		HandlePrivateUpdatePropertyDataID(Reader);
		break;
	case ACEOpcode::PublicUpdatePropertyDataID:
		HandlePublicUpdatePropertyDataID(Reader);
		break;
	case ACEOpcode::PublicUpdateInstanceId:
		HandlePublicUpdateInstanceId(Reader);
		break;
	case ACEOpcode::PrivateUpdatePosition:
		HandlePositionQuality(Reader,false); break;
	case ACEOpcode::PublicUpdatePosition:
		HandlePositionQuality(Reader,true); break;
	case ACEOpcode::PrivateUpdateInstanceId:
		HandlePrivateUpdateInstanceId(Reader);
		break;
	case ACEOpcode::InventoryRemoveObject:
		HandleInventoryRemoveObject(Reader);
		break;
	case ACEOpcode::SetStackSize:
		HandleSetStackSize(Reader);
		break;
	case ACEOpcode::DDD_Interrogation:
		Log(TEXT("DDD_Interrogation received (ignored — disable DAT patching on server for UE clients)"));
		break;
	case ACEOpcode::DDD_EndDDD:
		Log(TEXT("DDD_EndDDD"));
		break;
	default:
		break;
	}
}

void FACESession::HandleCharacterList(FACEBinaryReader& Reader)
{
	const bool bReturningFromWorld =
		bLogOffPending
		|| State == EACESessionState::InWorld
		|| State == EACESessionState::EnteringWorld;

	Reader.ReadUInt32(); // 0
	const uint32 Count = Reader.ReadUInt32();
	Characters.Reset();
	for (uint32 i = 0; i < Count; ++i)
	{
		FACECharacterInfo Info;
		Info.CharacterId = static_cast<int32>(Reader.ReadUInt32());
		Info.Name = Reader.ReadString16L();
		Info.DeleteSeconds = static_cast<int32>(Reader.ReadUInt32());
		Characters.Add(Info);
	}
	Reader.ReadUInt32(); // 0
	CharacterSlotCount = FMath::Clamp(static_cast<int32>(Reader.ReadUInt32()), 1, 100);
	PendingCharacterMutation = 0;
	CharacterManagementError.Reset();
	bCharacterCreationPending = false;
	AccountName = Reader.ReadString16L();

	if (bReturningFromWorld)
	{
		ClearWorldState();
		bLogOffPending = false;
		LogOffPortalStartTime = 0.;
		LogOffTimeout = 0.f;
		LogOffRetransmitTimer = 0.f;
		Log(TEXT("Logged off — returned to character select"));
	}

	SetState(EACESessionState::CharacterSelect);
	Log(FString::Printf(TEXT("Character list: %d character(s)"), Characters.Num()));
	OnCharacterList.Broadcast(Characters, ServerName);
}

void FACESession::HandlePlayerCreate(FACEBinaryReader& Reader)
{
	PlayerGuid = static_cast<int32>(Reader.ReadUInt32());
	Log(FString::Printf(TEXT("PlayerCreate guid=0x%08X"), PlayerGuid));
	ApplyPlayerInventoryProfile();
	if (FACEWorldObject* Existing = WorldObjects.Find(PlayerGuid))
	{
		Existing->bIsSelf = true;
		Existing->bIsPlayer = true;
		if (ApplyPlayerInventoryCapacity(*Existing)) ++InventoryDataRevision;
		AdoptPlayerPhysicsTimestamps(*Existing);
	}
	if (!bLoginCompleteSent)
	{
		SendLoginComplete();
	}
	MaybeEnterWorldComplete();
}

void FACESession::UpsertWorldObject(const FACEWorldObject& Object, bool bForceRecreate)
{
	// A fresh server description re-establishes interest in this incarnation.
	ObjectVisibilityDeadlines.Remove(Object.Guid);
	FACEWorldObject Merged = Object;
	const FACEWorldObject* Existing = WorldObjects.Find(Object.Guid);
	const bool bWasInventory = Existing && (HasPlayerInventoryState(*Existing)
		|| (PlayerGuid && Existing->ParentGuid == PlayerGuid));
	const bool bSameInstance = Existing && (Existing->bPhysicsDescriptionPending
		|| !Object.bHasPhysicsTimestamps || !Existing->bHasPhysicsTimestamps
		|| Object.PhysicsTimestamps[ACEPhysicsTimeStamp::Instance] == Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance]);
	if (Existing && !bSameInstance) PropertyStamps.Remove(Object.Guid);
	if (!bSameInstance || bForceRecreate) EquipmentRemovalEvidence.Remove(Object.Guid);
	if (Existing && bSameInstance && !bForceRecreate)
	{
		Merged.ReceivedActionStamp=Existing->ReceivedActionStamp;
		Merged.bHasReceivedActionStamp=Existing->bHasReceivedActionStamp;
		// CreateObject (SmartboxQueue) and ContainId (UIQueue) can reorder. If ContainId
		// homed an item first, a later CreateObject that omits Container must not wipe it
		// — that dropped newly created salvage bags out of GetPackItems.
		if (!Merged.bHasPosition && Merged.ContainerId == 0 && Existing->ContainerId != 0
			&& Merged.ParentGuid == 0 && Merged.WielderId == 0
			&& Merged.CurrentWieldedLocation == 0)
		{
			Merged.ContainerId = Existing->ContainerId;
		}
		// A repeated description of the same dead creature is not a respawn.
		// A server may replace a defeated monster with a quest NPC while keeping
		// its GUID/instance. Preserve death only for the same weenie and role;
		// otherwise the replacement inherits unselectable, non-colliding state.
		const bool bSameWeenieRole = Merged.WeenieClassId == Existing->WeenieClassId
			&& Merged.ItemType == Existing->ItemType
			&& ((Merged.ObjectDescriptionFlags ^ Existing->ObjectDescriptionFlags)
				& (ACEObjectDescFlag::Attackable | ACEObjectDescFlag::Vendor | ACEObjectDescFlag::Corpse)) == 0;
		if (bSameWeenieRole)
		{
			if (Merged.ItemCurrentMana == -1) Merged.ItemCurrentMana = Existing->ItemCurrentMana;
			if (Merged.ItemMaximumMana == -1) Merged.ItemMaximumMana = Existing->ItemMaximumMana;
		}
		if (Existing->bDying && !Merged.bIsPlayer && bSameInstance && bSameWeenieRole && !Merged.AllowsDeadPoseInteraction())
		{
			Merged.bDying = true;
			Merged.InitialMotionCommand = ACEMotion::Dead;
			Merged.InitialMotionStyle = ACEMotion::StanceNonCombat;
		}
		if (Merged.PlacementPosition < 0 && Existing->PlacementPosition >= 0)
		{
			Merged.PlacementPosition = Existing->PlacementPosition;
		}
        // An equipment profile/wield notification can precede the mesh description.
        if (!Merged.bHasPosition && !Merged.ContainerId && !Merged.CurrentWieldedLocation
            && Existing->WielderId && Existing->CurrentWieldedLocation
            && (!Merged.WielderId || Merged.WielderId == Existing->WielderId)
            && (!Merged.ParentGuid || Merged.ParentGuid == Existing->WielderId))
        {
            Merged.WielderId = Existing->WielderId;
            Merged.CurrentWieldedLocation = Existing->CurrentWieldedLocation;
            Merged.ClothingPriority = Existing->ClothingPriority;
        }
		if (Merged.MaterialType == 0 && Existing->MaterialType != 0)
		{
			Merged.MaterialType = Existing->MaterialType;
		}
		if (Merged.IconId == 0 && Existing->IconId != 0)
		{
			Merged.IconId = Existing->IconId;
		}
		if (Merged.IconUnderlayId == 0 && Existing->IconUnderlayId != 0)
		{
			Merged.IconUnderlayId = Existing->IconUnderlayId;
		}
		if (Merged.SpellDID == 0 && Existing->SpellDID != 0)
		{
			Merged.SpellDID = Existing->SpellDID;
		}
		if (Merged.IconOverlayId == 0 && Existing->IconOverlayId != 0)
		{
			Merged.IconOverlayId = Existing->IconOverlayId;
		}
		if (Merged.UiEffects == 0 && Existing->UiEffects != 0)
		{
			Merged.UiEffects = Existing->UiEffects;
		}
		if (Merged.Name.IsEmpty() && !Existing->Name.IsEmpty())
		{
			Merged.Name = Existing->Name;
		}
		if (Merged.PluralName.IsEmpty()) Merged.PluralName=Existing->PluralName;
	}
	// ViewContents and CreateObject use separate ordered queues. The list can arrive
	// before an item (or its containing bag); recover membership before publishing
	// that item to inventory/appraisal consumers, including descriptions without Container.
	if (!Merged.ContainerId && !Merged.WielderId && !Merged.CurrentWieldedLocation && !Merged.bHasPosition)
	{
		for (const auto& Contents : ContainerContents)
		{
			if (Contents.Value.ContainsByPredicate([&](const FACEContainerItemRef& Ref) { return Ref.ItemGuid == Merged.Guid; }))
			{
				Merged.ContainerId = Contents.Key;
				break;
			}
		}
	}
	// CreateObject has no PropertyInt.PlacementPosition. Retail fills the pack from
	// PlayerDescription ContentProfile order (ACE OrderBy PlacementPosition, densified
	// 0..n-1 on load). Stamp that index when ObjectCreate arrives after the footer.
	if (Merged.PlacementPosition < 0 && PlayerGuid != 0)
	{
		if (const TArray<FACEContainerItemRef>* Contents = ContainerContents.Find(Merged.ContainerId))
		{
			int32 MainI = 0;
			int32 PackI = 0;
			for (const FACEContainerItemRef& Ref : *Contents)
			{
				if (Ref.ItemGuid == Merged.Guid)
				{
					Merged.PlacementPosition = (Ref.ContainerType == 0) ? MainI : PackI;
					break;
				}
				if (Ref.ContainerType == 0)
				{
					++MainI;
				}
				else
				{
					++PackI;
				}
			}
		}
	}
	ApplyPlayerInventoryCapacity(Merged);
	WorldObjects.Add(Merged.Guid, Merged);
	// CreateObject can complete after ContainId, or arrive without another
	// inventory receipt. Refresh cached rows on arrival/replacement as well as
	// movement; an appraisal-only refresh cannot insert a previously absent row.
	if (bWasInventory || HasPlayerInventoryState(Merged)
		|| (PlayerGuid && Merged.ParentGuid == PlayerGuid)) ++InventoryDataRevision;
	QueueReadyGameEvents(Merged.Guid);
	if (Merged.Guid == PlayerGuid) AdoptPlayerPhysicsTimestamps(Merged);
	ReconcileOpenContainerOwnership();
	OnObjectCreated.Broadcast(Merged);
}

void FACESession::AdoptPlayerPhysicsTimestamps(const FACEWorldObject& Object)
{
	if (!Object.bHasPhysicsTimestamps || Object.bPhysicsDescriptionPending) return;
	InstanceSeq = Object.PhysicsTimestamps[ACEPhysicsTimeStamp::Instance];
	ServerControlSeq = Object.PhysicsTimestamps[ACEPhysicsTimeStamp::ServerControl];
	TeleportSeq = Object.PhysicsTimestamps[ACEPhysicsTimeStamp::Teleport];
	ForcePositionSeq = Object.PhysicsTimestamps[ACEPhysicsTimeStamp::ForcePosition];
}

void FACESession::MaybeEnterWorldComplete()
{
	if (State != EACESessionState::EnteringWorld || PlayerGuid == 0 || !PlayerPosition.IsValid())
	{
		return;
	}
	SetState(EACESessionState::InWorld);
	// Avatar playback is shared by desktop and VR. Negotiate once per world
	// entry even when no local headset/VR component exists.
	RequestVRCapabilities(false);

	{
		constexpr float WorldScale = 100.f;
		const FVector Ue = PlayerPosition.ToUnrealLocation(WorldScale);
		Log(FString::Printf(
			TEXT("EnterWorld place cell=0x%08X local=(%.2f,%.2f,%.2f) unreal=(%.0f,%.0f,%.0f) heading=%.1f°"),
			PlayerPosition.CellId,
			PlayerPosition.Location.X, PlayerPosition.Location.Y, PlayerPosition.Location.Z,
			Ue.X, Ue.Y, Ue.Z,
			PlayerPosition.GetAceHeadingDegrees()));
	}

	OnEnteredWorld.Broadcast(PlayerGuid, PlayerPosition);
}

void FACESession::HandleObjectCreate(FACEBinaryReader& Reader, bool bForceRecreate)
{
	FACEDecodedObject Decoded;
	if (!FACEObjectCreateParser::Parse(Reader, Decoded) || !Decoded.bParseOk)
	{
		Log(FString::Printf(TEXT("ObjectCreate parse failed guid=0x%08X isSelf=%d"),
			Decoded.Guid, (PlayerGuid != 0 && Decoded.Guid == PlayerGuid) ? 1 : 0));
		return;
	}

	const FACEWorldObject* Existing = WorldObjects.Find(Decoded.Guid);
	const bool bComparableInstance = Existing && !Existing->bPhysicsDescriptionPending
		&& Existing->bHasPhysicsTimestamps && Decoded.bHasPhysicsTimestamps;
	const bool bSameInstance = bComparableInstance
		&& Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance] == Decoded.PhysicsTimestamps[ACEPhysicsTimeStamp::Instance];
	if (!bForceRecreate && bComparableInstance && !bSameInstance
		&& !ACEPhysicsTimeStamp::IsNewer(Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance],
			Decoded.PhysicsTimestamps[ACEPhysicsTimeStamp::Instance]))
	{
		return; // SmartBox::HandleCreateObject ignores descriptions of an older incarnation.
	}

	FACEWorldObject Obj;
	Obj.Guid = Decoded.Guid;
	Obj.Name = Decoded.Name;
	Obj.CreateFlags1 = Decoded.CreateFlags1; Obj.CreateFlags2 = Decoded.CreateFlags2; Obj.PhysicsDataFlags = Decoded.PhysicsDataFlags;
	Obj.PublicPhysicsScript = Decoded.PublicPhysicsScript;
	Obj.PluralName = Decoded.PluralName;
	Obj.WeenieClassId = Decoded.WeenieClassId;
	Obj.IconId = Decoded.IconId;
	Obj.IconOverlayId = Decoded.IconOverlayId;
	Obj.IconUnderlayId = Decoded.IconUnderlayId;
	Obj.CooldownId = Decoded.CooldownId;
	Obj.CooldownDuration = Decoded.CooldownDuration;
	Obj.UiEffects = static_cast<int32>(Decoded.UiEffects);
	Obj.ContainerId = Decoded.ContainerId;
	Obj.WielderId = Decoded.WielderId;
	Obj.MonarchGuid = Decoded.MonarchGuid;
	Obj.Value = Decoded.Value;
	Obj.Burden = Decoded.Burden;
	Obj.ContainersCapacity = Decoded.ContainersCapacity;
	Obj.ItemsCapacity = Decoded.ItemsCapacity;
	if (PlayerGuid != 0 && Decoded.Guid == PlayerGuid && Decoded.Burden > 0)
	{
		// ObjectCreate Burden on the player weenie is EncumbranceVal until PrivateUpdate arrives.
		bHasPlayerEncumbrance = true;
		PlayerEncumbranceVal = Decoded.Burden;
	}
	Obj.StackSize = Decoded.StackSize;
	Obj.MaxStackSize = Decoded.MaxStackSize;
	Obj.Structure = Decoded.Structure;
	Obj.MaxStructure = Decoded.MaxStructure;
	Obj.SpellDID = Decoded.SpellDID;
	Obj.ItemUseable = Decoded.ItemUseable;
	Obj.TargetType = Decoded.TargetType;
	Obj.MaterialType = Decoded.MaterialType;
	Obj.SalvageWorkmanship = Decoded.SalvageWorkmanship;
	if (Obj.MaxStackSize > 0 && Obj.StackSize > Obj.MaxStackSize)
	{
		Obj.StackSize = Obj.MaxStackSize;
	}
	Obj.SetupId = Decoded.SetupId;
	Obj.MotionTableId = Decoded.MotionTableId;
	Obj.SoundTableId = Decoded.SoundTableId;
	Obj.PhysicsEffectTableId = Decoded.PhysicsEffectTableId;
	Obj.ItemType = Decoded.ItemType;
	Obj.bIsHouseHook = Decoded.bIsHouseHook;
	Obj.HookItemTypes = Decoded.HookItemTypes;
	Obj.HookType = Decoded.HookType;
	Obj.HouseOwnerId = Decoded.HouseOwnerId;
	Obj.Scale = Decoded.Scale > 0.f ? Decoded.Scale : 1.f;
	Obj.Friction = FMath::IsFinite(Decoded.Friction) ? FMath::Clamp(Decoded.Friction,0.f,1.f) : .95f;
	Obj.Elasticity = Decoded.Elasticity;
	Obj.Elasticity = Obj.GetValidElasticity();
	Obj.Position = Decoded.Position;
	Obj.bHasPosition = Decoded.bHasPosition;
	Obj.bIsPlayer = Decoded.bIsPlayer;
	Obj.bIsSelf = (PlayerGuid != 0 && Decoded.Guid == PlayerGuid);
	if (Obj.bIsSelf && (Obj.ObjectDescriptionFlags & ACEObjectDescFlag::Admin) != 0)
	{
		bLocalPlayerIsAdmin = true;
	}
	Obj.ParentGuid = Decoded.ParentGuid;
	Obj.ParentLocation = Decoded.ParentLocation;
	Obj.PlacementId = Decoded.PlacementId;
	Obj.bHasServerPhysicsDescription = Decoded.bHasPhysicsTimestamps;
	Obj.ClothingPriority = Decoded.ClothingPriority;
	Obj.CurrentWieldedLocation = static_cast<int64>(Decoded.CurrentWieldedLocation);
	Obj.ValidLocations = static_cast<int64>(Decoded.ValidLocations);
	Obj.ObjectDescriptionFlags = Decoded.ObjectDescriptionFlags;
	Obj.PetOwnerId = Decoded.PetOwnerId;
	Obj.PhysicsState = Decoded.PhysicsState;
	Obj.UseRadius = Decoded.UseRadius;
	Obj.AmmoType = Decoded.AmmoType;
	Obj.CombatUse = Decoded.CombatUse;
	Obj.RadarBlipColor = Decoded.RadarBlipColor;
	Obj.RadarBehavior = Decoded.RadarBehavior;
	Obj.Translucency = Decoded.Translucency;
	Obj.Omega = Decoded.Omega;
	Obj.Velocity = Decoded.Velocity;
	Obj.bHasVelocity = Decoded.bHasVelocity;
	Obj.DefaultScriptId = Decoded.DefaultScriptId;
	Obj.DefaultScriptIntensity = Decoded.DefaultScriptIntensity;
	Obj.InitialMotionCommand = Decoded.InitialMotionCommand;
	Obj.InitialMotionStyle = Decoded.InitialMotionStyle;
	Obj.bDying = !Obj.IsCorpse() && !Obj.AllowsDeadPoseInteraction() && ACEMotion::NormalizeCommand(Obj.InitialMotionCommand) == ACEMotion::Dead;
	Obj.Appearance = Decoded.Appearance;
	if (Decoded.bHasPhysicsTimestamps)
	{
		FMemory::Memcpy(Obj.PhysicsTimestamps, Decoded.PhysicsTimestamps, sizeof(Obj.PhysicsTimestamps));
		Obj.bHasPhysicsTimestamps = true;
	}

	if (bSameInstance && !bForceRecreate)
	{
		// A repeated CreateObject is a collection of timestamped physics updates,
		// not a replacement. Only UpdateObject (house hooks, etc.) rebuilds the setup.
		Obj.SetupId = Existing->SetupId;
		Obj.MotionTableId = Existing->MotionTableId;
		Obj.SoundTableId = Existing->SoundTableId;
		Obj.PhysicsEffectTableId = Existing->PhysicsEffectTableId;
		Obj.Scale = Existing->Scale;
		Obj.Friction = Existing->Friction;
		Obj.Elasticity = Existing->Elasticity;
		Obj.Translucency = Existing->Translucency;
		Obj.DefaultScriptId = Existing->DefaultScriptId;
		Obj.DefaultScriptIntensity = Existing->DefaultScriptIntensity;
		Obj.DefaultAnimationId = Existing->DefaultAnimationId;
		const auto IsNewer = [&](int32 Index)
		{
			return ACEPhysicsTimeStamp::IsNewer(Existing->PhysicsTimestamps[Index], Obj.PhysicsTimestamps[Index]);
		};
		if (!IsNewer(ACEPhysicsTimeStamp::ObjDesc)) Obj.Appearance = Existing->Appearance;
		if (!IsNewer(ACEPhysicsTimeStamp::State)) Obj.PhysicsState = Existing->PhysicsState;
		if (!IsNewer(ACEPhysicsTimeStamp::Vector))
		{
			Obj.Velocity = Existing->Velocity;
			Obj.bHasVelocity = Existing->bHasVelocity;
			Obj.Omega = Existing->Omega;
		}
		const bool bSameWeenieRole = Obj.WeenieClassId == Existing->WeenieClassId && Obj.ItemType == Existing->ItemType
			&& ((Obj.ObjectDescriptionFlags ^ Existing->ObjectDescriptionFlags)
				& (ACEObjectDescFlag::Attackable | ACEObjectDescFlag::Vendor | ACEObjectDescFlag::Corpse)) == 0;
		if (bSameWeenieRole && !IsNewer(ACEPhysicsTimeStamp::Movement) && !IsNewer(ACEPhysicsTimeStamp::ServerControl))
		{
			Obj.InitialMotionCommand = Existing->InitialMotionCommand;
			Obj.InitialMotionStyle = Existing->InitialMotionStyle;
			Obj.bDying = Existing->bDying;
		}
		const bool bStaleTeleport = Obj.bHasPosition && ACEPhysicsTimeStamp::IsNewer(
			Obj.PhysicsTimestamps[ACEPhysicsTimeStamp::Teleport], Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::Teleport]);
		if (bStaleTeleport || (!IsNewer(ACEPhysicsTimeStamp::Position)
			&& !(Obj.bIsSelf && IsNewer(ACEPhysicsTimeStamp::ForcePosition))))
		{
			Obj.Position = Existing->Position;
			Obj.bHasPosition = Existing->bHasPosition;
			Obj.ParentGuid = Existing->ParentGuid;
			Obj.ParentLocation = Existing->ParentLocation;
			Obj.PlacementId = Existing->PlacementId;
			Obj.PhysicsTimestamps[ACEPhysicsTimeStamp::Position] = Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::Position];
			Obj.PhysicsTimestamps[ACEPhysicsTimeStamp::Teleport] = Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::Teleport];
			Obj.PhysicsTimestamps[ACEPhysicsTimeStamp::ForcePosition] = Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::ForcePosition];
		}
		for (int32 Index = 0; Index < ACEPhysicsTimeStamp::Instance; ++Index)
		{
			if (!IsNewer(Index)) Obj.PhysicsTimestamps[Index] = Existing->PhysicsTimestamps[Index];
		}
	}
	if (Existing && (bForceRecreate || (bComparableInstance && !bSameInstance)))
	{
		// Retail destroys the prior physics object before publishing the replacement.
		VRPoses.Remove(Obj.Guid);
		OnObjectDeleted.Broadcast(Obj.Guid);
	}
	if (Obj.bIsSelf && Obj.bHasPosition)
	{
		PlayerPosition = Obj.Position;
	}

	UpsertWorldObject(Obj, bForceRecreate);
	ReplayObjectPhysicsEvents(Obj.Guid);
	Log(FString::Printf(TEXT("ObjectCreate '%s' guid=0x%08X setup=0x%08X parent=0x%08X loc=%d animParts=%d wcid=%d type=0x%08X desc=0x%08X physics=0x%08X placement=%d motionTable=0x%08X motion=0x%08X style=0x%08X usable=0x%08X"),
		*Obj.Name, Obj.Guid, Obj.SetupId, Obj.ParentGuid, Obj.ParentLocation,
		Obj.Appearance.AnimPartChanges.Num(), Obj.WeenieClassId, Obj.ItemType, Obj.ObjectDescriptionFlags,
		Obj.PhysicsState, Obj.PlacementId, Obj.MotionTableId, Obj.InitialMotionCommand, Obj.InitialMotionStyle, Obj.ItemUseable));
	MaybeEnterWorldComplete();
}

void FACESession::ExpireObjectPhysicsEvents(double Now)
{
	if (Now >= LastObjectPhysicsExpiryAt && Now - LastObjectPhysicsExpiryAt < 1.) return;
	LastObjectPhysicsExpiryAt = Now;
	for (auto It = PendingObjectPhysicsEvents.CreateIterator(); It; ++It)
	{
		It.Value().RemoveAll([this, Now](const FPendingObjectPhysicsEvent& Event)
		{
			if (Now - Event.QueuedAt <= 25.) return false;
			PendingObjectPhysicsBytes -= Event.Payload.Num();
			return true;
		});
		if (It.Value().IsEmpty()) It.RemoveCurrent();
	}
}

bool FACESession::DeferObjectPhysicsEvent(int32 Guid, uint16 Instance, uint32 Opcode,
	const FACEBinaryReader& Reader, int32 PayloadStart)
{
	if (const FACEWorldObject* Existing = WorldObjects.Find(Guid); Existing && !Existing->bPhysicsDescriptionPending)
	{
		if (!Existing->bHasPhysicsTimestamps || Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance] == Instance)
			return false;
		if (!ACEPhysicsTimeStamp::IsNewer(Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance], Instance))
			return true; // An older incarnation cannot change this object, regardless of its category sequence.
	}
	// SmartBox queues packets that precede their object's CreateObject. Keep their
	// original ordering, bound abandoned GUIDs, and expire them with retail's lost-object grace.
	const double Now = FPlatformTime::Seconds();
	ExpireObjectPhysicsEvents(Now);
	const int32 PayloadSize = Reader.GetLength() - PayloadStart;
	if (PayloadStart < 0 || PayloadSize <= 0 || PayloadSize > 65536
		|| PendingObjectPhysicsBytes + PayloadSize > 4 * 1024 * 1024
		|| (PendingObjectPhysicsEvents.Num() >= 1024 && !PendingObjectPhysicsEvents.Contains(Guid))) return true;
	TArray<FPendingObjectPhysicsEvent>& Pending = PendingObjectPhysicsEvents.FindOrAdd(Guid);
	if (Pending.Num() >= 64) return true;
	FPendingObjectPhysicsEvent& Event = Pending.AddDefaulted_GetRef();
	Event.Opcode = Opcode;
	Event.QueuedAt = Now;
	Event.Payload.Append(Reader.GetData() + PayloadStart, PayloadSize);
	PendingObjectPhysicsBytes += PayloadSize;
	return true;
}

void FACESession::ReplayObjectPhysicsEvents(int32 Guid)
{
	TArray<FPendingObjectPhysicsEvent> Pending;
	if (!PendingObjectPhysicsEvents.RemoveAndCopyValue(Guid, Pending)) return;
	for (const FPendingObjectPhysicsEvent& Event : Pending) PendingObjectPhysicsBytes -= Event.Payload.Num();
	const double Now = FPlatformTime::Seconds();
	for (const FPendingObjectPhysicsEvent& Event : Pending)
	{
		if (Now - Event.QueuedAt > 25.) continue;
		FACEBinaryReader Reader(Event.Payload);
		switch (Event.Opcode)
		{
		case ACEOpcode::ObjDescEvent: HandleObjDescEvent(Reader); break;
		case ACEOpcode::ObjectDelete: HandleObjectDelete(Reader); break;
		case ACEOpcode::SetState: HandleSetState(Reader); break;
		case ACEOpcode::ParentEvent: HandleParentEvent(Reader); break;
		case ACEOpcode::PickupEvent: HandlePickupEvent(Reader); break;
		case ACEOpcode::UpdatePosition: HandleUpdatePosition(Reader); break;
		case ACEOpcode::UpdateMotion: HandleUpdateMotion(Reader); break;
		case ACEOpcode::VectorUpdate: HandleVectorUpdate(Reader); break;
		default: break;
		}
	}
}

void FACESession::HandleObjDescEvent(FACEBinaryReader& Reader)
{
	const int32 PayloadStart = Reader.Tell();
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	FACEObjDesc Appearance;
	if (!FACEObjectCreateParser::ParseModelData(Reader, Appearance))
	{
		Log(TEXT("ObjDescEvent parse failed"));
		return;
	}
	if (!Reader.CanRead(4)) return;
	const uint16 IncomingInstance = Reader.ReadUInt16();
	const uint16 IncomingVisual = Reader.ReadUInt16();
	if (Reader.CanRead(4))
	{
		Reader.ReadUInt32(); // optional pad / unused
	}

	if (DeferObjectPhysicsEvent(Guid, IncomingInstance, ACEOpcode::ObjDescEvent, Reader, PayloadStart)) return;
	FACEWorldObject* Existing = WorldObjects.Find(Guid);
	if (Existing->bHasPhysicsTimestamps
		&& !ACEPhysicsTimeStamp::IsNewer(Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::ObjDesc], IncomingVisual)) return;
	Existing->PhysicsTimestamps[ACEPhysicsTimeStamp::ObjDesc] = IncomingVisual;

	Existing->Appearance = Appearance;
	Existing->bIsSelf = (PlayerGuid != 0 && Guid == PlayerGuid);
	FACEWorldObject VisualUpdate = *Existing; VisualUpdate.bAppearanceOnlyUpdate = true;
	OnObjectCreated.Broadcast(VisualUpdate);
	Log(FString::Printf(TEXT("ObjDescEvent guid=0x%08X animParts=%d"), Guid, Appearance.AnimPartChanges.Num()));
}

void FACESession::HandleObjectDelete(FACEBinaryReader& Reader)
{
	const int32 PayloadStart = Reader.Tell();
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	if (Guid == PlayerGuid && PlayerGuid != 0) return;
	if (Reader.CanRead(2))
	{
		const uint16 IncomingInstance = Reader.ReadUInt16();
		if (DeferObjectPhysicsEvent(Guid, IncomingInstance, ACEOpcode::ObjectDelete, Reader, PayloadStart)) return;
	}
	if (!UpdateEquipmentRemovalEvidence(Guid, WorldDeleted)) DeleteWorldObject(Guid);
}

void FACESession::DeleteWorldObject(int32 Guid)
{
	ObjectVisibilityDeadlines.Remove(Guid);
	ClosedContainerItemDeadlines.Remove(Guid);
	// ACE removes completed trade offers with DeleteObject (not InventoryRemove).
	// PickupEvent/attachment updates still preserve owned items in their own path.
	if (TradeSelfItems.Contains(Guid))
	{
		TradeRemovedItems.Add(Guid);
		RemoveInventoryObject(Guid);
		return;
	}
	// ObjectDelete = leave the 3D world. Keep WorldObjects only for items the player still
	// owns (main pack / side pack / wielded). Do NOT keep corpse/chest ViewContents rows or
	// arbitrary ContainerId — that hid lifestones/chests after false inventory parses.
	bool bKeepInventoryRecord = false;
	if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
	{
		bool bInPlayerPack = (PlayerGuid != 0 && Obj->ContainerId == PlayerGuid);
		if (!bInPlayerPack && Obj->ContainerId != 0 && PlayerGuid != 0)
		{
			if (const TArray<FACEContainerItemRef>* Root = ContainerContents.Find(PlayerGuid))
			{
				for (const FACEContainerItemRef& Ref : *Root)
				{
					if (Ref.ContainerType != 0 && Ref.ItemGuid == Obj->ContainerId)
					{
						bInPlayerPack = true;
						break;
					}
				}
			}
			if (!bInPlayerPack)
			{
				if (const FACEWorldObject* Pack = WorldObjects.Find(Obj->ContainerId))
				{
					bInPlayerPack = Pack->ContainerId == PlayerGuid
						&& (Pack->ItemsCapacity > 0 || Pack->ContainersCapacity > 0
							|| (Pack->ItemType & ACEItemType::Container) != 0);
				}
			}
		}
		if (bInPlayerPack || (PlayerGuid != 0 && Obj->WielderId == PlayerGuid)
			|| (PlayerGuid != 0 && Obj->ParentGuid == PlayerGuid))
		{
			bKeepInventoryRecord = true;
			// Inventory retains the weenie snapshot, not the deleted CPhysicsObj.
			// A later full Create must rebuild its setup even if the instance is unchanged.
			Obj->bPhysicsDescriptionPending = true;
			Obj->bHasPhysicsTimestamps = false;
			Obj->bHasReceivedActionStamp = false;
			FMemory::Memzero(Obj->PhysicsTimestamps, sizeof(Obj->PhysicsTimestamps));
			Obj->bDying = false;
			Obj->bHasPosition = false;
			Obj->ParentGuid = 0;
			Obj->ParentLocation = 0;
		}
	}
	if (!bKeepInventoryRecord)
	{
		EquipmentRemovalEvidence.Remove(Guid);
		if (GiveRecipientGuid == Guid) GiveRecipientGuid = 0;
		if (OpenExternalContainerGuid == Guid)
		{
			OpenExternalContainerGuid = 0;
			OnCloseGroundContainer.Broadcast(Guid);
			ClearContainerContents(Guid);
		}
		if (SelectedObject.Guid == Guid) SelectObject(0);
		VRPoses.Remove(Guid);
		WorldObjects.Remove(Guid);
		PropertyStamps.Remove(Guid);
		RemoveFromContainerLists(Guid);
	}
	OnObjectDeleted.Broadcast(Guid);
}

void FACESession::MaintainWorldObjectVisibility(double Now, TFunctionRef<bool(int32)> IsCellInPVS)
{
	ExpireObjectPhysicsEvents(Now);
	ExpireClosedContainerItems(Now);
	if (PlayerGuid == 0) return;
	// Retail CPhysicsObj::prepare_to_leave_visibility and CObjectMaint::UseTime
	// retain lost objects for 25 seconds. Servers then forget the observer without
	// sending ObjectDelete. Keeping the descriptor forever resurrects expired
	// summoned portals (and other removed objects) when terrain streams back in.
	constexpr double DestructionTime = 25.0;
	TSet<int32> Expired;
	for (const auto& Pair : WorldObjects)
	{
		const FACEWorldObject& Object = Pair.Value;
		if (Object.Guid == PlayerGuid || Object.bIsSelf || !Object.bHasPosition
			|| !Object.Position.IsValid() || Object.ContainerId || Object.ParentGuid || Object.WielderId)
		{
			ObjectVisibilityDeadlines.Remove(Pair.Key);
			continue;
		}
		// Expire before considering re-entry: a late return requires a fresh
		// ObjectCreate, not the descriptor from an earlier visit.
		if (const double* Deadline = ObjectVisibilityDeadlines.Find(Pair.Key); Deadline && Now >= *Deadline)
			Expired.Add(Pair.Key);
		else if (IsCellInPVS(Object.Position.CellId))
			ObjectVisibilityDeadlines.Remove(Pair.Key);
		else if (!ObjectVisibilityDeadlines.Contains(Pair.Key))
			ObjectVisibilityDeadlines.Add(Pair.Key, Now + DestructionTime);
	}
	// Wielded visuals follow their remote owner, just as retail's child list does.
	if (Expired.IsEmpty()) return;
	TArray<int32> Children;
	for (const auto& Pair : WorldObjects)
		if (Expired.Contains(Pair.Value.ParentGuid) || Expired.Contains(Pair.Value.WielderId))
			Children.Add(Pair.Key);
	for (int32 Guid : Children) Expired.Add(Guid);
	for (int32 Guid : Expired) DeleteWorldObject(Guid);
}

void FACESession::HandleSound(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(12))
	{
		return;
	}
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const int32 SoundType = static_cast<int32>(Reader.ReadUInt32());
	const float Volume = Reader.ReadFloat();
	OnSound.Broadcast(Guid, SoundType, Volume);
}

void FACESession::HandlePlayerTeleport(FACEBinaryReader& Reader)
{
	// GameMessagePlayerTeleport: ObjectTeleport sequence + align.
	// Destination UpdatePosition (0xF748) carries the TeleportSeq the controller watches;
	// this message is the retail "enter portal space" cue that accompanies it.
	if (!Reader.CanRead(2))
	{
		return;
	}
	const uint16 ObjectTeleportSeq = Reader.ReadUInt16();
	Reader.Align();
	Log(FString::Printf(TEXT("PlayerTeleport seq=%u"), ObjectTeleportSeq));
	bPositionReportingSuspended = true;
	bHaveReportedPosition = false;
	// A pending interaction in the old location cannot survive portal entry.
	CancelPendingUse(TEXT("portal entry"));
	SelectObject(0);
	if (OpenExternalContainerGuid != 0)
	{
		// Recall/portal entry revokes the old interaction immediately; a matching
		// CloseGroundContainer packet is not guaranteed before the new world loads.
		const int32 OldContainer = OpenExternalContainerGuid;
		OpenExternalContainerGuid = 0;
		OnCloseGroundContainer.Broadcast(OldContainer);
		ClearContainerContents(OldContainer);
	}
	// Retail SmartBox::HandlePlayerTeleport enters portal space on this message —
	// before the destination UpdatePosition arrives. Mirror that so the tunnel is up
	// instantly and the old world never flashes.
	OnPlayerTeleportStarted.Broadcast();
}

void FACESession::HandlePlayScriptId(FACEBinaryReader& Reader)
{
	// Explicit PhysicsScript DID, rather than the PlayScript enum used by PlayEffect.
	if (!Reader.CanRead(12))
	{
		return;
	}
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const int32 PhysicsScriptId = static_cast<int32>(Reader.ReadUInt32());
	const float Intensity = Reader.ReadFloat();
	OnPlayScriptId.Broadcast(Guid, PhysicsScriptId, Intensity);
}

void FACESession::HandlePlayEffect(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(12))
	{
		return;
	}
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const int32 ScriptType = static_cast<int32>(Reader.ReadUInt32());
	const float Intensity = Reader.ReadFloat();
	OnPlayEffect.Broadcast(Guid, ScriptType, Intensity);
}

void FACESession::EmitChatMessage(const FString& Text, const FString& Sender, int32 Type, int32 FilterType)
{
	// Keep protocol categories intact for UCM/metas and third-party chat observers.
	OnChatMessage.Broadcast(Text, Sender, Type);
	OnRoutedChatMessage.Broadcast(Text, Sender, Type, FilterType == INDEX_NONE ? Type : FilterType);
}

void FACESession::HandleServerMessage(FACEBinaryReader& Reader)
{
	const FString Text = Reader.ReadString16L();
	const int32 Type = Reader.CanRead(4) ? Reader.ReadInt32() : 0;
	EmitChatMessage(Text, TEXT(""), Type);
}

void FACESession::HandleHearSpeech(FACEBinaryReader& Reader)
{
	const FString Text = Reader.ReadString16L();
	FString Sender = Reader.ReadString16L();
	const int32 SenderGuid = Reader.CanRead(4) ? Reader.ReadInt32() : 0;
	const int32 Type = Reader.CanRead(4) ? static_cast<int32>(Reader.ReadUInt32()) : 0;
	const bool SpeakerOlthoi = Sender.EndsWith(TEXT("&"));
	const bool Foreign = ACEOlthoi::DecodeSpeaker(Sender,PlayerVitals.HeritageGroup,PlayerVitals.QualityBools.FindRef(129));
	if (IsSenderSquelched(SenderGuid,Sender,Type)) return;
	if (Foreign && SenderGuid != PlayerGuid)
	{
		EmitChatMessage(Sender+TEXT(" ")+ACEOlthoi::ForeignSpeech(SpeakerOlthoi),FString(),Type);
		return;
	}
	EmitChatMessage(Text, Sender, Type);
}

void FACESession::HandleChannelBroadcast(FACEBinaryReader& Reader)
{
	// channelId, senderName (empty = "You"), message
	if (!Reader.CanRead(4))
	{
		return;
	}
	const uint32 ChannelId = Reader.ReadUInt32();
	const FString Sender = Reader.ReadString16L();
	const FString Text = Reader.ReadString16L();
	int32 Type;
	const FString Line = ACERetailChat::Format(ChannelId,Sender,Text,Type);
	if (IsSenderSquelched(0,Sender,Type)) return;
	// Retail OnChannelBroadcast remembers who last used @p/@m, separately
	// from private tells; our own empty-sender echoes cannot replace them.
	if (!Sender.IsEmpty())
	{
		if (ChannelId == ACEChatChannel::Patron) LastPatronTellSenderName = Sender;
		if (ChannelId == ACEChatChannel::Monarch) LastMonarchTellSenderName = Sender;
	}
	EmitChatMessage(Line, Sender, Type);
}

void FACESession::HandleSetTurbineChatChannels(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(40))
	{
		return;
	}
	TurbineAllegianceChannel = Reader.ReadUInt32();
	TurbineGeneralChannel = Reader.ReadUInt32();
	TurbineTradeChannel = Reader.ReadUInt32();
	TurbineLfgChannel = Reader.ReadUInt32();
	TurbineRoleplayChannel = Reader.ReadUInt32();
	TurbineOlthoiChannel = Reader.ReadUInt32();
	TurbineSocietyChannel = Reader.ReadUInt32();
	Reader.ReadUInt32(); // SocietyCelestialHand
	Reader.ReadUInt32(); // SocietyEldrytchWeb
	Reader.ReadUInt32(); // SocietyRadiantBlood
	if (TurbineAllegianceChannel == 0) { TurbineAllegianceChannel = ACETurbineChat::Allegiance; }
	if (TurbineGeneralChannel == 0) { TurbineGeneralChannel = ACETurbineChat::General; }
	if (TurbineTradeChannel == 0) { TurbineTradeChannel = ACETurbineChat::Trade; }
	if (TurbineLfgChannel == 0) { TurbineLfgChannel = ACETurbineChat::LFG; }
	if (TurbineRoleplayChannel == 0) { TurbineRoleplayChannel = ACETurbineChat::Roleplay; }
	if (TurbineOlthoiChannel == 0) { TurbineOlthoiChannel = ACETurbineChat::Olthoi; }
	if (TurbineSocietyChannel == 0) { TurbineSocietyChannel = ACETurbineChat::Society; }
}

void FACESession::HandleTurbineChat(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(36))
	{
		return;
	}
	Reader.ReadUInt32(); // bytes to follow
	const uint32 BlobType = Reader.ReadUInt32();
	Reader.ReadUInt32(); // dispatch
	Reader.ReadUInt32();
	Reader.ReadUInt32();
	Reader.ReadUInt32();
	Reader.ReadUInt32();
	Reader.ReadUInt32();
	Reader.ReadUInt32(); // payload size
	if (BlobType == ACETurbineChat::BlobResponseBinary)
	{
		return;
	}
	if (BlobType != ACETurbineChat::BlobEventBinary)
	{
		return;
	}
	const uint32 ChannelId = Reader.ReadUInt32();
	const FString Sender = Reader.ReadPackedUnicode();
	const FString Text = Reader.ReadPackedUnicode();
	if (Reader.CanRead(4)) { Reader.ReadUInt32(); } // extra size
	const int32 Speaker = Reader.CanRead(4) ? Reader.ReadInt32() : 0;
	if (Reader.CanRead(4)) { Reader.ReadUInt32(); } // hresult
	const uint32 ChatType = Reader.CanRead(4) ? Reader.ReadUInt32() : ChannelId;

	const TCHAR* Tag = TEXT("General");
	int32 ChannelLogType = ACEChatMessageType::General;
	switch (ChatType)
	{
	case ACETurbineChat::Allegiance: Tag = TEXT("Allegiance"); ChannelLogType = ACEChatMessageType::Allegiance; break;
	case ACETurbineChat::Trade: Tag = TEXT("Trade"); ChannelLogType = ACEChatMessageType::Trade; break;
	case ACETurbineChat::LFG: Tag = TEXT("LFG"); ChannelLogType = ACEChatMessageType::LFG; break;
	case ACETurbineChat::Roleplay: Tag = TEXT("Roleplay"); ChannelLogType = ACEChatMessageType::Roleplay; break;
	case ACETurbineChat::Society:
	case ACETurbineChat::SocietyCelestialHand:
	case ACETurbineChat::SocietyEldrytchWeb:
	case ACETurbineChat::SocietyRadiantBlood:
		Tag = TEXT("Society"); ChannelLogType = ACEChatMessageType::Society;
		break;
	case ACETurbineChat::Olthoi: Tag = TEXT("Olthoi"); ChannelLogType = ACEChatMessageType::Allegiance; break;
	default: Tag = TEXT("General"); break;
	}
	const FString Line = FString::Printf(TEXT("[%s] %s says, \"%s\""), Tag, *Sender, *Text);
	if (IsSenderSquelched(Speaker,Sender,ChannelLogType)) return;
	EmitChatMessage(Line, Sender, ChannelLogType);
}

void FACESession::HandleHearRangedSpeech(FACEBinaryReader& Reader)
{
	const FString Text = Reader.ReadString16L();
	FString Sender = Reader.ReadString16L();
	const int32 SenderGuid = Reader.CanRead(4) ? Reader.ReadInt32() : 0;
	if (Reader.CanRead(4))
	{
		Reader.ReadFloat(); // range
	}
	const int32 Type = Reader.CanRead(4) ? static_cast<int32>(Reader.ReadUInt32()) : 0;
	const bool SpeakerOlthoi = Sender.EndsWith(TEXT("&"));
	const bool Foreign = ACEOlthoi::DecodeSpeaker(Sender,PlayerVitals.HeritageGroup,PlayerVitals.QualityBools.FindRef(129));
	if (IsSenderSquelched(SenderGuid,Sender,Type)) return;
	if (Foreign && SenderGuid != PlayerGuid)
	{
		EmitChatMessage(Sender+TEXT(" ")+ACEOlthoi::ForeignSpeech(SpeakerOlthoi),FString(),Type);
		return;
	}
	EmitChatMessage(Text, Sender, Type);
}

void FACESession::HandleSoulEmote(FACEBinaryReader& Reader)
{
	// GameMessageSoulEmote: senderId, senderName, emoteText (OtherEmote fragment).
	uint32 SenderGuid = 0;
	if (Reader.CanRead(4))
	{
		SenderGuid = Reader.ReadUInt32();
	}
	FString Sender = Reader.ReadString16L();
	const FString Text = Reader.ReadString16L();
	// Local player already appended "You …" — skip self echo.
	if (SenderGuid != 0 && SenderGuid == static_cast<uint32>(PlayerGuid))
	{
		return;
	}
	const bool SpeakerOlthoi = Sender.EndsWith(TEXT("&"));
	const bool Foreign = ACEOlthoi::DecodeSpeaker(Sender,PlayerVitals.HeritageGroup,PlayerVitals.QualityBools.FindRef(129));
	if (!IsSenderSquelched(SenderGuid,Sender,ACEChatMessageType::Emote))
		EmitChatMessage(Foreign ? ACEOlthoi::ForeignSpeech(SpeakerOlthoi) : Text, Sender, ACEChatMessageType::Emote);
}

bool FACESession::RespondToConfirmation(uint32 Type, uint32 Context, bool Accept)
{
	if (State != EACESessionState::InWorld) return false;
	const int32 Index = Confirmations.IndexOfByPredicate([&](const FACEConfirmation& C) { return C.Type == Type && C.Context == Context; });
	if (Index == INDEX_NONE) return false;
	FACEBinaryWriter Body;
	Body.WriteUInt32(Type); Body.WriteUInt32(Context); Body.WriteUInt32(Accept ? 1u : 0u);
	SendGameAction(0x0275, Body.GetData(), ACEQueue::UIQueue);
	Confirmations.RemoveAt(Index);
	return true;
}

void FACESession::HandleGameEvent(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(12))
	{
		return;
	}
	Reader.ReadUInt32(); // player guid
	Reader.ReadUInt32(); // event sequence
	const uint32 EventType = Reader.ReadUInt32();

	switch (EventType)
	{
	case 0x021D: HandleHouseProfile(Reader); break;
	case 0x0271: HandleAvailableHouses(Reader); break;
	case 0x0075: // StartBarber: publish only a complete retail appearance.
		if(Reader.CanRead(FACEBarberProfile::Count*4))
		{
			for(auto& Value:BarberProfile.Values) Value=Reader.ReadUInt32();
			bBarberOpen=true; ++BarberRevision;
		}
		break;
	case 0x0259: // FailedHouseTransaction: retail refreshes the active slumlord offer.
		if (Reader.CanRead(4))
		{
			Reader.ReadUInt32();
			if (HouseOffer.LordGuid && State==EACESessionState::InWorld)
				SendGameActionU32(0x0258, HouseOffer.LordGuid);
		}
		break;
	case ACEGameEvent::PopupString:
	case 0x0274: // CharacterConfirmationRequest
	case 0x0276: // CharacterConfirmationDone
		HandleServerDialog(EventType, Reader);
		break;
	case 0xF7D1: HandleVRPose(Reader); break;
	case 0xF7D2: HandleHealthFeedback(Reader); break;
	case 0xF7D4: HandleVRRecovery(Reader); break;
	case 0xF7D5: HandleVRCasting(Reader); break;
	case 0xF7D3:
		if (State == EACESessionState::InWorld && (VRCapabilities & 256u) && (Reader.Remaining() == 12 || Reader.Remaining() == 16))
		{
			const int32 Spell = Reader.ReadUInt32(); const float Speed = Reader.ReadFloat(); const uint32 Gravity = Reader.ReadUInt32();
			const float Radius = Reader.CanRead(4) ? Reader.ReadFloat() : .05f;
			if (Spell > 0 && FMath::IsFinite(Speed) && Speed >= 0 && Speed <= 200 && Gravity <= 1
				&& FMath::IsFinite(Radius) && Radius >= 0.f && Radius <= 10.f)
			{
				if (VRSpellProfiles.Num() >= 128) VRSpellProfiles.Reset();
				VRSpellProfiles.Add(Spell,FVector(Speed,Gravity,Radius));
			}
		}
		break;
	case 0xF7D0:
		if (State == EACESessionState::InWorld && Reader.CanRead(8))
		{
			const uint32 Version = Reader.ReadUInt32();
			const uint32 Flags = Reader.ReadUInt32();
			VRCapabilities = Version == 1 ? Flags & 524287u : 0u;
			SendVRSubscriptions();
            if ((VRCapabilities & 8u) != 0) ApplyVRWorldSnapshot(Reader);
			VRMissileWeapon = 0; VRMissileSpeed = 0.f;
			if ((VRCapabilities & 32u) && Reader.CanRead(8))
			{
				const int32 Weapon = Reader.ReadUInt32(); const float Speed = Reader.ReadFloat();
				if (FMath::IsFinite(Speed) && Speed > 0.f && Speed <= 50.f)
				{ VRMissileWeapon = Weapon; VRMissileSpeed = Speed; }
			}
			Log(TEXT("VR server capability response received"));
		}
		break;
	case ACEGameEvent::QueryAgeResponse:
	{
		if (!Reader.CanRead(4)) break;
		const FString Name=Reader.ReadString16L();
		if (!Reader.CanRead(4)) break;
		const FString Age=Reader.ReadString16L();
		EmitChatMessage(Name.IsEmpty() ? TEXT("You have played for ")+Age+TEXT(".") : Name+TEXT(" has played for ")+Age+TEXT("."),TEXT(""),ACEChatMessageType::System);
		break;
	}
	case ACEGameEvent::PlayerDescription:
		HandlePlayerDescription(Reader);
		break;
	case ACEGameEvent::UpdateHealth:
		HandleUpdateHealth(Reader);
		break;
	case ACEGameEvent::QueryItemManaResponse:
		HandleQueryItemManaResponse(Reader);
		break;
	case ACEGameEvent::IdentifyObjectResponse:
		HandleIdentifyObjectResponse(Reader);
		break;
	case ACEGameEvent::ViewContents:
		HandleViewContents(Reader);
		break;
	case ACEGameEvent::CloseGroundContainer:
		HandleCloseGroundContainer(Reader);
		break;
	case ACEGameEvent::ApproachVendor:
		HandleApproachVendor(Reader);
		break;
	case ACEGameEvent::RegisterTrade:
		HandleRegisterTrade(Reader);
		break;
	case ACEGameEvent::ChessJoinGameResponse:
	case ACEGameEvent::ChessStartGame:
	case ACEGameEvent::ChessMoveResponse:
	case ACEGameEvent::ChessOpponentTurn:
	case ACEGameEvent::ChessOpponentStalemate:
	case ACEGameEvent::ChessGameOver:
		HandleChessGameEvent(EventType, Reader);
		break;
	case ACEGameEvent::OpenTrade:
		HandleOpenTrade(Reader);
		break;
	case ACEGameEvent::CloseTrade:
		HandleCloseTrade(Reader);
		break;
	case ACEGameEvent::AddToTrade:
		HandleAddToTradeEvent(Reader);
		break;
	case ACEGameEvent::RemoveFromTrade:
		HandleRemoveFromTradeEvent(Reader);
		break;
	case ACEGameEvent::AcceptTrade:
		HandleAcceptTradeEvent(Reader);
		break;
	case ACEGameEvent::DeclineTrade:
		HandleDeclineTradeEvent(Reader);
		break;
	case ACEGameEvent::ResetTrade:
		HandleResetTradeEvent(Reader);
		break;
	case ACEGameEvent::TradeFailure:
		HandleTradeFailure(Reader);
		break;
	case ACEGameEvent::ClearTradeAcceptance:
		HandleClearTradeAcceptance(Reader);
		break;
	case ACEGameEvent::ChannelIndex:
	case ACEGameEvent::ChannelList:
	{
		if (!Reader.CanRead(4)) break;
		const uint32 Count = Reader.ReadUInt32();
		if (Count > 4096) break;
		TArray<FString> Names;
		for (uint32 I=0; I<Count && Reader.CanRead(2); ++I) Names.Add(Reader.ReadString16L());
		EmitChatMessage(Names.IsEmpty() ? TEXT("No entries.") : FString::Join(Names,TEXT(", ")),
			FString(),ACEChatMessageType::System);
		break;
	}
	case ACEGameEvent::ChannelBroadcast:
		HandleChannelBroadcast(Reader);
		break;
	case ACEGameEvent::FellowshipFullUpdate:
		HandleFellowshipFullUpdate(Reader);
		break;
	case ACEGameEvent::FellowshipUpdateFellow:
		HandleFellowshipUpdateFellow(Reader);
		break;
	case ACEGameEvent::FellowshipQuit:
		HandleFellowshipQuitEvent(Reader);
		break;
	case ACEGameEvent::FellowshipDismiss:
		HandleFellowshipDismissEvent(Reader);
		break;
	case ACEGameEvent::FellowshipDisband:
		HandleFellowshipDisband(Reader);
		break;
	case ACEGameEvent::FellowshipFellowUpdateDone:
	case ACEGameEvent::FellowshipFellowStatsDone:
		break;
	case ACEGameEvent::AllegianceUpdate:
		HandleAllegianceUpdate(Reader);
		break;
	case ACEGameEvent::AllegianceLoginNotification:
		HandleAllegianceLoginNotification(Reader);
		break;
	case ACEGameEvent::AllegianceUpdateDone:
		break;
	case ACEGameEvent::AllegianceInfoResponse:
		HandleAllegianceInfoResponse(Reader);
		break;
	case ACEGameEvent::FriendsListUpdate:
		HandleFriendsListUpdate(Reader);
		break;
	case ACEGameEvent::SetSquelchDB:
		HandleSetSquelchDB(Reader);
		break;
	case ACEGameEvent::SalvageOperationsResult:
		HandleSalvageOperationsResult(Reader);
		break;
	case ACEGameEvent::SendClientContractTrackerTable:
		HandleContractTrackerTable(Reader);
		break;
	case ACEGameEvent::SendClientContractTracker:
		HandleContractTracker(Reader);
		break;
	case ACEGameEvent::HouseData:
		HandleHouseData(Reader);
		break;
	case ACEGameEvent::HouseStatus:
		HandleHouseStatus(Reader);
		break;
	case ACEGameEvent::HouseUpdateRentTime:
		HandleHouseUpdateRentTime(Reader);
		break;
	case ACEGameEvent::HouseUpdateRentPayment:
		HandleHouseUpdateRentPayment(Reader);
		break;
	case ACEGameEvent::HouseUpdateHAR:
		HandleHouseUpdateHAR(Reader);
		break;
	case ACEGameEvent::SetTurbineChatChannels:
		HandleSetTurbineChatChannels(Reader);
		break;
	case ACEGameEvent::BookDataResponse:
		HandleBookDataResponse(Reader);
		break;
	case ACEGameEvent::BookPageDataResponse:
		HandleBookPageDataResponse(Reader);
		break;
	case ACEGameEvent::BookModifyPageResponse:
	case ACEGameEvent::BookAddPageResponse:
	case ACEGameEvent::BookDeletePageResponse:
		HandleBookWriteResponse(EventType, Reader);
		break;
	case ACEGameEvent::CharacterTitle:
		HandleCharacterTitle(Reader);
		break;
	case ACEGameEvent::UpdateTitle:
		HandleUpdateTitle(Reader);
		break;
	case ACEGameEvent::UseDone:
		HandleUseDone(Reader);
		break;
	case ACEGameEvent::ItemAppraiseDone:
		HandleItemAppraiseDone(Reader);
		break;
	case ACEGameEvent::MagicUpdateSpell:
		HandleMagicUpdateSpell(Reader);
		break;
	case ACEGameEvent::MagicRemoveSpell:
		HandleMagicRemoveSpell(Reader);
		break;
	case ACEGameEvent::MagicUpdateEnchantment:
		HandleMagicUpdateEnchantment(Reader);
		break;
	case ACEGameEvent::MagicRemoveEnchantment:
		HandleMagicRemoveEnchantment(Reader);
		break;
	case ACEGameEvent::MagicUpdateMultipleEnchantments:
		HandleMagicUpdateMultipleEnchantments(Reader);
		break;
	case ACEGameEvent::MagicRemoveMultipleEnchantments:
		HandleMagicRemoveMultipleEnchantments(Reader);
		break;
	case ACEGameEvent::MagicPurgeEnchantments:
		HandleMagicPurgeEnchantments(Reader);
		break;
	case ACEGameEvent::MagicPurgeBadEnchantments:
		HandleMagicPurgeBadEnchantments(Reader);
		break;
	case ACEGameEvent::MagicDispelEnchantment:
		HandleMagicDispelEnchantment(Reader);
		break;
	case ACEGameEvent::MagicDispelMultipleEnchantments:
		HandleMagicDispelMultipleEnchantments(Reader);
		break;
	case ACEGameEvent::PingResponse:
		HandlePingResponse(Reader);
		break;
	case ACEGameEvent::InventoryPutObjInContainer:
		HandleInventoryPutObjInContainer(Reader);
		break;
	case ACEGameEvent::WieldItem:
		HandleWieldItem(Reader);
		break;
	case ACEGameEvent::InventoryPutObjIn3D:
		HandleInventoryPutObjIn3D(Reader);
		break;
	case ACEGameEvent::VictimNotification:
		HandleCombatVictimNotification(Reader);
		break;
	case ACEGameEvent::KillerNotification:
		HandleCombatKillerNotification(Reader);
		break;
	case ACEGameEvent::AttackerNotification:
		HandleCombatAttackerNotification(Reader);
		break;
	case ACEGameEvent::CombatCommenceAttack:
		CursorAttackUntil = FPlatformTime::Seconds()+30.;
		bServerAttackInProgress = true;
		LastAttackError = 0;
		++CombatEventRevision;
		break;
	case ACEGameEvent::AttackDone:
		if (!Reader.CanRead(4)) break;
		CursorAttackUntil = 0.;
		bServerAttackInProgress = false;
		LastAttackError = Reader.ReadUInt32();
		++CombatEventRevision;
		break;
	case ACEGameEvent::DefenderNotification:
		HandleCombatDefenderNotification(Reader);
		break;
	case ACEGameEvent::EvasionAttackerNotification:
		HandleCombatEvasionAttacker(Reader);
		break;
	case ACEGameEvent::EvasionDefenderNotification:
		HandleCombatEvasionDefender(Reader);
		break;
	case ACEGameEvent::WeenieError:
		HandleWeenieError(Reader);
		break;
	case ACEGameEvent::WeenieErrorWithString:
		HandleWeenieErrorWithString(Reader);
		break;
	case ACEGameEvent::CommunicationTransientString:
		HandleTransientString(Reader);
		break;
	case ACEGameEvent::InventoryServerSaveFailed:
		HandleInventoryServerSaveFailed(Reader);
		break;
	case ACEGameEvent::Tell:
		HandleTell(Reader);
		break;
	default:
		break;
	}
}

void FACESession::HandleCombatVictimNotification(FACEBinaryReader& Reader)
{
	const FString Msg = Reader.ReadString16L();
	if (!Msg.IsEmpty())
	{
		EmitChatMessage(Msg, TEXT(""), ACEChatMessageType::Broadcast);
	}
}

void FACESession::HandleCombatKillerNotification(FACEBinaryReader& Reader)
{
	const FString Msg = Reader.ReadString16L();
	if (!Msg.IsEmpty())
	{
		// Retail renders this as Default (green). ACE also sends summon kills here.
		// Route kill results with outgoing combat without hiding unrelated Default
		// gameplay messages or the player's own death notice.
		EmitChatMessage(Msg, TEXT(""), ACEChatMessageType::Broadcast, ACEChatMessageType::CombatSelf);
	}
}

static bool ReadCombatNotificationName(FACEBinaryReader& Reader, FString& Name)
{
	if (!Reader.CanRead(2)) return false;
	const int32 Start = Reader.Tell();
	const int32 Length = Reader.ReadUInt16();
	Reader.Seek(Start);
	if (!Reader.CanRead((Length + 2 + 3) & ~3)) return false;
	Name = Reader.ReadString16L();
	return true;
}

void FACESession::HandleCombatAttackerNotification(FACEBinaryReader& Reader)
{
	FString DefenderName;
	if (!ReadCombatNotificationName(Reader, DefenderName)) return;
	if (!Reader.CanRead(4 + 8 + 4 + 4 + 8))
	{
		return;
	}
	const uint32 DamageType = Reader.ReadUInt32();
	const float Percent = static_cast<float>(Reader.ReadDouble());
	const uint32 Damage = Reader.ReadUInt32();
	const bool bCritical = Reader.ReadUInt32() != 0;
	const uint64 Conditions = Reader.ReadUInt64();
	const FString Msg = ACECombatChat::FormatAttackerNotification(
		DefenderName, DamageType, Percent, Damage, bCritical, Conditions);
	EmitChatMessage(Msg, TEXT(""), ACEChatMessageType::CombatSelf);
	OnCombatFeedback.Broadcast(DefenderName, static_cast<int32>(FMath::Min(Damage, uint32(MAX_int32))), false, bCritical);
}

void FACESession::HandleCombatDefenderNotification(FACEBinaryReader& Reader)
{
	FString AttackerName;
	if (!ReadCombatNotificationName(Reader, AttackerName)) return;
	if (!Reader.CanRead(4 + 8 + 4 + 4 + 4 + 8))
	{
		return;
	}
	const uint32 DamageType = Reader.ReadUInt32();
	const float Percent = static_cast<float>(Reader.ReadDouble());
	const uint32 Damage = Reader.ReadUInt32();
	const uint32 DamageLocation = Reader.ReadUInt32();
	const bool bCritical = Reader.ReadUInt32() != 0;
	const uint64 Conditions = Reader.ReadUInt64();
	Reader.Align();
	// Retail refreshes the retaliation timer on each hit. Identity comes from
	// CurrentAttacker (IID 11), never a name search among identical creatures.
	LastAttackerTimeSeconds = FPlatformTime::Seconds();
	const FString Msg = ACECombatChat::FormatDefenderNotification(
		AttackerName, DamageType, Percent, Damage, DamageLocation, bCritical, Conditions);
	EmitChatMessage(Msg, TEXT(""), ACEChatMessageType::CombatEnemy);
}

void FACESession::HandleCombatEvasionAttacker(FACEBinaryReader& Reader)
{
	FString DefenderName;
	if (!ReadCombatNotificationName(Reader, DefenderName)) return;
	OnCombatFeedback.Broadcast(DefenderName, 0, false, false);
	EmitChatMessage(ACECombatChat::FormatEvasionAttacker(DefenderName), TEXT(""),
		ACEChatMessageType::CombatSelf);
}

void FACESession::HandleCombatEvasionDefender(FACEBinaryReader& Reader)
{
	FString AttackerName;
	if (!ReadCombatNotificationName(Reader, AttackerName)) return;
	LastAttackerTimeSeconds = FPlatformTime::Seconds();
	OnCombatFeedback.Broadcast(AttackerName, 0, true, false);
	EmitChatMessage(ACECombatChat::FormatEvasionDefender(AttackerName), TEXT(""),
		ACEChatMessageType::CombatEnemy);
}

void FACESession::HandleWeenieError(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(4))
	{
		return;
	}
	const uint32 Code = Reader.ReadUInt32();
	if (Code == 0x0529u) // TradeComplete: authoritative commit, before ResetTrade clears offers.
	{
		const auto Transferred = TradeSelfItems;
		// ACE may recreate an item for its original owner if delivery fails.
		// Its earlier Delete was already applied; do not erase that returned copy.
		for (int32 Guid : Transferred) if (!TradeRemovedItems.Contains(Guid)) RemoveInventoryObject(Guid);
	}
	// Some servers reject a world Use with WeenieError alone, without UseDone.
	// Do not retain its local busy gate until timeout after that terminal reply.
	// Keep unrelated inventory/targeted-use transactions locked: a combat error
	// received while drinking or healing must not permit a duplicate consumption.
	if (bUseBusy && UseTargetGuid == 0 && (Code == 0x001Du || Code == 0x0036u))
	{
		const FACEWorldObject* Source = WorldObjects.Find(UseSourceGuid);
		if (Source && Source->bHasPosition && Source->ContainerId == 0 && Source->WielderId == 0
			&& Source->Guid != PlayerGuid)
		{
			Log(FString::Printf(TEXT("World use rejected: error=0x%04X source=0x%08X combat=%d age=%.2fs"),
				Code, UseSourceGuid, PlayerVitals.CombatMode, FPlatformTime::Seconds()-UseStartedAt));
			ClearPendingUse();
			ReleaseCursorUse(true);
			OnUseDone.Broadcast(Code);
		}
	}
	if (Code == 0x003D) // A server failure must also release local approach prediction.
	{
		ReportMoveToFailure(Code);
		return;
	}
	// Portal cancel/teleport status — retail does not dump these into chat as "Error 0x…".
	if (Code == 0x003Bu || Code == 0x003Cu) // ILeftTheWorld / ITeleported
	{
		return;
	}
	const FString Msg = ACECombatChat::LookupWeenieError(Code);
	if (!Msg.IsEmpty())
	{
		// Retail HandleFailureEvent uses each message's chat destination.
		EmitChatMessage(Msg, TEXT(""), ACECombatChat::WeenieErrorChatType(Code));
	}
}

void FACESession::ReportMoveToFailure(uint32 Error)
{
	OnMoveToFailed.Broadcast(Error);
	// Stop the controller's active approach before OnUseDone clears its state.
	CancelPendingUse(TEXT("approach failed"));
	const FString Message = ACECombatChat::LookupWeenieError(Error);
	if (!Message.IsEmpty()) EmitChatMessage(Message, TEXT(""), ACECombatChat::WeenieErrorChatType(Error));
}

void FACESession::HandleWeenieErrorWithString(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(6))
	{
		return;
	}
	const uint32 Code = Reader.ReadUInt32();
	const int32 ArgumentPosition = Reader.Tell();
	const uint16 ArgumentBytes = Reader.ReadUInt16();
	if (!Reader.CanRead(ArgumentBytes)) return;
	Reader.Seek(ArgumentPosition);
	const FString Arg = Reader.ReadString16L();
	const FString Msg = ACECombatChat::LookupWeenieErrorWithString(Code, Arg);
	if (!Msg.IsEmpty())
	{
		EmitChatMessage(Msg, TEXT(""), ACECombatChat::WeenieErrorChatType(Code));
	}
}

void FACESession::HandleTransientString(FACEBinaryReader& Reader)
{
	const FString Msg = Reader.ReadString16L();
	if (!Msg.IsEmpty())
	{
		EmitChatMessage(Msg, TEXT(""), ACEChatMessageType::TransientInfo);
	}
}

void FACESession::HandleTell(FACEBinaryReader& Reader)
{
	// GameEvent Tell: message, senderName, senderId, targetId, chatType, pad.
	const FString Text = Reader.ReadString16L();
	FString Sender = Reader.ReadString16L();
	int32 SenderId = 0;
	if (Reader.CanRead(4))
	{
		SenderId = static_cast<int32>(Reader.ReadUInt32());
	}
	if (Reader.CanRead(4))
	{
		Reader.ReadUInt32(); // targetId
	}
	const int32 Type = Reader.CanRead(4)
		? static_cast<int32>(Reader.ReadUInt32())
		: ACEChatMessageType::Tell;
	if (Reader.CanRead(4))
	{
		Reader.ReadUInt32();
	}
	// NPC dialogue must not replace the last player teller (retail IID range).
	const bool SpeakerOlthoi = Sender.EndsWith(TEXT("&"));
	const bool Foreign = ACEOlthoi::DecodeSpeaker(Sender,PlayerVitals.HeritageGroup,PlayerVitals.QualityBools.FindRef(129));
	if (IsSenderSquelched(SenderId,Sender,Type)) return;
	if (Foreign && SenderId != PlayerGuid)
	{
		// Retail preserves the incoming channel/color, but formats foreign
		// speech as an action rather than a quoted tell (and no reply link).
		EmitChatMessage(Sender+TEXT(" ")+ACEOlthoi::ForeignSpeech(SpeakerOlthoi),FString(),Type);
		return;
	}
	if ((Type == ACEChatMessageType::Tell || Type == ACEChatMessageType::AdminTell)
		&& static_cast<uint32>(SenderId) >= 0x50000001u
		&& static_cast<uint32>(SenderId) <= 0x6FFFFFFFu && SenderId != PlayerGuid)
	{
		LastTellSenderGuid = SenderId;
		LastTellSenderName = Sender;
		OnPlayerTell.Broadcast(Text,Sender,SenderId);
	}
	EmitChatMessage(Text, Sender, Type);
	if (const auto* NPC = WorldObjects.Find(SenderId); NPC && !NPC->bIsPlayer && (NPC->ItemType & ACEItemType::Creature))
		OnNPCSpeech.Broadcast(SenderId, Text);
}

void FACESession::HandlePlayerDescription(FACEBinaryReader& Reader)
{
	TMap<uint32,FACEPosition> PositionQualities;
	// Mirrors ACE GameEventPlayerDescription.WriteEventBody (CACQualities + attribute cache).
	constexpr uint32 FlagPropertyInt32  = 0x0001;
	constexpr uint32 FlagPropertyBool   = 0x0002;
	constexpr uint32 FlagPropertyDouble = 0x0004;
	constexpr uint32 FlagPropertyDid    = 0x0008;
	constexpr uint32 FlagPropertyString = 0x0010;
	constexpr uint32 FlagPosition       = 0x0020;
	constexpr uint32 FlagPropertyIid    = 0x0040;
	constexpr uint32 FlagPropertyInt64  = 0x0080;

	constexpr uint32 VectorAttribute = 0x0001;
	constexpr uint32 VectorSkill     = 0x0002;
	constexpr uint32 VectorSpell     = 0x0100;
	constexpr uint32 VectorEnchant   = 0x0200;

	auto SkillName = [](uint32 Id) -> FString
	{
		static const TCHAR* Names[] = {
			TEXT("None"), TEXT("Axe"), TEXT("Bow"), TEXT("Crossbow"), TEXT("Dagger"), TEXT("Mace"),
			TEXT("Melee Defense"), TEXT("Missile Defense"), TEXT("Sling"), TEXT("Spear"), TEXT("Staff"),
			TEXT("Sword"), TEXT("Thrown Weapon"), TEXT("Unarmed Combat"), TEXT("Arcane Lore"),
			TEXT("Magic Defense"), TEXT("Mana Conversion"), TEXT("Spellcraft"), TEXT("Item Tinkering"),
			TEXT("Assess Person"), TEXT("Deception"), TEXT("Healing"), TEXT("Jump"), TEXT("Lockpick"),
			TEXT("Run"), TEXT("Awareness"), TEXT("Arms and Armor Repair"), TEXT("Assess Creature"),
			TEXT("Weapon Tinkering"), TEXT("Armor Tinkering"), TEXT("Magic Item Tinkering"),
			TEXT("Creature Enchantment"), TEXT("Item Enchantment"), TEXT("Life Magic"), TEXT("War Magic"),
			TEXT("Leadership"), TEXT("Loyalty"), TEXT("Fletching"), TEXT("Alchemy"), TEXT("Cooking"),
			TEXT("Salvaging"), TEXT("Two Handed Combat"), TEXT("Gearcraft"), TEXT("Void Magic"),
			TEXT("Heavy Weapons"), TEXT("Light Weapons"), TEXT("Finesse Weapons"), TEXT("Missile Weapons"),
			TEXT("Shield"), TEXT("Dual Wield"), TEXT("Recklessness"), TEXT("Sneak Attack"),
			TEXT("Dirty Fighting"), TEXT("Challenge"), TEXT("Summoning")
		};
		return (Id < UE_ARRAY_COUNT(Names)) ? FString(Names[Id]) : FString::Printf(TEXT("Skill %u"), Id);
	};

	if (!Reader.CanRead(8))
	{
		return;
	}
	const uint32 PropertyFlags = Reader.ReadUInt32();
	Reader.ReadUInt32(); // weenie type

	FACEPlayerVitals Vitals;
	int32 DescriptionAttackerGuid = 0;

	auto ReadTableHeader = [&Reader]() -> int32
	{
		if (!Reader.CanRead(4))
		{
			return -1;
		}
		const uint16 Count = Reader.ReadUInt16();
		Reader.ReadUInt16(); // bucket count
		return Count;
	};

	if (PropertyFlags & FlagPropertyInt32)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(8); ++i)
		{
			const uint32 Key = Reader.ReadUInt32();
			const int32 Value = Reader.ReadInt32();
			Vitals.StatQualityInts.Add(static_cast<int32>(Key), Value);
			if (Key == 25) // PropertyInt.Level
			{
				Vitals.Level = Value;
			}
			else if (Key == 24) // PropertyInt.AvailableSkillCredits
			{
				Vitals.AvailableSkillCredits = Value;
			}
			else if (Key == 113) // PropertyInt.Gender
			{
				Vitals.Gender = Value;
			}
			else if (Key == 134) // PropertyInt.PlayerKillerStatus
			{
				Vitals.PlayerKillerStatus = Value;
			}
			else if (Key == 188) // PropertyInt.HeritageGroup
			{
				Vitals.HeritageGroup = Value;
			}
			else if (Key == 40) // PropertyInt.CombatMode
			{
				// CharacterDesc path seeds vitals before broadcast; keep raw value.
				Vitals.CombatMode = Value;
			}
			else if (Key == 5) // PropertyInt.EncumbranceVal — apply once Self WO exists
			{
				bHasPlayerEncumbrance = true;
				PlayerEncumbranceVal = Value;
				if (PlayerGuid != 0)
				{
					if (FACEWorldObject* Self = WorldObjects.Find(PlayerGuid))
					{
						Self->Burden = Value;
					}
				}
			}
			else if (Key == 322) { Vitals.AetheriaUnlocked = Value & 7; }
			else if (Key == 230) // PropertyInt.AugmentationIncreasedCarryingCapacity
			{
				Vitals.CarryingCapacityAugs = FMath::Clamp(Value, 0, 5);
			}
			else if (Key == 129) // PropertyInt.VitaeCpPool
			{
				VitaeCpPool = Value;
			}
			else if (Key == 139) // PropertyInt.DeathLevel
			{
				DeathLevel = Value;
			}
			else if (Key == 125) // PropertyInt.Age
			{
				Vitals.AgeSeconds = Value;
			}
			else if (Key == 43) // PropertyInt.NumDeaths
			{
				Vitals.NumDeaths = Value;
			}
			else if (Key == 181) // PropertyInt.ChessRank
			{
				Vitals.ChessRank = Value;
			}
			else if (Key == 192) // PropertyInt.FakeFishingSkill
			{
				Vitals.FishingSkill = Value;
			}
		}
	}
	if (PropertyFlags & FlagPropertyInt64)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(12); ++i)
		{
			const uint32 Key = Reader.ReadUInt32();
			const int64 Value = static_cast<int64>(Reader.ReadUInt64());
			Vitals.QualityInt64s.Add(static_cast<int32>(Key), Value);
			if (Key == 1) // PropertyInt64.TotalExperience
			{
				Vitals.TotalExperience = Value;
			}
			else if (Key == 2) // PropertyInt64.AvailableExperience
			{
				Vitals.AvailableExperience = Value;
			}
			else if (Key == 6) // PropertyInt64.AvailableLuminance
			{
				Vitals.AvailableLuminance = Value;
			}
			else if (Key == 7) // PropertyInt64.MaximumLuminance
			{
				Vitals.MaximumLuminance = Value;
			}
		}
	}
	if (PropertyFlags & FlagPropertyBool)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(8); ++i)
		{
			const uint32 Key = Reader.ReadUInt32();
			const uint32 Value = Reader.ReadUInt32();
			Vitals.QualityBools.Add(static_cast<int32>(Key), Value != 0);
			// PropertyBool.IsAdmin=44, IsArch=45, IsSentinel=46 (SendOnLogin).
			if ((Key == 44 || Key == 45 || Key == 46) && Value != 0)
			{
				bLocalPlayerIsAdmin = true;
			}
		}
	}
	if (PropertyFlags & FlagPropertyDouble)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(12); ++i)
		{
			const int32 Key = static_cast<int32>(Reader.ReadUInt32());
			const double Value = Reader.ReadDouble();
			if (FMath::IsFinite(Value)) Vitals.QualityDoubles.Add(Key, Value);
		}
	}
	if (PropertyFlags & FlagPropertyString)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(6); ++i)
		{
			const uint32 Key = Reader.ReadUInt32();
			const FString Value = Reader.ReadString16L();
			Vitals.QualityStrings.Add(static_cast<int32>(Key), Value);
			if (Key == 5) // PropertyString.Template
			{
				Vitals.TemplateName = Value;
			}
		}
	}
	if (PropertyFlags & FlagPropertyDid)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(8); ++i)
		{
			Reader.ReadUInt32();
			Reader.ReadUInt32();
		}
	}
	if (PropertyFlags & FlagPropertyIid)
	{
		const int32 Count = ReadTableHeader();
		if (Count < 0 || !Reader.CanRead(Count * 8)) return;
		for (int32 i = 0; i < Count; ++i)
		{
			const uint32 Key = Reader.ReadUInt32();
			const int32 Value = Reader.ReadInt32();
			if (Key == 11) DescriptionAttackerGuid = Value;
		}
	}
	if (PropertyFlags & FlagPosition)
	{
		const int32 Count = ReadTableHeader();
		// key u32 + landblock u32 + xyz 3*f32 + quat 4*f32 = 36 bytes per entry.
		// Keep position qualities separate from the physical player position.
		if (Count<0 || !Reader.CanRead(Count*36)) return;
		for (int32 i=0;i<Count;++i)
		{
			const uint32 Key=Reader.ReadUInt32();
			FACEPosition P; P.CellId=Reader.ReadUInt32();
			P.Location.X=Reader.ReadFloat();P.Location.Y=Reader.ReadFloat();P.Location.Z=Reader.ReadFloat();
			P.RotationW=Reader.ReadFloat();
			P.RotationXYZ.X=Reader.ReadFloat();P.RotationXYZ.Y=Reader.ReadFloat();P.RotationXYZ.Z=Reader.ReadFloat();
			PositionQualities.Add(Key,P);
		}
	}

	if (!Reader.CanRead(8))
	{
		return;
	}
	const uint32 VectorFlags = Reader.ReadUInt32();
	Reader.ReadUInt32(); // has health

	if (VectorFlags & VectorAttribute)
	{
		if (!Reader.CanRead(4))
		{
			return;
		}
		const uint32 AttribFlags = Reader.ReadUInt32();

		auto ReadAttribute = [&Reader](int32& OutCurrent, int32& OutRanks, int32& OutStart, int32& OutXp) -> bool
		{
			if (!Reader.CanRead(12))
			{
				return false;
			}
			OutRanks = static_cast<int32>(Reader.ReadUInt32());
			OutStart = static_cast<int32>(Reader.ReadUInt32());
			OutXp = static_cast<int32>(Reader.ReadUInt32());
			OutCurrent = OutRanks + OutStart;
			return true;
		};
		auto ReadVital = [&Reader](int32& OutRanks, int32& OutStart, int32& OutXp, int32& OutCurrent) -> bool
		{
			if (!Reader.CanRead(16))
			{
				return false;
			}
			OutRanks = static_cast<int32>(Reader.ReadUInt32());
			OutStart = static_cast<int32>(Reader.ReadUInt32());
			OutXp = static_cast<int32>(Reader.ReadUInt32());
			OutCurrent = static_cast<int32>(Reader.ReadUInt32());
			return true;
		};

		bool bOk = true;
		if (AttribFlags & 0x001) { bOk = bOk && ReadAttribute(Vitals.Strength, Vitals.StrengthRanks, Vitals.StrengthStart, Vitals.StrengthXpSpent); }
		if (AttribFlags & 0x002) { bOk = bOk && ReadAttribute(Vitals.Endurance, Vitals.EnduranceRanks, Vitals.EnduranceStart, Vitals.EnduranceXpSpent); }
		if (AttribFlags & 0x004) { bOk = bOk && ReadAttribute(Vitals.Quickness, Vitals.QuicknessRanks, Vitals.QuicknessStart, Vitals.QuicknessXpSpent); }
		if (AttribFlags & 0x008) { bOk = bOk && ReadAttribute(Vitals.Coordination, Vitals.CoordinationRanks, Vitals.CoordinationStart, Vitals.CoordinationXpSpent); }
		if (AttribFlags & 0x010) { bOk = bOk && ReadAttribute(Vitals.Focus, Vitals.FocusRanks, Vitals.FocusStart, Vitals.FocusXpSpent); }
		if (AttribFlags & 0x020) { bOk = bOk && ReadAttribute(Vitals.Self, Vitals.SelfRanks, Vitals.SelfStart, Vitals.SelfXpSpent); }
		if (AttribFlags & 0x040) { bOk = bOk && ReadVital(Vitals.HealthRanks, Vitals.HealthStart, Vitals.HealthXpSpent, Vitals.Health); }
		if (AttribFlags & 0x080) { bOk = bOk && ReadVital(Vitals.StaminaRanks, Vitals.StaminaStart, Vitals.StaminaXpSpent, Vitals.Stamina); }
		if (AttribFlags & 0x100) { bOk = bOk && ReadVital(Vitals.ManaRanks, Vitals.ManaStart, Vitals.ManaXpSpent, Vitals.Mana); }
		if (!bOk)
		{
			return;
		}
	}

	if (VectorFlags & VectorSkill)
	{
		const int32 Count = ReadTableHeader();
		Vitals.Skills.Reset();
		Vitals.Skills.Reserve(Count);
		// skillId u32, ranks u16, u16, sac u32, xp u32, init u32, resistance u32, lastUsed f64 = 32 bytes
		for (int32 i = 0; i < Count && Reader.CanRead(32); ++i)
		{
			FACESkillInfo Skill;
			Skill.SkillId = static_cast<int32>(Reader.ReadUInt32());
			Skill.Ranks = static_cast<int32>(Reader.ReadUInt16());
			Reader.ReadUInt16();
			Skill.AdvancementClass = static_cast<int32>(Reader.ReadUInt32());
			Skill.XpSpent = static_cast<int32>(Reader.ReadUInt32());
			Skill.InitLevel = static_cast<int32>(Reader.ReadUInt32());
			Reader.ReadUInt32(); // resistance_of_last_check
			Reader.ReadDouble(); // last_time_used
			Skill.Name = SkillName(static_cast<uint32>(Skill.SkillId));
			Skill.Current = Skill.InitLevel + Skill.Ranks;
			// Formula attributes (retail-ish base without buffs).
			switch (Skill.SkillId)
			{
			case 6:  Skill.Current += (Vitals.Coordination + Vitals.Quickness) / 3; break; // MeleeDefense
			case 7:  Skill.Current += (Vitals.Coordination + Vitals.Quickness) / 5; break; // MissileDefense
			case 15: Skill.Current += (Vitals.Focus + Vitals.Self) / 7; break; // MagicDefense
			case 22: Skill.Current += (Vitals.Strength + Vitals.Coordination) / 2; break; // Jump
			case 24: Skill.Current += Vitals.Quickness; break; // Run
			default: break;
			}
			if (Skill.SkillId == 24)
			{
				Vitals.RunSkillCurrent = Skill.Current;
			}
			if (Skill.SkillId == 22)
			{
				Vitals.JumpSkillCurrent = Skill.Current;
			}
			Vitals.Skills.Add(Skill);
		}
	}

	KnownSpells.Reset();
	SpellBars.SetNum(8);
	++SpellDataRevision;
	for (TArray<int32>& Bar : SpellBars) { Bar.Reset(); }
	ShortcutObjects.Reset();

	if (VectorFlags & VectorSpell)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(8); ++i)
		{
			const int32 SpellId = Reader.ReadInt32();
			Reader.ReadFloat(); // configuration (ACE writes 2.0)
			if (SpellId > 0)
			{
				KnownSpells.AddUnique(SpellId);
			}
		}
	}

	if (VectorFlags & VectorEnchant)
	{
		if (!Reader.CanRead(4)) return;
		const uint32 EnchantMask = Reader.ReadUInt32();
		if (EnchantMask & ~0xFu) return;
		TArray<FACEActiveEnchantment> UpdatedEnchantments;
		if ((EnchantMask & 0x1) && !ReadEnchantmentList(Reader, UpdatedEnchantments)) return;
		if ((EnchantMask & 0x2) && !ReadEnchantmentList(Reader, UpdatedEnchantments)) return;
		if (EnchantMask & 0x8)
		{
			const int32 Start = UpdatedEnchantments.Num();
			if (!ReadEnchantmentList(Reader, UpdatedEnchantments)) return;
			for (int32 I = Start; I < UpdatedEnchantments.Num(); ++I) UpdatedEnchantments[I].bCooldown = true;
		}
		if (EnchantMask & 0x4)
		{
			FACEActiveEnchantment Vitae;
			if (!ReadEnchantmentRecord(Reader, Vitae)) return;
			Vitae.bVitae = true;
			UpdatedEnchantments.Add(Vitae);
		}
		// A registry snapshot carries the server's complete lists. Do not apply
		// incremental recast replacement rules to entries within that snapshot.
		ActiveEnchantments = MoveTemp(UpdatedEnchantments);
		RecomputeAttributeEnchantments(Vitals);
	}

	// Player module / options (shortcuts, 8 spell bars, filters…).
	if (Reader.CanRead(8))
	{
		constexpr uint32 OptShortcut = 0x00000001;
		constexpr uint32 OptDesiredComps = 0x00000008;
		constexpr uint32 OptSpellbookFilters = 0x00000020;
		constexpr uint32 OptCharacterOptions2 = 0x00000040;
		constexpr uint32 OptGameplayOptions = 0x00000200;
		constexpr uint32 OptSpellLists8 = 0x00000400;

		const uint32 OptionFlags = Reader.ReadUInt32();
		CharacterOptions1 = Reader.ReadUInt32();

		if (OptionFlags & OptShortcut)
		{
			if (Reader.CanRead(4))
			{
				const int32 Count = Reader.ReadInt32();
				for (int32 i = 0; i < Count && Reader.CanRead(12); ++i)
				{
					const int32 Index = Reader.ReadInt32();
					const int32 ObjectGuid = Reader.ReadInt32();
					Reader.Skip(4); // layered spell u16+u16 (unused)
					if (Index >= 0 && Index < 100)
					{
						if (ShortcutObjects.Num() <= Index)
						{
							ShortcutObjects.SetNumZeroed(Index + 1);
						}
						ShortcutObjects[Index] = ObjectGuid;
					}
				}
			}
		}

		// PlayerModule::UnPack always reads the first list; older clients have
		// five or seven bars. Validate counts before allocating server-sized data.
		const int32 BarCount = (OptionFlags & 0x04u) ? 5 : (OptionFlags & 0x10u) ? 7
			: (OptionFlags & OptSpellLists8) ? 8 : 1;
		{
			for (int32 Bar = 0; Bar < BarCount; ++Bar)
			{
				if (!Reader.CanRead(4)) { return; }
				const int32 Count = Reader.ReadInt32();
				if (Count < 0 || Count > Reader.Remaining() / 4) { return; }
				const int32 BarSize = FMath::Max(56, Count);
				SpellBars[Bar].SetNumZeroed(BarSize);
				for (int32 i = 0; i < Count && Reader.CanRead(4); ++i)
				{
					const int32 SpellId = Reader.ReadInt32();
					if (i < BarSize)
					{
						SpellBars[Bar][i] = SpellId;
					}
				}
			}
		}

		DesiredComponents.Reset();
		if (OptionFlags & OptDesiredComps)
		{
			const int32 Count = ReadTableHeader();
			for (int32 i = 0; i < Count && Reader.CanRead(8); ++i)
			{
				const int32 Wcid = static_cast<int32>(Reader.ReadUInt32());
				const int32 Quantity = Reader.ReadInt32();
				if (Wcid != 0)
				{
					DesiredComponents.Add(Wcid, Quantity);
				}
			}
		}

		SpellbookFilters = 0x3FFFu;
		if (OptionFlags & OptSpellbookFilters)
		{
			if (!Reader.CanRead(4)) { return; }
			SpellbookFilters = Reader.ReadUInt32();
		}

		CharacterOptions2 = 9733888u; // PlayerModule::UnPack's legacy default.
		if (OptionFlags & OptCharacterOptions2)
		{
			if (!Reader.CanRead(4)) { return; }
			CharacterOptions2 = Reader.ReadUInt32();
		}
		// Legacy timestamp and GenericQualitiesData occur before gameplay options.
		// They cannot be mistaken for the inventory footer on non-ACE servers.
		auto SkipString = [&Reader]() -> bool
		{
			if (!Reader.CanRead(2)) return false;
			const uint32 Length = Reader.ReadUInt16();
			const int32 Bytes = Length + ((4 - ((Length + 2) & 3)) & 3);
			if (!Reader.CanRead(Bytes)) return false;
			Reader.Skip(Bytes); return true;
		};
		if ((OptionFlags & 0x80u) && !SkipString()) return;
		if (OptionFlags & 0x100u)
		{
			if (!Reader.CanRead(4)) return;
			const uint32 Qualities = Reader.ReadUInt32();
			if (Qualities & ~0xFu) return;
			for (uint32 Flag : {1u, 2u, 4u, 8u})
			{
				if (!(Qualities & Flag)) continue;
				const int32 Count = ReadTableHeader();
				if (Count < 0) return;
				if (Flag != 8u)
				{
					const int32 Bytes = Count * (Flag == 4u ? 12 : 8);
					if (!Reader.CanRead(Bytes)) return;
					Reader.Skip(Bytes);
				}
				else for (int32 I = 0; I < Count; ++I)
				{
					if (!Reader.CanRead(4)) return;
					Reader.Skip(4);
					if (!SkipString()) return;
				}
			}
		}
		Reader.Align();

		// GameplayOptions is a raw byte[] with no length prefix (System.IO.BinaryWriter).
		// Inventory + equipped lists follow it. Scan for a valid inventory footer.
		auto TryParseInventoryFooter = [this](FACEBinaryReader& R) -> bool
		{
			if (!R.CanRead(4))
			{
				return false;
			}
			const int32 Start = R.Tell();
			const uint32 InvCount = R.ReadUInt32();
			if (InvCount > 200u || !R.CanRead(static_cast<int32>(InvCount) * 8 + 4))
			{
				R.Seek(Start);
				return false;
			}
			TArray<FACEContainerItemRef> Items;
			Items.Reserve(static_cast<int32>(InvCount));
			for (uint32 i = 0; i < InvCount; ++i)
			{
				FACEContainerItemRef Ref;
				Ref.ItemGuid = static_cast<int32>(R.ReadUInt32());
				Ref.ContainerType = static_cast<int32>(R.ReadUInt32());
				if (Ref.ItemGuid == 0 || Ref.ContainerType < 0 || Ref.ContainerType > 2)
				{
					R.Seek(Start);
					return false;
				}
				// Reject GameplayOptions false-positives that claim world props (lifestone/chest).
				if (const FACEWorldObject* Obj = WorldObjects.Find(Ref.ItemGuid))
				{
					const bool bWorldPlaced = Obj->bHasPosition && Obj->ContainerId == 0
						&& Obj->WielderId == 0 && Obj->ParentGuid == 0;
					if (bWorldPlaced)
					{
						R.Seek(Start);
						return false;
					}
				}
				Items.Add(Ref);

			}
			if (!R.CanRead(4))
			{
				R.Seek(Start);
				return false;
			}
			const uint32 EquipCount = R.ReadUInt32();
			if (EquipCount > 100u || !R.CanRead(static_cast<int32>(EquipCount) * 12))
			{
				R.Seek(Start);
				return false;
			}
			TMap<int32, TPair<int64, uint32>> Equipment;
            for (uint32 I = 0; I < EquipCount; ++I)
            {
                const int32 Guid = R.ReadUInt32();
                const int64 Location = R.ReadUInt32();
                const uint32 Priority = R.ReadUInt32();
                if (!Guid || Equipment.Contains(Guid)) { R.Seek(Start); return false; }
                Equipment.Add(Guid, {Location, Priority});
            }
			// Prefer exact consume; allow tiny alignment pad.
			if (R.Remaining() > 3)
			{
				R.Seek(Start);
				return false;
			}
			ContainerContents.FindOrAdd(PlayerGuid) = MoveTemp(Items);
			LoginEquipment = MoveTemp(Equipment);
			ApplyPlayerInventoryProfile();
			return true;
		};

		const int32 TailStart = Reader.Tell();
		const int32 TailBytes = Reader.Remaining();
		bool bInvParsed = false;
		if (!(OptionFlags & OptGameplayOptions))
		{
			bInvParsed = TryParseInventoryFooter(Reader);
		}
		else
		{
			// Skip unknown GameplayOptions blob length by probing inventory start.
			for (int32 Skip = 0; Skip <= TailBytes - 8 && !bInvParsed; ++Skip)
			{
				Reader.Seek(TailStart + Skip);
				bInvParsed = TryParseInventoryFooter(Reader);
			}
		}
		if (!bInvParsed)
		{
			Reader.Seek(TailStart); // leave unread; vitals already applied
		}
	}

	Vitals.RecomputeMaxVitals();
	Vitals.bValid = true;
	Vitals.bHasLuminanceBalance = true;
	PlayerVitals = Vitals;
	bAfkMode=Vitals.QualityBools.FindRef(110);
	PlayerPositionQualities=MoveTemp(PositionQualities);
	LastAttackerGuid = DescriptionAttackerGuid;
	LastAttackerTimeSeconds = 0.0; // A login snapshot is not a new hit.
	Log(FString::Printf(TEXT("PlayerDescription: L%d H%d/%d S%d/%d M%d/%d run=%d skills=%d spells=%d"),
		Vitals.Level, Vitals.Health, Vitals.MaxHealth, Vitals.Stamina, Vitals.MaxStamina,
		Vitals.Mana, Vitals.MaxMana, Vitals.RunSkillCurrent, Vitals.Skills.Num(), KnownSpells.Num()));
	NotifyVitalsChanged();
	OnEnchantmentsChanged.Broadcast();
}

void FACESession::HandlePrivateUpdateAttribute(FACEBinaryReader& Reader)
{
	// byte sequence, attribute enum u32, ranks u32, startingValue u32, xp u32
	if (!Reader.CanRead(17))
	{
		return;
	}
	Reader.ReadUInt8();
	const uint32 Attribute = Reader.ReadUInt32();
	const int32 Ranks = static_cast<int32>(Reader.ReadUInt32());
	const int32 Start = static_cast<int32>(Reader.ReadUInt32());
	const int32 XpSpent = static_cast<int32>(Reader.ReadUInt32());
	const int32 Current = Ranks + Start;

	switch (Attribute)
	{
	case 1:
		PlayerVitals.Strength = Current;
		PlayerVitals.StrengthRanks = Ranks;
		PlayerVitals.StrengthStart = Start;
		PlayerVitals.StrengthXpSpent = XpSpent;
		break;
	case 2:
		PlayerVitals.Endurance = Current;
		PlayerVitals.EnduranceRanks = Ranks;
		PlayerVitals.EnduranceStart = Start;
		PlayerVitals.EnduranceXpSpent = XpSpent;
		break;
	case 3:
		PlayerVitals.Quickness = Current;
		PlayerVitals.QuicknessRanks = Ranks;
		PlayerVitals.QuicknessStart = Start;
		PlayerVitals.QuicknessXpSpent = XpSpent;
		break;
	case 4:
		PlayerVitals.Coordination = Current;
		PlayerVitals.CoordinationRanks = Ranks;
		PlayerVitals.CoordinationStart = Start;
		PlayerVitals.CoordinationXpSpent = XpSpent;
		break;
	case 5:
		PlayerVitals.Focus = Current;
		PlayerVitals.FocusRanks = Ranks;
		PlayerVitals.FocusStart = Start;
		PlayerVitals.FocusXpSpent = XpSpent;
		break;
	case 6:
		PlayerVitals.Self = Current;
		PlayerVitals.SelfRanks = Ranks;
		PlayerVitals.SelfStart = Start;
		PlayerVitals.SelfXpSpent = XpSpent;
		break;
	default: return;
	}
	PlayerVitals.RecomputeMaxVitals();
	NotifyVitalsChanged();
}

void FACESession::HandlePrivateUpdateAttributeLevel(FACEBinaryReader& Reader)
{
	// AttributeCache::SetAttribute(stype, value) changes _init_level only.
	if (!Reader.CanRead(9)) return;
	Reader.ReadUInt8();
	const uint32 Attribute = Reader.ReadUInt32();
	const uint32 Value = Reader.ReadUInt32();
	if (Value > MAX_int32) return;
	const int32 Start = static_cast<int32>(Value);
	switch (Attribute)
	{
	case 1: PlayerVitals.StrengthStart = Start; PlayerVitals.Strength = Start + PlayerVitals.StrengthRanks; break;
	case 2: PlayerVitals.EnduranceStart = Start; PlayerVitals.Endurance = Start + PlayerVitals.EnduranceRanks; break;
	case 3: PlayerVitals.QuicknessStart = Start; PlayerVitals.Quickness = Start + PlayerVitals.QuicknessRanks; break;
	case 4: PlayerVitals.CoordinationStart = Start; PlayerVitals.Coordination = Start + PlayerVitals.CoordinationRanks; break;
	case 5: PlayerVitals.FocusStart = Start; PlayerVitals.Focus = Start + PlayerVitals.FocusRanks; break;
	case 6: PlayerVitals.SelfStart = Start; PlayerVitals.Self = Start + PlayerVitals.SelfRanks; break;
	default: return;
	}
	PlayerVitals.RecomputeMaxVitals();
	NotifyVitalsChanged();
}

void FACESession::HandlePrivateUpdateSkill(FACEBinaryReader& Reader)
{
	// byte sequence, skillId u32, ranks u16, adjustPP u16, sac u32, xp u32, init u32,
	// resistance u32, lastUsed f64 — matches GameMessagePrivateUpdateSkill / PlayerDescription.
	if (!Reader.CanRead(33))
	{
		return;
	}
	Reader.ReadUInt8();
	const int32 SkillId = static_cast<int32>(Reader.ReadUInt32());
	const int32 Ranks = static_cast<int32>(Reader.ReadUInt16());
	Reader.ReadUInt16(); // adjustPP
	const int32 Sac = static_cast<int32>(Reader.ReadUInt32());
	const int32 XpSpent = static_cast<int32>(Reader.ReadUInt32());
	const int32 InitLevel = static_cast<int32>(Reader.ReadUInt32());
	Reader.ReadUInt32(); // resistance
	Reader.ReadDouble(); // lastUsed

	FACESkillInfo& Skill = FindOrAddPlayerSkill(SkillId);
	Skill.Ranks = Ranks;
	Skill.AdvancementClass = Sac;
	Skill.XpSpent = XpSpent;
	Skill.InitLevel = InitLevel;
	// An explicit full update supersedes a deferred compact SAC calculation.
	Skill.bRecomputeRanksFromXp = false;
	UpdatePlayerSkillFallback(Skill);
	NotifyVitalsChanged();
}

void FACESession::HandlePrivateUpdateSkillValue(FACEBinaryReader& Reader, bool bAdvancementClass)
{
	if (!Reader.CanRead(9)) return;
	Reader.ReadUInt8();
	const uint32 SkillId = Reader.ReadUInt32();
	const uint32 Value = Reader.ReadUInt32();
	if (!SkillId || SkillId > MAX_int32 || Value > (bAdvancementClass ? 3u : uint32(MAX_int32))) return;
	FACESkillInfo& Skill = FindOrAddPlayerSkill(static_cast<int32>(SkillId));
	if (bAdvancementClass)
	{
		Skill.AdvancementClass = static_cast<int32>(Value);
		// Skill::SetSkillAdvancementClass retains XP/init, but changes the XP curve.
		Skill.bRecomputeRanksFromXp = Value >= 2;
		if (Value < 2) Skill.Ranks = 0;
	}
	else
	{
		Skill.InitLevel = static_cast<int32>(Value);
	}
	UpdatePlayerSkillFallback(Skill);
	NotifyVitalsChanged();
}

FACESkillInfo& FACESession::FindOrAddPlayerSkill(int32 SkillId)
{
	FACESkillInfo* Found = nullptr;
	for (FACESkillInfo& Sk : PlayerVitals.Skills)
	{
		if (Sk.SkillId == SkillId)
		{
			Found = &Sk;
			break;
		}
	}
	if (!Found)
	{
		FACESkillInfo& Added = PlayerVitals.Skills.AddDefaulted_GetRef();
		Added.SkillId = SkillId;
		static const TCHAR* Names[] = {
			TEXT("None"), TEXT("Axe"), TEXT("Bow"), TEXT("Crossbow"), TEXT("Dagger"), TEXT("Mace"),
			TEXT("Melee Defense"), TEXT("Missile Defense"), TEXT("Sling"), TEXT("Spear"), TEXT("Staff"),
			TEXT("Sword"), TEXT("Thrown Weapon"), TEXT("Unarmed Combat"), TEXT("Arcane Lore"),
			TEXT("Magic Defense"), TEXT("Mana Conversion"), TEXT("Spellcraft"), TEXT("Item Tinkering"),
			TEXT("Assess Person"), TEXT("Deception"), TEXT("Healing"), TEXT("Jump"), TEXT("Lockpick"),
			TEXT("Run"), TEXT("Awareness"), TEXT("Arms and Armor Repair"), TEXT("Assess Creature"),
			TEXT("Weapon Tinkering"), TEXT("Armor Tinkering"), TEXT("Magic Item Tinkering"),
			TEXT("Creature Enchantment"), TEXT("Item Enchantment"), TEXT("Life Magic"), TEXT("War Magic"),
			TEXT("Leadership"), TEXT("Loyalty"), TEXT("Fletching"), TEXT("Alchemy"), TEXT("Cooking"),
			TEXT("Salvaging"), TEXT("Two Handed Combat"), TEXT("Gearcraft"), TEXT("Void Magic"),
			TEXT("Heavy Weapons"), TEXT("Light Weapons"), TEXT("Finesse Weapons"), TEXT("Missile Weapons"),
			TEXT("Shield"), TEXT("Dual Wield"), TEXT("Recklessness"), TEXT("Sneak Attack"),
			TEXT("Dirty Fighting"), TEXT("Challenge"), TEXT("Summoning")
		};
		Added.Name = (SkillId >= 0 && SkillId < static_cast<int32>(UE_ARRAY_COUNT(Names)))
			? FString(Names[SkillId])
			: FString::Printf(TEXT("Skill %d"), SkillId);
		Found = &Added;
	}
	return *Found;
}

void FACESession::UpdatePlayerSkillFallback(FACESkillInfo& Skill)
{
	Skill.Current = Skill.InitLevel + Skill.Ranks;
	switch (Skill.SkillId)
	{
	case 6:  Skill.Current += (PlayerVitals.Coordination + PlayerVitals.Quickness) / 3; break;
	case 7:  Skill.Current += (PlayerVitals.Coordination + PlayerVitals.Quickness) / 5; break;
	case 15: Skill.Current += (PlayerVitals.Focus + PlayerVitals.Self) / 7; break;
	case 22: Skill.Current += (PlayerVitals.Strength + PlayerVitals.Coordination) / 2; break;
	case 24:
		Skill.Current += PlayerVitals.Quickness;
		PlayerVitals.RunSkillCurrent = Skill.Current;
		break;
	default: break;
	}
	Skill.Base = Skill.Current;
	if (Skill.SkillId == 22) PlayerVitals.JumpSkillCurrent = Skill.Current;
}

void FACESession::HandlePrivateUpdateVital(FACEBinaryReader& Reader)
{
	// byte sequence, vital (PropertyAttribute2nd Max*) u32, ranks u32, start u32, xp u32, current u32
	if (!Reader.CanRead(21))
	{
		return;
	}
	Reader.ReadUInt8();
	const uint32 Vital = Reader.ReadUInt32();
	const int32 Ranks = static_cast<int32>(Reader.ReadUInt32());
	const int32 Start = static_cast<int32>(Reader.ReadUInt32());
	const int32 XpSpent = static_cast<int32>(Reader.ReadUInt32());
	const int32 Current = static_cast<int32>(Reader.ReadUInt32());

	switch (Vital)
	{
	case 1: // PropertyAttribute2nd.MaxHealth — ranks/start/xp + current HP
		PlayerVitals.HealthRanks = Ranks;
		PlayerVitals.HealthStart = Start;
		PlayerVitals.HealthXpSpent = XpSpent;
		PlayerVitals.Health = Current;
		break;
	case 2: // PropertyAttribute2nd.Health / Vital.Health — current only
		PlayerVitals.Health = Current;
		break;
	case 3: // MaxStamina
		PlayerVitals.StaminaRanks = Ranks;
		PlayerVitals.StaminaStart = Start;
		PlayerVitals.StaminaXpSpent = XpSpent;
		PlayerVitals.Stamina = Current;
		break;
	case 4: // Stamina
		PlayerVitals.Stamina = Current;
		break;
	case 5: // MaxMana
		PlayerVitals.ManaRanks = Ranks;
		PlayerVitals.ManaStart = Start;
		PlayerVitals.ManaXpSpent = XpSpent;
		PlayerVitals.Mana = Current;
		break;
	case 6: // Mana
		PlayerVitals.Mana = Current;
		break;
	default: return;
	}
	PlayerVitals.RecomputeMaxVitals();
	// Current can briefly exceed unbuffed Max while enchantments are pending — keep UI sane.
	PlayerVitals.MaxHealth = FMath::Max(PlayerVitals.MaxHealth, PlayerVitals.Health);
	PlayerVitals.MaxStamina = FMath::Max(PlayerVitals.MaxStamina, PlayerVitals.Stamina);
	PlayerVitals.MaxMana = FMath::Max(PlayerVitals.MaxMana, PlayerVitals.Mana);
	PlayerVitals.bValid = true;
	NotifyVitalsChanged();
}

void FACESession::HandlePrivateUpdateAttribute2ndLevel(FACEBinaryReader& Reader)
{
	// Odd IDs change _init_level; even IDs change _current_level (AttributeCache).
	if (!Reader.CanRead(9))
	{
		return;
	}
	Reader.ReadUInt8();
	const uint32 Vital = Reader.ReadUInt32();
	const uint32 Value = Reader.ReadUInt32();
	if (Value > MAX_int32) return;
	const int32 Current = static_cast<int32>(Value);

	switch (Vital)
	{
	case 1: PlayerVitals.HealthStart = Current; break;
	case 2: PlayerVitals.Health = Current; break;
	case 3: PlayerVitals.StaminaStart = Current; break;
	case 4: PlayerVitals.Stamina = Current; break;
	case 5: PlayerVitals.ManaStart = Current; break;
	case 6: PlayerVitals.Mana = Current; break;
	default: return;
	}
	PlayerVitals.RecomputeMaxVitals();
	PlayerVitals.MaxHealth = FMath::Max(PlayerVitals.MaxHealth, PlayerVitals.Health);
	PlayerVitals.MaxStamina = FMath::Max(PlayerVitals.MaxStamina, PlayerVitals.Stamina);
	PlayerVitals.MaxMana = FMath::Max(PlayerVitals.MaxMana, PlayerVitals.Mana);
	PlayerVitals.bValid = true;
	NotifyVitalsChanged();
}

void FACESession::HandlePositionQuality(FACEBinaryReader& Reader, bool bPublic)
{
	if (!Reader.CanRead(bPublic?41:37)) return;
	Reader.ReadUInt8(); // accepted by the shared property sequence gate
	const int32 Guid=bPublic?Reader.ReadInt32():PlayerGuid;
	const uint32 Key=Reader.ReadUInt32();
	FACEPosition P;P.CellId=Reader.ReadUInt32();
	P.Location.X=Reader.ReadFloat();P.Location.Y=Reader.ReadFloat();P.Location.Z=Reader.ReadFloat();
	P.RotationW=Reader.ReadFloat();
	P.RotationXYZ.X=Reader.ReadFloat();P.RotationXYZ.Y=Reader.ReadFloat();P.RotationXYZ.Z=Reader.ReadFloat();
	if (Guid==PlayerGuid) PlayerPositionQualities.Add(Key,P);
}

void FACESession::HandlePrivateUpdatePropertyInt(FACEBinaryReader& Reader)
{
	// byte sequence, property key u32, value i32
	if (!Reader.CanRead(9))
	{
		return;
	}
	Reader.ReadUInt8();
	const uint32 Key = Reader.ReadUInt32();
	const int32 Value = Reader.ReadInt32();
	ApplyPlayerIntQuality(Key, Value);
}

void FACESession::ApplyPlayerIntQuality(uint32 Key, int32 Value)
{
	PlayerVitals.StatQualityInts.Add(static_cast<int32>(Key), Value);
	if (Key == 25) // Level
	{
		PlayerVitals.Level = Value;
		NotifyVitalsChanged();
	}
	else if (Key == 24) // AvailableSkillCredits
	{
		PlayerVitals.AvailableSkillCredits = Value;
		NotifyVitalsChanged();
	}
	else if (Key == 134) // PlayerKillerStatus
	{
		ApplyPlayerKillerStatus(PlayerGuid, Value);
	}
	else if (Key == 181) // Server-authoritative chess rating, including losses.
	{
		PlayerVitals.ChessRank = Value;
		NotifyVitalsChanged();
	}
	else if (Key == 188) // Heritage can change on custom servers without relogging.
	{
		PlayerVitals.HeritageGroup = Value;
		NotifyVitalsChanged();
	}
	else if (Key == 113) { PlayerVitals.Gender = Value; NotifyVitalsChanged(); }
	else if (Key == 125) { PlayerVitals.AgeSeconds = Value; NotifyVitalsChanged(); }
	else if (Key == 43) { PlayerVitals.NumDeaths = Value; NotifyVitalsChanged(); }
	else if (Key == 192) { PlayerVitals.FishingSkill = Value; NotifyVitalsChanged(); }
	else if (Key == 40) // PropertyInt.CombatMode
	{
		ApplyServerCombatMode(Value);
	}
	else if (Key == 5) // PropertyInt.EncumbranceVal
	{
		bHasPlayerEncumbrance = true;
		PlayerEncumbranceVal = Value;
		if (FACEWorldObject* Self = WorldObjects.Find(PlayerGuid))
		{
			Self->Burden = Value;
		}
	}
	else if (Key == 322) { PlayerVitals.AetheriaUnlocked = Value & 7; NotifyVitalsChanged(); }
	else if (Key == 230) // PropertyInt.AugmentationIncreasedCarryingCapacity
	{
		PlayerVitals.CarryingCapacityAugs = FMath::Clamp(Value, 0, 5);
		NotifyVitalsChanged();
	}
	else if (Key == 129) // PropertyInt.VitaeCpPool
	{
		VitaeCpPool = Value;
		OnEnchantmentsChanged.Broadcast();
	}
	else if (Key == 139) // PropertyInt.DeathLevel
	{
		DeathLevel = Value;
		OnEnchantmentsChanged.Broadcast();
	}
	else
	{
		// Equipment ratings and custom stat qualities can change without an
		// attribute packet. Re-evaluate the shared desktop/VR vitals immediately.
		NotifyVitalsChanged();
	}
}

void FACESession::ApplyServerCombatMode(int32 Mode)
{
	PlayerVitals.StatQualityInts.Add(40, Mode);
	++CombatModeRevision;
	const double Now = FPlatformTime::Seconds();
	if (PendingCombatMode != 0 && Now < PendingCombatModeUntil)
	{
		if (Mode == static_cast<int32>(PendingCombatMode))
		{
			PendingCombatMode = 0;
			DeferredServerCombatMode = INDEX_NONE;
			PlayerVitals.CombatMode = Mode;
			NotifyVitalsChanged();
			return;
		}
		// SwitchCombatStyles briefly publishes NonCombat — keep optimistic mode so
		// MoveToState / UI do not flash peace (casting recovers via LastCombatMode).
		if (Mode == static_cast<int32>(ACECombatMode::NonCombat)
			&& PendingCombatMode != ACECombatMode::NonCombat)
		{
			DeferredServerCombatMode = Mode;
			return;
		}
	}

	PendingCombatMode = 0;
	DeferredServerCombatMode = INDEX_NONE;
	if (PlayerVitals.CombatMode == Mode)
	{
		return;
	}
	PlayerVitals.CombatMode = Mode;
	NotifyVitalsChanged();
}

void FACESession::HandlePrivateUpdatePropertyInt64(FACEBinaryReader& Reader, bool bPublic)
{
	// Public numeric updates insert the object GUID before the property key.
	if (!Reader.CanRead(bPublic ? 17 : 13)) return;
	Reader.ReadUInt8();
	const int32 Guid = bPublic ? static_cast<int32>(Reader.ReadUInt32()) : PlayerGuid;
	const int32 Key = static_cast<int32>(Reader.ReadUInt32());
	const int64 Value = static_cast<int64>(Reader.ReadUInt64());
	if (Guid != PlayerGuid) return;
	PlayerVitals.QualityInt64s.Add(Key, Value);
	if (Key == 1) PlayerVitals.TotalExperience = Value;
	else if (Key == 2) PlayerVitals.AvailableExperience = Value;
	else if (Key == 6) { PlayerVitals.AvailableLuminance = Value; PlayerVitals.bHasLuminanceBalance = true; }
	else if (Key == 7) PlayerVitals.MaximumLuminance = Value;
	NotifyVitalsChanged();
}

void FACESession::HandleUpdatePropertyFloat(FACEBinaryReader& Reader, bool bPublic)
{
	if (!Reader.CanRead(bPublic ? 17 : 13)) return;
	Reader.ReadUInt8();
	const int32 Guid = bPublic ? static_cast<int32>(Reader.ReadUInt32()) : PlayerGuid;
	const int32 Key = static_cast<int32>(Reader.ReadUInt32());
	const double Value = Reader.ReadDouble();
	if (!FMath::IsFinite(Value)) return;
	if (Key == 167 && Value >= 0.0) // PropertyFloat.CooldownDuration
	{
		if (FACEWorldObject* Object = WorldObjects.Find(Guid)) Object->CooldownDuration = Value;
	}
	if (Guid != PlayerGuid) return;
	PlayerVitals.QualityDoubles.Add(Key, Value);
	NotifyVitalsChanged();
}

bool FACESession::ReadStringPropertyHeader(FACEBinaryReader& Reader, bool bPublic, int32& Guid, uint32& Property) const
{
	if (!Reader.CanRead(bPublic ? 8 : 4)) return false;
	Property = Reader.ReadUInt32();
	Guid = PlayerGuid;
	if (bPublic)
	{
		const uint32 Second = Reader.ReadUInt32();
		// Retail UIQueueManager sends GUID, property; ACE's public string writer
		// sends property, GUID. Use known identity, not a property-ID range (custom
		// servers use high keys). Prefer retail if neither/both words are owners.
		const auto IsOwner = [this](uint32 Id)
		{
			return Id != 0 && (int32(Id) == PlayerGuid || WorldObjects.Contains(int32(Id)));
		};
		if (!IsOwner(Property) && IsOwner(Second)) Guid = int32(Second);
		else { Guid = int32(Property); Property = Second; }
	}
	Reader.Align();
	if (!Reader.CanRead(2)) return false;
	const int32 StringStart = Reader.Tell();
	const uint16 Length = Reader.ReadUInt16();
	// A partial payload must not consume the property's timestamp, including
	// String16L's trailing alignment bytes.
	if (!Reader.CanRead(Length + ((4 - ((2 + Length) & 3)) & 3))) return false;
	Reader.Seek(StringStart);
	return Guid != 0;
}

void FACESession::HandleUpdatePropertyString(FACEBinaryReader& Reader, bool bPublic)
{
	if (!Reader.CanRead(1)) return;
	Reader.ReadUInt8();
	int32 Guid;
	uint32 Key;
	if (!ReadStringPropertyHeader(Reader, bPublic, Guid, Key)) return;
	const FString Value = Reader.ReadString16L();
	if (Guid != PlayerGuid) return;
	PlayerVitals.QualityStrings.Add(Key, Value);
	if (Key == 5) PlayerVitals.TemplateName = Value;
	NotifyVitalsChanged();
}

void FACESession::SendTalk(const FString& Message)
{
	if (State != EACESessionState::InWorld || Message.IsEmpty())
	{
		return;
	}
	// Server/custom commands can use RequestedAppraisalTarget OR the last
	// successful CurrentAppraisalTarget. Silent plugin inventory scans change
	// both on the server. Restore the user's appraisal immediately before Talk,
	// on the same ordered WeenieQueue; no command-name whitelist or round trip.
	if (Message.StartsWith(TEXT("@")) || Message.StartsWith(TEXT("/")))
		RestoreManualAppraisalContext();
	FACEBinaryWriter W;
	W.WriteString16L(Message);
	SendGameAction(ACEGameAction::Talk, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendChatChannel(uint32 ChannelId, const FString& Message)
{
	if (State != EACESessionState::InWorld || Message.IsEmpty() || ChannelId == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(ChannelId);
	W.WriteString16L(Message);
	SendGameAction(ACEGameAction::ChatChannel, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SetSelectedObjectInternal(const FACESelectedObject& Sel, bool bInventorySelection)
{
	// Preserve retail's ordinary selection history separately. Inventory tabs,
	// packs and automatic vendor-stock selection can all intervene before Give.
	if (SelectedObject.Guid != Sel.Guid) PreviousSelectedObjectGuid = SelectedObject.Guid;
	const auto* Object = WorldObjects.Find(Sel.Guid);
	const bool bBrowsingItems = Object && (Object->ContainerId || Object->WielderId || Object->ParentGuid
		|| (bInventorySelection && Sel.Guid == PlayerGuid));
	if (!bBrowsingItems)
	{
		GiveRecipientGuid = Object && Sel.Guid != PlayerGuid && Object->IsSelectableWorldObject()
			&& !Object->bDying && Object->IsGiveOrCreatureTarget() ? Sel.Guid : 0;
	}
	const uint32 Serial = SelectedObject.SelectionSerial + 1;
	SelectedObject = Sel;
	SelectedObject.SelectionSerial = Serial;
	OnSelectionChanged.Broadcast(SelectedObject);
}

bool FACESession::CanQueryObjectHealth(int32 Guid) const
{
	const FACEWorldObject* Object = WorldObjects.Find(Guid);
	if (!Object || Object->StackSize > 1) return false;
	if (Guid == PlayerGuid || Object->bIsPlayer || Object->PetOwnerId) return true;
	if (!(Object->ItemType & ACEItemType::Creature)) return false;
	const auto* Player = WorldObjects.Find(PlayerGuid);
	return Object->IsAttackable() || (Object->ObjectDescriptionFlags & ACEObjectDescFlag::FreePkStatus)
		|| (Player && (Player->ObjectDescriptionFlags & ACEObjectDescFlag::FreePkStatus));
}

bool FACESession::IsCombatTarget(int32 Guid) const
{
	const auto* Target = WorldObjects.Find(Guid);
	return Target && Guid != PlayerGuid && ACECombatTargeting::CanAttack(*Target, WorldObjects.Find(PlayerGuid));
}

bool FACESession::CanQueryItemMana(int32 Guid) const
{
	const FACEWorldObject* Item = WorldObjects.Find(Guid);
	if (!Item || Guid == PlayerGuid || Item->StackSize > 1 || CanQueryObjectHealth(Guid)) return false;
	// IsOwnedByPlayer includes equipped items and items inside owned packs.
	TSet<int32> Seen;
	while (Item && !Seen.Contains(Item->Guid))
	{
		Seen.Add(Item->Guid);
		const int32 Owner = Item->WielderId ? Item->WielderId : Item->ContainerId ? Item->ContainerId : Item->ParentGuid;
		if (!Owner) return false;
		if (Owner == PlayerGuid) return true;
		Item = WorldObjects.Find(Owner);
	}
	return false;
}

void FACESession::SelectObject(int32 ObjectGuid, bool bInventorySelection)
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}

	FACESelectedObject Sel;
	int32 HealthTarget = 0;
	if (ObjectGuid != 0)
	{
		FACEWorldObject Obj;
		if (GetWorldObject(ObjectGuid, Obj))
		{
			// UIHidden weenies remain rendered (pedestals/hooks), but retail never
			// accepts them as a mouse, hotkey or programmatic world selection.
			if (Obj.IsUiHidden() || Obj.IsUnnamedDecoration()) return;
			Sel.Guid = ObjectGuid;
			Sel.Name = Obj.Name;
			Sel.bValid = true;
			// gmToolbarUI: stacks take priority, then player/pet/attackable
			// health, then owned-item mana. Meters appear only after a reply.
			if (CanQueryObjectHealth(ObjectGuid)) HealthTarget = ObjectGuid;
			if (SelectedObject.Guid == ObjectGuid)
			{
				Sel.HealthFraction = SelectedObject.HealthFraction;
				Sel.bShowHealth = HealthTarget && SelectedObject.bShowHealth;
				Sel.ManaFraction = SelectedObject.ManaFraction;
				Sel.bShowMana = CanQueryItemMana(ObjectGuid) && SelectedObject.bShowMana;
			}
		}
		else
		{
			Sel.Guid = ObjectGuid;
			Sel.Name = TEXT("Unknown");
			Sel.bValid = true;
		}
	}
	SetSelectedObjectInternal(Sel, bInventorySelection);

	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(HealthTarget));
	SendGameAction(ACEGameAction::QueryHealth, W.GetData(), ACEQueue::WeenieQueue);
	const int32 ManaTarget = CanQueryItemMana(ObjectGuid) ? ObjectGuid : 0;
	if (ManaTarget || SelectedManaQueryGuid)
	{
		SelectedManaQueryGuid = ManaTarget;
		FACEBinaryWriter Mana;
		Mana.WriteUInt32(static_cast<uint32>(ManaTarget));
		SendGameAction(ACEGameAction::QueryItemMana, Mana.GetData(), ACEQueue::WeenieQueue);
	}
}

void FACESession::RestoreManualAppraisalContext()
{
	if (!bBackgroundAppraisalContext) return;
	// Clear both server targets first. If the user has not appraised anything,
	// the target has disappeared, or the fresh appraisal fails, a command must
	// never fall back to the plugin's last successfully assessed inventory item.
	SendGameActionU32(ACEGameAction::IdentifyObject, 0);
	if (ManualAppraisalGuid && WorldObjects.Contains(ManualAppraisalGuid))
		SendGameActionU32(ACEGameAction::IdentifyObject, uint32(ManualAppraisalGuid));
	bBackgroundAppraisalContext = false;
}

void FACESession::SendIdentifyObject(int32 ObjectGuid, bool bBackground)
{
	if (State != EACESessionState::InWorld || ObjectGuid == 0)
	{
		return;
	}
	if (bBackground)
		bBackgroundAppraisalContext = true;
	else
	{
		// A failed manual appraisal must not leave CurrentAppraisalTarget pointing
		// at an earlier background item either. Do not restore the previous user
		// target here: this request supersedes it.
		if (bBackgroundAppraisalContext) SendGameActionU32(ACEGameAction::IdentifyObject, 0);
		ManualAppraisalGuid = ObjectGuid;
		CursorAppraisalGuid = ObjectGuid;
		CursorAppraisalUntil = FPlatformTime::Seconds()+30.;
		bBackgroundAppraisalContext = false;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(ObjectGuid));
	SendGameAction(ACEGameAction::IdentifyObject, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendRaiseAttribute(uint32 AttributeId, uint32 XpAmount)
{
	if (State != EACESessionState::InWorld || AttributeId < 1 || AttributeId > 6 || XpAmount == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(AttributeId);
	W.WriteUInt32(XpAmount);
	SendGameAction(ACEGameAction::RaiseAttribute, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendRaiseVital(uint32 VitalId, uint32 XpAmount)
{
	// Vital IDs match PropertyAttribute2nd Max*: Health=1, Stamina=3, Mana=5.
	if (State != EACESessionState::InWorld || XpAmount == 0
		|| (VitalId != 1 && VitalId != 3 && VitalId != 5))
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(VitalId);
	W.WriteUInt32(XpAmount);
	SendGameAction(ACEGameAction::RaiseVital, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendRaiseSkill(uint32 SkillId, uint32 XpAmount)
{
	if (State != EACESessionState::InWorld || SkillId == 0 || XpAmount == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(SkillId);
	W.WriteUInt32(XpAmount);
	SendGameAction(ACEGameAction::RaiseSkill, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendTrainSkill(uint32 SkillId, int32 CreditsSpent)
{
	if (State != EACESessionState::InWorld || SkillId == 0 || CreditsSpent <= 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(SkillId);
	W.WriteInt32(CreditsSpent);
	SendGameAction(ACEGameAction::TrainSkill, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendEmote(const FString& EmoteText)
{
	if (State != EACESessionState::InWorld || EmoteText.IsEmpty())
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteString16L(EmoteText);
	SendGameAction(ACEGameAction::Emote, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendSoulEmote(const FString& EmoteText)
{
	if (State != EACESessionState::InWorld || EmoteText.IsEmpty())
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteString16L(EmoteText);
	SendGameAction(ACEGameAction::SoulEmote, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendSoulEmoteMotion(uint32 MotionCommand)
{
	// Point is the animation transition; the client-requestable soul emote is
	// PointState. Older hotkeys and DAT pose names can resolve to the transition.
	if (MotionCommand == 0x13000084u) MotionCommand = 0x430000f0u;
	if (State != EACESessionState::InWorld || MotionCommand == 0 || bLogOffPending)
	{
		return;
	}
	{
		const uint32 Cell = static_cast<uint32>(PlayerPosition.CellId);
		if ((Cell & 0xFFFFu) < 0x0100u && PlayerPosition.Location.Z < -40.f)
		{
			return;
		}
	}

	// RawMotionState::ApplyMotion puts substates in forward_command. They are
	// not stamped actions, and must survive later sidestep/turn-only packets.
	if (MotionCommand & 0x40000000u)
	{
		CurrentForwardCommand = ACEMotion::NormalizeCommand(MotionCommand);
		SendMoveToState(0.f, 0.f, 0.f, false, bAutoPosContact);
		return;
	}
	// PackedFlags: low 11 bits = RawMotionFlags; bits 11+ = CommandListLength.
	constexpr uint32 CmdListLenShift = 11u;
	uint32 PackedFlags = ACERawMotionFlags::CurrentHoldKey | ACERawMotionFlags::CurrentStyle
		| (1u << CmdListLenShift);

	FACEBinaryWriter W;
	W.WriteUInt32(PackedFlags);
	W.WriteUInt32(ACEMotion::HoldKeyNone);
	W.WriteUInt32(CurrentStance != 0 ? CurrentStance : ACEMotion::StanceNonCombat);

	// MotionItem (client autonomous): ushort command, ushort seq|0x8000, float speed.
	W.WriteUInt16(static_cast<uint16>(MotionCommand));
	++MotionActionSeq;
	W.WriteUInt16(static_cast<uint16>((MotionActionSeq & 0x7FFFu) | 0x8000u));
	W.WriteFloat(1.f);

	W.WriteUInt32(static_cast<uint32>(PlayerPosition.CellId));
	W.WriteFloat(PlayerPosition.Location.X);
	W.WriteFloat(PlayerPosition.Location.Y);
	W.WriteFloat(PlayerPosition.Location.Z);
	W.WriteFloat(PlayerPosition.RotationW);
	W.WriteFloat(PlayerPosition.RotationXYZ.X);
	W.WriteFloat(PlayerPosition.RotationXYZ.Y);
	W.WriteFloat(PlayerPosition.RotationXYZ.Z);

	W.WriteUInt16(InstanceSeq);
	W.WriteUInt16(ServerControlSeq);
	W.WriteUInt16(TeleportSeq);
	W.WriteUInt16(ForcePositionSeq);
	W.WriteUInt8(bAutoPosContact ? 0x01 : 0x00);
	W.Align();

	SendGameAction(ACEGameAction::MoveToState, W.GetData(), ACEQueue::WeenieQueue);
	// A gesture is another SendMovementEvent: retain physical contact and
	// restart the periodic position clock just like an ordinary movement send.
	AutoPosTimer = 0.f;
}

void FACESession::SendTell(const FString& TargetName, const FString& Message)
{
	if (State != EACESessionState::InWorld || TargetName.IsEmpty() || Message.IsEmpty())
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteString16L(Message);
	W.WriteString16L(TargetName);
	SendGameAction(ACEGameAction::Tell, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendTalkDirect(int32 TargetGuid, const FString& Message)
{
	if (State != EACESessionState::InWorld || TargetGuid == 0 || Message.IsEmpty())
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteString16L(Message);
	W.WriteUInt32(static_cast<uint32>(TargetGuid));
	SendGameAction(ACEGameAction::TalkDirect, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendTeleToLifestone()
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	SendGameAction(ACEGameAction::TeleToLifestone, {}, ACEQueue::WeenieQueue);
}

void FACESession::SendTeleToMarketplace()
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	SendGameAction(ACEGameAction::TeleToMarketPlace, {}, ACEQueue::WeenieQueue);
}

void FACESession::SendTeleToHouse()
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	SendGameAction(ACEGameAction::TeleToHouse, {}, ACEQueue::WeenieQueue);
}

void FACESession::SendTeleToMansion()
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	SendGameAction(ACEGameAction::TeleToMansion, {}, ACEQueue::WeenieQueue);
}

void FACESession::SendRecallAllegianceHometown()
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	SendGameAction(ACEGameAction::RecallAllegianceHometown, {}, ACEQueue::WeenieQueue);
}

FString FACESession::GetPlayerName() const
{
	if (const FACEWorldObject* Obj = WorldObjects.Find(PlayerGuid))
	{
		return Obj->Name;
	}
	return FString();
}

uint32 FACESession::GetTurbineChannelId(uint32 ChatType) const
{
	switch (ChatType)
	{
	case ACETurbineChat::Allegiance: return TurbineAllegianceChannel;
	case ACETurbineChat::General: return TurbineGeneralChannel;
	case ACETurbineChat::Trade: return TurbineTradeChannel;
	case ACETurbineChat::LFG: return TurbineLfgChannel;
	case ACETurbineChat::Roleplay: return TurbineRoleplayChannel;
	case ACETurbineChat::Society:
	case ACETurbineChat::SocietyCelestialHand:
	case ACETurbineChat::SocietyEldrytchWeb:
	case ACETurbineChat::SocietyRadiantBlood:
		return TurbineSocietyChannel;
	case ACETurbineChat::Olthoi: return TurbineOlthoiChannel;
	default: return TurbineGeneralChannel;
	}
}

void FACESession::SendSimpleGameAction(uint32 ActionType)
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	SendGameAction(ActionType, {}, ACEQueue::WeenieQueue);
}

void FACESession::SendGameActionU32(uint32 ActionType, uint32 Value)
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(Value);
	SendGameAction(ActionType, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendGameActionString(uint32 ActionType, const FString& Value)
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteString16L(Value);
	SendGameAction(ActionType, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendGameActionStringU32(uint32 ActionType, const FString& Value, uint32 Extra)
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteString16L(Value);
	W.WriteUInt32(Extra);
	SendGameAction(ActionType, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendTurbineChat(uint32 ChannelId, uint32 ChatType, const FString& Message)
{
	if (State != EACESessionState::InWorld || Message.IsEmpty())
	{
		return;
	}
	const uint32 ContextId = TurbineChatContextId++;
	if (TurbineChatContextId == 0 || TurbineChatContextId > 0x71u)
	{
		TurbineChatContextId = 1;
	}

	FACEBinaryWriter W;
	const int32 FirstSizePos = W.Num();
	W.WriteUInt32(0);
	W.WriteUInt32(ACETurbineChat::BlobRequestBinary);
	W.WriteUInt32(ACETurbineChat::DispatchById);
	W.WriteUInt32(1);
	W.WriteUInt32(0);
	W.WriteUInt32(0);
	W.WriteUInt32(0);
	W.WriteUInt32(0);
	const int32 SecondSizePos = W.Num();
	W.WriteUInt32(0);
	W.WriteUInt32(ContextId);
	W.WriteUInt32(2);
	W.WriteUInt32(2);
	W.WriteUInt32(ChannelId);
	W.WritePackedUnicode(Message);
	W.WriteUInt32(0x0Cu);
	W.WriteUInt32(static_cast<uint32>(PlayerGuid));
	W.WriteUInt32(0);
	W.WriteUInt32(ChatType);
	W.WriteUInt32At(FirstSizePos, static_cast<uint32>(W.Num() - FirstSizePos - 4));
	W.WriteUInt32At(SecondSizePos, static_cast<uint32>(W.Num() - SecondSizePos - 4));
	SendGameMessage(ACEOpcode::TurbineChat, W.GetData(), ACEQueue::LoginQueue);
}

int32 FACESession::FindContainerMergeTarget(int32 ItemGuid, int32 ContainerGuid, int32 Amount) const
{
	const auto* Item=WorldObjects.Find(ItemGuid);
	const auto* Destination=WorldObjects.Find(ContainerGuid);
	if (!Item || !ContainerGuid || ItemGuid==ContainerGuid || Amount<=0
		|| !Item->WeenieClassId || Item->MaxStackSize<=1
		|| Item->ContainerId==ContainerGuid || Item->WielderId || Item->ParentGuid || Item->CurrentWieldedLocation
		|| Item->StackSize<0 || Amount>FMath::Max(1,Item->StackSize)
		|| (Destination && !ACEInventoryRules::CheckHookStatus(*Item,*Destination))) return 0;
	auto Offered=[&](int32 Guid)
	{
		for(int32 Depth=0;Guid && Guid!=PlayerGuid && Depth<32;++Depth)
		{
			if(TradeSelfItems.Contains(Guid)||TradePartnerItems.Contains(Guid))return true;
			const auto* Object=WorldObjects.Find(Guid);Guid=Object?Object->ContainerId:0;
		}
		return false;
	};
	if(Offered(ItemGuid)||Offered(ContainerGuid))return 0;
	// ItemHolder::AttemptAutoMerge uses the destination's retail item-list order,
	// then its contained packs. Same-bag moves remain deliberate reorders/splits.
	TArray<int32> Containers{ContainerGuid};
	TArray<int32> Packs;GetContainerPackGuids(ContainerGuid,Packs);Containers.Append(Packs);
	for(int32 Container:Containers)
	{
		if(Offered(Container))continue;
		TArray<int32> Items;GetPackItemGuids(Container,Items);
		for(int32 Guid:Items)
		{
			const auto* Other=WorldObjects.Find(Guid);
			if(Other && !Other->WielderId && !Other->ParentGuid && !Other->CurrentWieldedLocation
				&& Other->StackSize>=0 && !Offered(Guid) && ACEInventoryRules::MergeAmount(*Item,*Other)>=Amount)return Guid;
		}
	}
	return 0;
}

void FACESession::SendPutItemInContainer(int32 ItemGuid, int32 ContainerGuid, int32 Placement)
{
	if (State != EACESessionState::InWorld || ItemGuid == 0 || ContainerGuid == 0)
	{
		return;
	}
	// Retail keeps the authoritative IDList until ServerSaysContainID. Updating it
	// optimistically re-applied shifts when acknowledgements arrived after another
	// move, so the displayed order differed from the order saved on the server.
	// Placement is an insertion index, never a sparse grid address (IDList::AddAtNum).
	if (const FACEWorldObject* Item = WorldObjects.Find(ItemGuid))
	{
		// Manual unequip/hotbar use reaches this path without the swap planner.
		// ACE can dequip before rejecting a full bag, then silently restore the
		// item elsewhere without ContainID. Check room before sending that action.
		if (PlayerGuid && Item->CurrentWieldedLocation && Item->WielderId == PlayerGuid
			&& ResolveContainerRoot(ContainerGuid) == PlayerGuid
			&& !CanStowEquipmentInContainer(ContainerGuid))
		{
			const int32 Fallback = ContainerGuid == PlayerGuid ? FindEquipmentStowContainer({}) : 0;
			if (!Fallback)
			{
				EmitChatMessage(TEXT("Not enough room in that pack to put away this equipment."), TEXT(""), ACEChatMessageType::TransientInfo);
				return;
			}
			ContainerGuid = Fallback;
			Placement = 0;
		}
		const int32 Amount=FMath::Max(1,Item->StackSize);
		if(const int32 MergeTo=FindContainerMergeTarget(ItemGuid,ContainerGuid,Amount))
		{SendStackableMerge(ItemGuid,MergeTo,Amount);return;}
		int32 Count = 0;
		for (const auto& Pair : WorldObjects)
		{
			const auto& Other = Pair.Value;
			if (Other.Guid != ItemGuid && Other.ContainerId == ContainerGuid
				&& IsPackSlotItem(Other) == IsPackSlotItem(*Item)) ++Count;
		}
		Placement = FMath::Clamp(Placement, 0, Count);
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(ItemGuid));
	W.WriteUInt32(static_cast<uint32>(ContainerGuid));
	W.WriteInt32(Placement);
	SendGameAction(ACEGameAction::PutItemInContainer, W.GetData(), ACEQueue::WeenieQueue);
	PendingInventoryTransfers.Add(ItemGuid,FPlatformTime::Seconds()+5.);
}

void FACESession::SendStackableMerge(int32 MergeFromGuid, int32 MergeToGuid, int32 Amount)
{
	if (State != EACESessionState::InWorld || MergeFromGuid == 0 || MergeToGuid == 0 || Amount <= 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(MergeFromGuid));
	W.WriteUInt32(static_cast<uint32>(MergeToGuid));
	W.WriteInt32(Amount);
	SendGameAction(ACEGameAction::StackableMerge, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendStackableSplitToContainer(int32 StackGuid, int32 ContainerGuid, int32 Placement, int32 Amount)
{
	if (State != EACESessionState::InWorld || StackGuid == 0 || ContainerGuid == 0 || Amount <= 0)
	{
		return;
	}
	TArray<FACEWorldObject> Destination;
	GetPackItems(ContainerGuid, Destination);
	Placement = FMath::Clamp(Placement, 0, Destination.Num());
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(StackGuid));
	W.WriteUInt32(static_cast<uint32>(ContainerGuid));
	W.WriteInt32(Placement);
	W.WriteInt32(Amount);
	SendGameAction(ACEGameAction::StackableSplitToContainer, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendStackableSplitTo3D(int32 StackGuid, int32 Amount)
{
	if (State != EACESessionState::InWorld || StackGuid == 0 || Amount <= 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(StackGuid));
	W.WriteInt32(Amount);
	SendGameAction(ACEGameAction::StackableSplitTo3D, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendStackableSplitToWield(int32 StackGuid, int64 WieldLocation, int32 Amount)
{
	if (State != EACESessionState::InWorld || StackGuid == 0 || WieldLocation == 0 || Amount <= 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(StackGuid));
	W.WriteInt32(static_cast<int32>(WieldLocation));
	W.WriteInt32(Amount);
	SendGameAction(ACEGameAction::StackableSplitToWield, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendUseWithTarget(int32 SourceGuid, int32 TargetGuid)
{
	if (State != EACESessionState::InWorld || SourceGuid == 0 || TargetGuid == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(SourceGuid));
	W.WriteUInt32(static_cast<uint32>(TargetGuid));
	SendGameAction(ACEGameAction::UseWithTarget, W.GetData(), ACEQueue::WeenieQueue);
	BeginPendingUse(SourceGuid, TargetGuid);
	Log(FString::Printf(TEXT("UseWithTarget src=0x%08X tgt=0x%08X"), SourceGuid, TargetGuid));
}

void FACESession::SendSetTitle(uint32 TitleId)
{
	if (State != EACESessionState::InWorld || TitleId == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(TitleId);
	SendGameAction(ACEGameAction::TitleSet, W.GetData(), ACEQueue::WeenieQueue);
}

bool FACESession::IsCharacterOptionSet(int32 Option) const
{
	const FACECharacterOptionDesc* Desc = ACECharacterOptions::Find(Option);
	if (!Desc)
	{
		return false;
	}
	const uint32 Bits = Desc->bInOptions2 ? CharacterOptions2 : CharacterOptions1;
	return (Bits & Desc->Flag) != 0;
}

void FACESession::SendSetSingleCharacterOption(int32 Option, bool bValue)
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	if (const FACECharacterOptionDesc* Desc = ACECharacterOptions::Find(Option))
	{
		uint32& Bits = Desc->bInOptions2 ? CharacterOptions2 : CharacterOptions1;
		if (bValue) { Bits |= Desc->Flag; } else { Bits &= ~Desc->Flag; }
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(Option));
	W.WriteUInt32(bValue ? 1u : 0u);
	SendGameAction(ACEGameAction::SetSingleCharacterOption, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendCharacterOptions(uint32 Options1, uint32 Options2)
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	// The individual appearance options trigger ACE's ObjDesc broadcast. The
	// bulk preferences packet alone changes the flags without rebuilding gear.
	for (int32 Option : {0x2F, 0x32})
		if (const auto* Desc = ACECharacterOptions::Find(Option))
		{
			const bool Value = ((Desc->bInOptions2 ? Options2 : Options1) & Desc->Flag) != 0;
			if (Value != IsCharacterOptionSet(Option)) SendSetSingleCharacterOption(Option, Value);
		}
	CharacterOptions1 = Options1;
	CharacterOptions2 = Options2;
	// Preserve the server's spell filters when applying other character options.
	FACEBinaryWriter W;
	W.WriteUInt32(0x00000060u);
	W.WriteUInt32(Options1);
	W.WriteUInt32(0);
	W.WriteUInt32(SpellbookFilters);
	W.WriteUInt32(Options2);
	SendGameAction(ACEGameAction::SetCharacterOptions, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendSpellbookFilters(uint32 Filters)
{
	if (State != EACESessionState::InWorld || SpellbookFilters == Filters) return;
	SpellbookFilters = Filters;
	FACEBinaryWriter W;
	W.WriteUInt32(Filters);
	// CM_Character::Event_SpellbookFilterEvent, also handled by ACE and GDLE.
	SendGameAction(0x0286u, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::CancelEquipmentSwap()
{
	PendingEquipmentGuid = 0;
	PendingEquipmentCombatMode = 0;
	bPendingEquipmentWieldSent = false;
	PendingEquipmentLocation = 0;
	PendingEquipmentRemovals.Reset();
	PendingEquipmentUntil = 0.0;
}

void FACESession::AdvanceEquipmentSwap()
{
	if (!PendingEquipmentGuid || bPendingEquipmentWieldSent || State != EACESessionState::InWorld) return;
	PendingEquipmentUntil = FPlatformTime::Seconds() + 15.0;
	if (!PendingEquipmentRemovals.IsEmpty())
	{
        // Retail ItemHolder::AttemptToPlaceInContainer checks main pack, then
        // side packs in inventory order before issuing the dequip. Sending a
        // full destination can make ACE dequip first, then restore into another
        // pack without a ContainID in its failure path (invisible until relog).
        const int32 Destination=FindEquipmentStowContainer({});
        if(!Destination)
        {
            CancelEquipmentSwap();
            EmitChatMessage(TEXT("Not enough room in your packs to change equipment."),TEXT(""),ACEChatMessageType::TransientInfo);
            return;
        }
		SendPutItemInContainer(PendingEquipmentRemovals[0], Destination, 0);
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(PendingEquipmentGuid));
	W.WriteInt32(static_cast<int32>(PendingEquipmentLocation));
	bPendingEquipmentWieldSent = true;
	SendGameAction(ACEGameAction::GetAndWieldItem, W.GetData(), ACEQueue::WeenieQueue);
}

int32 FACESession::FindEquipmentStowContainer(const TMap<int32, int32>& Reserved) const
{
    TArray<int32> Packs;GetPlayerPackGuids(Packs);Packs.Insert(PlayerGuid,0);
    for(int32 Container:Packs)
        if (CanStowEquipmentInContainer(Container, Reserved.FindRef(Container))) return Container;
    return 0;
}

bool FACESession::CanStowEquipmentInContainer(int32 Container, int32 Reserved) const
{
    if (!PlayerGuid || !Container) return false;
    const auto* Pack=WorldObjects.Find(Container);
    if(Container!=PlayerGuid && (!Pack || ResolveContainerRoot(Container)!=PlayerGuid
        || TradeSelfItems.Contains(Container) || TradePartnerItems.Contains(Container)))return false;
    const int32 Capacity=Container==PlayerGuid && (!Pack || Pack->ItemsCapacity==0)?102:Pack->ItemsCapacity;
    if(Capacity==-1)return true;
    if(Capacity<=0)return false; // foci occupy pack slots but cannot store equipment
    TSet<int32> Items;
    if(const auto* List=ContainerContents.Find(Container))for(const auto& Ref:*List)
    {
        const auto* Item=WorldObjects.Find(Ref.ItemGuid);
        if(Ref.ContainerType==0 && (!Item || (!IsPackSlotItem(*Item)
            && (!Item->ContainerId || Item->ContainerId==Container))))Items.Add(Ref.ItemGuid);
    }
    // Include ObjectCreate entries even before a contents list arrives, and
    // reserve unknown list entries so a late description cannot overfill it.
    for(const auto& Pair:WorldObjects)
    {
        const auto& Item=Pair.Value;
        // Early attachment does not free the source slot: WieldItem does.
        if(Item.ContainerId==Container && !IsPackSlotItem(Item))Items.Add(Item.Guid);
    }
    return Items.Num()+Reserved<Capacity;
}

void FACESession::SendGetAndWieldItem(int32 Guid, int64 Loc)
{
	if (State != EACESessionState::InWorld || !Guid || !Loc || PendingEquipmentGuid) return;
	FACEWorldObject Obj;
	GetWorldObject(Guid, Obj);
	TArray<FACEWorldObject> Equipped;
	GetEquippedItems(Equipped);
	PendingEquipmentGuid = Guid;
	PendingEquipmentLocation = Loc;
	PendingEquipmentCombatMode = PlayerVitals.CombatMode;
	if (PendingEquipmentCombatMode != 0 && PendingEquipmentCombatMode != ACECombatMode::NonCombat)
	{
		if (Loc & ACEEquipMask::Held) PendingEquipmentCombatMode = ACECombatMode::Magic;
		else if (Loc & ACEEquipMask::MissileWeapon) PendingEquipmentCombatMode = ACECombatMode::Missile;
		else if (Loc & (ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded)) PendingEquipmentCombatMode = ACECombatMode::Melee;
	}
	constexpr int64 WeaponHand = ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded
		| ACEEquipMask::MissileWeapon | ACEEquipMask::Held;
	constexpr int64 Jewelry = ACEEquipMask::NeckWear | ACEEquipMask::WristWearLeft
		| ACEEquipMask::WristWearRight | ACEEquipMask::FingerWearLeft | ACEEquipMask::FingerWearRight
		| ACEEquipMask::TrinketOne | ACEEquipMask::Cloak | ACEEquipMask::SigilOne
		| ACEEquipMask::SigilTwo | ACEEquipMask::SigilThree;
	const bool bWeaponHand = (Loc & WeaponHand) != 0;
	const uint32 NewCov = ACEInferClothingPriority(Loc, Obj.ItemType);
	const bool bClothingOrArmor = NewCov != 0;

	for (const FACEWorldObject& Eq : Equipped)
	{
		if (Eq.Guid == Guid || Eq.CurrentWieldedLocation == 0)
		{
			continue;
		}
		const int64 EqLoc = Eq.CurrentWieldedLocation;
		bool bConflict = false;
		if ((Loc & ACEEquipMask::MissileWeapon) != 0 && Obj.AmmoType != 0
			&& (EqLoc & ACEEquipMask::MissileAmmo) != 0 && (Eq.AmmoType & Obj.AmmoType) == 0)
		{
			// Launchers share a slot, ammunition does not. Stow incompatible
			// arrows/quarrels/darts and await ContainID before wielding the new
			// launcher, just as for the old weapon. All callers use this path.
			bConflict = true;
		}
		else if ((Loc == ACEEquipMask::Shield && ((EqLoc & (ACEEquipMask::Held | ACEEquipMask::TwoHanded)) != 0
			|| ((EqLoc & ACEEquipMask::MissileWeapon) != 0 && Eq.AmmoType != 0)))
			|| ((EqLoc & ACEEquipMask::Shield) != 0 && ((Loc & (ACEEquipMask::Held | ACEEquipMask::TwoHanded)) != 0
				|| ((Loc & ACEEquipMask::MissileWeapon) != 0 && Obj.AmmoType != 0))))
		{
			bConflict = true;
		}
		else if (bWeaponHand && (EqLoc & WeaponHand) != 0)
		{
			bConflict = true;
		}
		else if (bClothingOrArmor)
		{
			// Retail Creature_Equipment: Clothing conflicts use ClothingPriority, not EquipMask.
			const uint32 EqCov = ACEInferClothingPriority(EqLoc, Eq.ItemType);
			if (EqCov != 0)
			{
				bConflict = (NewCov & EqCov) != 0;
			}
			else if ((EqLoc & Loc) != 0)
			{
				bConflict = true;
			}
		}
		else if ((Loc & Jewelry) != 0 && (EqLoc & Loc) != 0)
		{
			bConflict = true;
		}
		else if ((EqLoc & Loc) != 0 && ACEInferClothingPriority(EqLoc, Eq.ItemType) == 0)
		{
			bConflict = true;
		}
		if (bConflict)
		{
			PendingEquipmentRemovals.Add(Eq.Guid);
		}
	}
    // Reserve room for every conflict before removing the first item (for
    // example, both weapon and shield when changing to a two-handed weapon).
    TMap<int32,int32> Reserved;
    for(int32 Removing:PendingEquipmentRemovals)
    {
        const int32 Destination=FindEquipmentStowContainer(Reserved);
        if(!Destination)
        {
            CancelEquipmentSwap();
            EmitChatMessage(TEXT("Not enough room in your packs to change equipment."),TEXT(""),ACEChatMessageType::TransientInfo);
            return;
        }
        ++Reserved.FindOrAdd(Destination);
    }
	// PutItemInContainer can wait for a stance animation on the server. Sending
	// Wield in the same burst races that dequip and leaves the new item unwielded.
	AdvanceEquipmentSwap();
}

void FACESession::SendDropItem(int32 ItemGuid)
{
	if (State != EACESessionState::InWorld || ItemGuid == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(ItemGuid));
	SendGameAction(ACEGameAction::DropItem, W.GetData(), ACEQueue::WeenieQueue);
	PendingInventoryTransfers.Add(ItemGuid,FPlatformTime::Seconds()+5.);
}

void FACESession::SendGiveObjectRequest(int32 TargetGuid, int32 ItemGuid, int32 Amount)
{
	if (State != EACESessionState::InWorld || TargetGuid == 0 || ItemGuid == 0 || Amount <= 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(TargetGuid));
	W.WriteUInt32(static_cast<uint32>(ItemGuid));
	W.WriteInt32(Amount);
	SendGameAction(ACEGameAction::GiveObjectRequest, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendNoLongerViewingContents(int32 ContainerGuid)
{
	if (State != EACESessionState::InWorld || ContainerGuid == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(ContainerGuid));
	SendGameAction(ACEGameAction::NoLongerViewingContents, W.GetData(), ACEQueue::WeenieQueue);
	if (OpenExternalContainerGuid == ContainerGuid)
	{
		OpenExternalContainerGuid = 0;
	}
	if (OpenVendorGuid == ContainerGuid)
	{
		OpenVendorGuid = 0;
	}
	ClearContainerContents(ContainerGuid);
}

void FACESession::SendBuyItems(int32 VendorGuid, const TArray<TPair<int32, int32>>& AmountAndObjectId)
{
	if (State != EACESessionState::InWorld || VendorGuid == 0 || AmountAndObjectId.Num() == 0)
	{
		return;
	}
	// Revalidate at send time: stock can change while a purchase is in the cart.
	TArray<TPair<int32,int32>> Available;
	if (VendorGuid != OpenVendorGuid) return;
	for (const auto& Pair : AmountAndObjectId)
	{
		const auto* Stock=VendorMerchandise.FindByPredicate([&](const FACEWorldObject& Item){return Item.Guid==Pair.Value;});
		if (!Stock || Pair.Key<=0) continue;
		const int32 Limit=ACEInventoryRules::VendorPurchaseLimit(*Stock);
		if (Limit<=0) continue;
		if (auto* Existing=Available.FindByPredicate([&](const auto& Item){return Item.Value==Pair.Value;}))
			Existing->Key=static_cast<int32>(FMath::Min<int64>(Limit,static_cast<int64>(Existing->Key)+Pair.Key));
		else Available.Emplace(FMath::Min(Pair.Key,Limit),Pair.Value);
	}
	if (Available.IsEmpty()) return;
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(VendorGuid));
	W.WriteUInt32(static_cast<uint32>(Available.Num()));
	for (const TPair<int32, int32>& Pair : Available)
	{
		W.WriteInt32(Pair.Key);   // amount
		W.WriteUInt32(static_cast<uint32>(Pair.Value)); // objectID
	}
	// Retail CM_Vendor::Event_Buy (006AA0F0) always appends this field,
	// including zero for pyreals. Stock ACE ignores it, but servers that read
	// the complete retail request cannot process a packet without the trailer.
	W.WriteUInt32(VendorCurrencyWeenie);
	SendGameAction(ACEGameAction::Buy, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendSellItems(int32 VendorGuid, const TArray<TPair<int32, int32>>& AmountAndObjectId)
{
	if (State != EACESessionState::InWorld || VendorGuid == 0 || AmountAndObjectId.Num() == 0)
	{
		return;
	}
	// Sell transfers whole objects on retail-compatible servers. A partial amount
	// paired with the source GUID can sell its entire stack; the UI must split first.
	for (const auto& Pair : AmountAndObjectId)
	{
		const FACEWorldObject* Item = WorldObjects.Find(Pair.Value);
		if (!Item || Pair.Key != FMath::Max(1, Item->StackSize)) return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(VendorGuid));
	W.WriteUInt32(static_cast<uint32>(AmountAndObjectId.Num()));
	for (const TPair<int32, int32>& Pair : AmountAndObjectId)
	{
		W.WriteInt32(Pair.Key);   // amount
		W.WriteUInt32(static_cast<uint32>(Pair.Value)); // objectID
	}
	SendGameAction(ACEGameAction::Sell, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendOpenTrade(int32 OtherPlayerGuid)
{
	if (State != EACESessionState::InWorld || OtherPlayerGuid == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(OtherPlayerGuid));
	SendGameAction(ACEGameAction::OpenTradeNegotiations, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendCloseTrade()
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	SendGameAction(ACEGameAction::CloseTradeNegotiations, {}, ACEQueue::WeenieQueue);
}

void FACESession::SendAddToTrade(int32 ItemGuid, int32 TradeSlotSide)
{
	if (State != EACESessionState::InWorld || ItemGuid == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(ItemGuid));
	W.WriteUInt32(static_cast<uint32>(TradeSlotSide));
	SendGameAction(ACEGameAction::AddToTrade, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendAcceptTrade()
{
	if (State != EACESessionState::InWorld || TradePartnerGuid == 0)
	{
		return;
	}
	// Retail Trade::Pack includes the registered timestamp, a boolean initiator
	// flag, both acceptance flags and both ordered ContentProfile lists. Stock
	// ACE ignores most of the blob; stricter hosts still need the complete wire form.
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(TradePartnerGuid));
	W.WriteDouble(TradeStamp);
	W.WriteUInt32(2); // registered trade
	W.WriteUInt32(TradeInitiatorGuid == PlayerGuid ? 1 : 0);
	W.WriteUInt32(1); // self accepts
	W.WriteUInt32(bTradePartnerAccepted ? 1 : 0);
	for (const TArray<int32>* Items : {&TradeSelfItems, &TradePartnerItems})
	{
		W.WriteUInt32(Items->Num());
		for (int32 Guid : *Items)
		{
			W.WriteUInt32(static_cast<uint32>(Guid));
			W.WriteUInt32(0); // Trade::AddItem constructs ContentProfile with no container flags
		}
	}
	SendGameAction(ACEGameAction::AcceptTrade, W.GetData(), ACEQueue::WeenieQueue);
	bTradeSelfAccepted = true;
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::AcceptTrade));
}

void FACESession::SendDeclineTrade()
{
	if (State != EACESessionState::InWorld || TradePartnerGuid == 0)
	{
		return;
	}
	SendGameAction(ACEGameAction::DeclineTrade, {}, ACEQueue::WeenieQueue);
	bTradeSelfAccepted = false;
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::DeclineTrade));
}

void FACESession::SendResetTrade()
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	SendGameAction(ACEGameAction::ResetTrade, {}, ACEQueue::WeenieQueue);
}

void FACESession::SendAddSpellToBar(int32 SpellId, int32 SlotIndex, int32 BarIndex)
{
	if (State != EACESessionState::InWorld || SpellId == 0)
	{
		return;
	}
	BarIndex = FMath::Clamp(BarIndex, 0, 7);
	if (SpellBars.Num() < 8)
	{
		SpellBars.SetNum(8);
	}
	TArray<int32>& Bar = SpellBars[BarIndex];
	// Favorites are a growing list; the thirteen-cell viewport is not a storage limit.

	// Dense retail bars: count filled prefix (trailing zeros are empty).
	auto CountFilled = [&Bar]() -> int32
	{
		int32 N = 0;
		for (int32 i = 0; i < Bar.Num(); ++i)
		{
			if (Bar[i] == 0) { break; }
			++N;
		}
		return N;
	};

	// Server AddSpellToBar rejects duplicates — remove first when relocating.
	int32 ExistingIdx = INDEX_NONE;
	for (int32 i = 0; i < Bar.Num(); ++i)
	{
		if (Bar[i] == SpellId)
		{
			ExistingIdx = i;
			break;
		}
	}
	if (ExistingIdx != INDEX_NONE)
	{
		FACEBinaryWriter Rem;
		Rem.WriteUInt32(static_cast<uint32>(SpellId));
		Rem.WriteUInt32(static_cast<uint32>(BarIndex));
		SendGameAction(ACEGameAction::RemoveFromSpellBar, Rem.GetData(), ACEQueue::WeenieQueue);
		for (int32 i = ExistingIdx; i < Bar.Num() - 1; ++i)
		{
			Bar[i] = Bar[i + 1];
		}
		Bar[Bar.Num() - 1] = 0;
		if (SlotIndex > ExistingIdx)
		{
			--SlotIndex;
		}
	}

	int32 Count = CountFilled();
	// ACE Character.AddSpellToBar: clamp past-end drops onto the first empty (append).
	int32 InsertAt = FMath::Clamp(SlotIndex, 0, Count);
	if (InsertAt > Count)
	{
		InsertAt = Count;
	}
	if (Bar.Num() <= Count)
	{
		Bar.Add(0);
	}

	// ACE HandleActionAddSpellFavorite: spellId, spellBarPositionId (0-based), spellBarId (0-based).
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(SpellId));
	W.WriteUInt32(static_cast<uint32>(InsertAt));
	W.WriteUInt32(static_cast<uint32>(BarIndex));
	SendGameAction(ACEGameAction::AddToSpellBar, W.GetData(), ACEQueue::WeenieQueue);

	// Mirror Character.AddSpellToBar insert/shift — server does not echo bar changes.
	for (int32 i = Count; i > InsertAt; --i)
	{
		Bar[i] = Bar[i - 1];
	}
	Bar[InsertAt] = SpellId;
	++SpellDataRevision;
}

void FACESession::SendRemoveSpellFromBar(int32 SpellId, int32 BarIndex)
{
	if (State != EACESessionState::InWorld || SpellId == 0)
	{
		return;
	}
	BarIndex = FMath::Clamp(BarIndex, 0, 7);
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(SpellId));
	W.WriteUInt32(static_cast<uint32>(BarIndex));
	SendGameAction(ACEGameAction::RemoveFromSpellBar, W.GetData(), ACEQueue::WeenieQueue);

	if (SpellBars.IsValidIndex(BarIndex))
	{
		TArray<int32>& Bar = SpellBars[BarIndex];
		int32 Found = INDEX_NONE;
		for (int32 i = 0; i < Bar.Num(); ++i)
		{
			if (Bar[i] == SpellId)
			{
				Found = i;
				break;
			}
		}
		if (Found != INDEX_NONE)
		{
			for (int32 i = Found; i < Bar.Num() - 1; ++i)
			{
				Bar[i] = Bar[i + 1];
			}
			Bar[Bar.Num() - 1] = 0;
			++SpellDataRevision;
		}
	}
}

void FACESession::SendSetDesiredComponentLevel(int32 ComponentWcid, int32 Amount)
{
	if (State != EACESessionState::InWorld || ComponentWcid == 0)
	{
		return;
	}
	const int32 Clamped = FMath::Clamp(Amount, 0, 5000);
	if (Clamped > 0)
	{
		DesiredComponents.Add(ComponentWcid, Clamped);
	}
	else
	{
		DesiredComponents.Remove(ComponentWcid);
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(ComponentWcid));
	W.WriteUInt32(static_cast<uint32>(Clamped));
	SendGameAction(ACEGameAction::SetDesiredComponentLevel, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendAddShortcut(int32 SlotIndex, int32 ObjectGuid)
{
	if (State != EACESessionState::InWorld || ObjectGuid == 0)
	{
		return;
	}
	const uint32 Index = static_cast<uint32>(FMath::Clamp(SlotIndex, 0, 17));
	// Shortcut wire: index u32, objectId u32, LayeredSpell (spellId u16 + layer u16) unused zeros.
	FACEBinaryWriter W;
	W.WriteUInt32(Index);
	W.WriteUInt32(static_cast<uint32>(ObjectGuid));
	W.WriteUInt16(0);
	W.WriteUInt16(0);
	SendGameAction(ACEGameAction::AddShortCut, W.GetData(), ACEQueue::WeenieQueue);

	if (ShortcutObjects.Num() <= static_cast<int32>(Index))
	{
		ShortcutObjects.SetNumZeroed(static_cast<int32>(Index) + 1);
	}
	ShortcutObjects[static_cast<int32>(Index)] = ObjectGuid;
}

void FACESession::SendRemoveShortcut(int32 SlotIndex)
{
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	const uint32 Index = static_cast<uint32>(FMath::Clamp(SlotIndex, 0, 17));
	FACEBinaryWriter W;
	W.WriteUInt32(Index);
	SendGameAction(ACEGameAction::RemoveShortCut, W.GetData(), ACEQueue::WeenieQueue);
	if (ShortcutObjects.IsValidIndex(static_cast<int32>(Index)))
	{
		ShortcutObjects[static_cast<int32>(Index)] = 0;
	}
}

void FACESession::SendCastSpell(int32 SpellId, int32 TargetGuid)
{
	if (State != EACESessionState::InWorld || SpellId == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	if (TargetGuid != 0)
	{
		W.WriteUInt32(static_cast<uint32>(TargetGuid));
		W.WriteUInt32(static_cast<uint32>(SpellId));
		SendGameAction(ACEGameAction::CastTargetedSpell, W.GetData(), ACEQueue::WeenieQueue);
	}
	else
	{
		W.WriteUInt32(static_cast<uint32>(SpellId));
		SendGameAction(ACEGameAction::CastUntargetedSpell, W.GetData(), ACEQueue::WeenieQueue);
	}
	OnSpellCastRequested.Broadcast(SpellId, TargetGuid);
}

const TArray<int32>& FACESession::GetSpellBar(int32 BarIndex) const
{
	static const TArray<int32> Empty;
	if (!SpellBars.IsValidIndex(BarIndex))
	{
		return Empty;
	}
	return SpellBars[BarIndex];
}

int32 FACESession::GetShortcutObject(int32 SlotIndex) const
{
	return ShortcutObjects.IsValidIndex(SlotIndex) ? ShortcutObjects[SlotIndex] : 0;
}

void FACESession::SetActiveSpellBar(int32 BarIndex)
{
	const int32 NewBar = FMath::Clamp(BarIndex, 0, 7);
	if (ActiveSpellBar != NewBar)
	{
		ActiveSpellBar = NewBar;
		++SpellDataRevision;
	}
}

void FACESession::HandleMagicUpdateSpell(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 SpellId = static_cast<int32>(Reader.ReadUInt16());
	Reader.ReadUInt16(); // layer
	if (SpellId > 0 && !KnownSpells.Contains(SpellId))
	{
		KnownSpells.Add(SpellId);
		++SpellDataRevision;
		// Incremental learning is distinct from the login spellbook snapshot.
		if (State == EACESessionState::InWorld) OnSpellLearned.Broadcast(SpellId);
	}
}

void FACESession::HandleMagicRemoveSpell(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 SpellId = static_cast<int32>(Reader.ReadUInt16());
	Reader.ReadUInt16();
	if (KnownSpells.Remove(SpellId) > 0) ++SpellDataRevision;
}

bool FACESession::ReadEnchantmentRecord(FACEBinaryReader& Reader, FACEActiveEnchantment& Out)
{
	// Matches ACE.Server Network.Structure.Enchantment.Write
	if (!Reader.CanRead(60))
	{
		return false;
	}
	Out = FACEActiveEnchantment();
	Out.ReceivedAt = FPlatformTime::Seconds();
	Out.SpellId = static_cast<int32>(Reader.ReadUInt16());
	Out.Layer = static_cast<int32>(Reader.ReadUInt16());
	Out.SpellCategory = static_cast<int32>(Reader.ReadUInt16());
	const uint16 HasSet = Reader.ReadUInt16();
	Out.PowerLevel = static_cast<int32>(Reader.ReadUInt32()); // PowerLevel
	Out.StartTime = static_cast<float>(Reader.ReadDouble());
	Out.Duration = static_cast<float>(Reader.ReadDouble());
	Out.CasterGuid = static_cast<int32>(Reader.ReadUInt32());
	Reader.ReadFloat();  // DegradeModifier
	Reader.ReadFloat();  // DegradeLimit
	Reader.ReadDouble(); // LastTimeDegraded
	Out.StatModType = static_cast<int32>(Reader.ReadUInt32());
	Out.StatModKey = static_cast<int32>(Reader.ReadUInt32());
	Out.StatModValue = Reader.ReadFloat();
	if (HasSet != 0)
	{
		if (!Reader.CanRead(4))
		{
			return false;
		}
		Reader.ReadUInt32(); // SpellSetID
	}
	constexpr uint32 FlagBeneficial = 0x2000000u;
	constexpr uint32 FlagVitae = 0x0800000u;
	constexpr uint32 FlagCooldown = 0x1000000u;
	Out.bBeneficial = (static_cast<uint32>(Out.StatModType) & FlagBeneficial) != 0;
	Out.bVitae = (static_cast<uint32>(Out.StatModType) & FlagVitae) != 0 || Out.SpellId == 666;
	Out.bCooldown = (static_cast<uint32>(Out.StatModType) & FlagCooldown) != 0;
	return FMath::IsFinite(Out.StartTime) && FMath::IsFinite(Out.Duration) && FMath::IsFinite(Out.StatModValue);
}

bool FACESession::ReadEnchantmentList(FACEBinaryReader& Reader, TArray<FACEActiveEnchantment>& Out)
{
	if (!Reader.CanRead(4)) return false;
	const uint32 Count = Reader.ReadUInt32();
	if (Count > static_cast<uint32>(Reader.Remaining() / 60)) return false;
	Out.Reserve(Out.Num() + static_cast<int32>(Count));
	for (uint32 I = 0; I < Count; ++I)
	{
		FACEActiveEnchantment Entry;
		if (!ReadEnchantmentRecord(Reader, Entry)) return false;
		Out.Add(Entry);
	}
	return true;
}

void FACESession::UpsertEnchantment(const FACEActiveEnchantment& Entry)
{
	// Retail CEnchantmentRegistry keeps separate vitae, cooldown, multiplicative
	// and additive lists. Identity is the packed spell/layer ID, never the caster.
	auto List = [](const FACEActiveEnchantment& E)
	{
		if (E.bVitae || E.SpellId == 666) return 4;
		if (E.bCooldown) return 8;
		if (E.StatModType & 0x4000) return 1;
		if (E.StatModType & 0x8000) return 2;
		return 0; // Preserve custom server records without a retail modifier flag.
	};
	const int32 Kind = List(Entry);
	int32 Existing = ActiveEnchantments.IndexOfByPredicate([&](const FACEActiveEnchantment& E)
	{
		return List(E) == Kind && (Kind == 4 || (E.SpellId == Entry.SpellId && E.Layer == Entry.Layer));
	});
	// AttemptToReplaceSpellInList: a timed recast may have a different layer.
	// Permanent item effects on other layers must remain independently removable.
	if (Existing == INDEX_NONE && Entry.Duration > 0.f)
		Existing = ActiveEnchantments.IndexOfByPredicate([&](const FACEActiveEnchantment& E)
		{
			return List(E) == Kind && E.SpellId == Entry.SpellId && E.Duration > 0.f;
		});
	if (Existing != INDEX_NONE) ActiveEnchantments[Existing] = Entry;
	else ActiveEnchantments.Add(Entry);
}

void FACESession::RemoveEnchantment(uint16 SpellId, uint16 Layer)
{
	ActiveEnchantments.RemoveAll([SpellId, Layer](const FACEActiveEnchantment& E)
	{
		return E.SpellId == static_cast<int32>(SpellId) && E.Layer == static_cast<int32>(Layer);
	});
}

void FACESession::NotifyEnchantmentsChanged()
{
	RecomputeAttributeEnchantments(PlayerVitals);
	NotifyVitalsChanged();
	OnEnchantmentsChanged.Broadcast();
}

void FACESession::GetActiveEnchantments(TArray<FACEActiveEnchantment>& Out, bool bIncludeCooldowns) const
{
	Out.Reset();
	for (const FACEActiveEnchantment& E : ActiveEnchantments)
	{
		if (bIncludeCooldowns || !E.bCooldown)
		{
			Out.Add(E);
		}
	}
}

int32 FACESession::GetItemCooldownPhase(const FACEWorldObject& Object, double Now) const
{
	if (Object.CooldownId <= 0 || Object.CooldownId > 0x7fff
		|| !FMath::IsFinite(Object.CooldownDuration) || Object.CooldownDuration <= 0.0) return 0;
	// UIElement_UIItem::UpdateCooldownDisplay uses the shared cooldown registry,
	// not use attempts or appraisal. Login restores StartTime as negative elapsed.
	for (const FACEActiveEnchantment& E : ActiveEnchantments)
	{
		if (!E.bCooldown || E.SpellId != Object.CooldownId + 0x8000) continue;
		const double Remaining = double(E.Duration) + E.StartTime - (Now - E.ReceivedAt);
		if (FMath::IsFinite(Remaining) && Remaining > 0.0)
			return 1 + FMath::FloorToInt(FMath::Clamp(Remaining / Object.CooldownDuration * 10.0, 0.0, 9.0));
	}
	return 0;
}

bool FACESession::TryGetVitaeMultiplier(float& OutMultiplier) const
{
	for (const FACEActiveEnchantment& E : ActiveEnchantments)
	{
		if (E.bVitae || E.SpellId == 666)
		{
			OutMultiplier = E.StatModValue;
			return true;
		}
	}
	return false;
}

void FACESession::SendPingRequest()
{
	if (State != EACESessionState::InWorld || (PingRequestSentAt > 0.0 && FPlatformTime::Seconds() - PingRequestSentAt < 120.0))
	{
		return;
	}
	PingRequestSentAt = FPlatformTime::Seconds();
	SendGameAction(ACEGameAction::PingRequest, {}, ACEQueue::UIQueue);
}

void FACESession::RecordLinkTraffic(double Now, uint32 Sent, uint32 Retransmits)
{
	const int64 Second = FMath::FloorToInt64(Now);
	auto& Bucket = LinkTraffic[Second % UE_ARRAY_COUNT(LinkTraffic)];
	if (Bucket.Second != Second) { Bucket = FLinkTrafficBucket(); Bucket.Second = Second; }
	Bucket.Sent += Sent;
	Bucket.Retransmits += Retransmits;
}

FACELinkStatus FACESession::GetLinkStatus() const
{
	return GetLinkStatusAt(FPlatformTime::Seconds());
}

FACELinkStatus FACESession::GetLinkStatusAt(double Now) const
{
	FACELinkStatus Status = LinkStatus;
	Status.bConnected = State >= EACESessionState::CharacterSelect && State < EACESessionState::Failed;
	Status.SecondsSinceLastPacket = LastServerPacketAt > 0.0 ? FMath::Max(0.0, Now - LastServerPacketAt) : 0.f;
	uint64 Sent = 0, Retransmits = 0;
	const int64 Second = FMath::FloorToInt64(Now);
	for (const auto& Bucket : LinkTraffic)
		if (Bucket.Second >= 0 && Bucket.Second <= Second && Second - Bucket.Second < UE_ARRAY_COUNT(LinkTraffic))
		{ Sent += Bucket.Sent; Retransmits += Bucket.Retransmits; }
	Status.PacketLossPercent = Sent + Retransmits > 0 ? 100.f * Retransmits / (Sent + Retransmits) : 0.f;
	return Status;
}

float FACESession::RecordEchoRequest(double SentAt)
{
	if (EchoTimeOrigin == 0.0) EchoTimeOrigin = SentAt - 1.0;
	const float Token = static_cast<float>(SentAt - EchoTimeOrigin);
	for (auto It = PendingEchoTimes.CreateIterator(); It; ++It)
		if (SentAt - It.Value() > 30.0) It.RemoveCurrent();
	PendingEchoTimes.Add(Token, SentAt);
	return Token;
}

void FACESession::UpdateLinkStatusFromEcho(float ClientTimeSent, double ReceivedAt)
{
	// The wire timestamp is float. Never subtract two absolute float clock values:
	// FPlatformTime's large origin can round a normal internet roundtrip to zero.
	double SentAt = 0.0;
	if (PendingEchoTimes.RemoveAndCopyValue(ClientTimeSent, SentAt)
		&& ReceivedAt > SentAt && ReceivedAt - SentAt <= 30.0)
	{
		LinkStatus.RoundTripSeconds = static_cast<float>(ReceivedAt - SentAt);
		LinkStatus.bHasPing = true;
	}
	OnLinkStatusChanged.Broadcast(GetLinkStatus());
}

void FACESession::HandlePingResponse(FACEBinaryReader& /*Reader*/)
{
	if (PingRequestSentAt > 0.0)
	{
		LinkStatus.RoundTripSeconds = static_cast<float>(FPlatformTime::Seconds() - PingRequestSentAt);
		LinkStatus.bHasPing = true;
		PingRequestSentAt = 0.0;
	}
	OnLinkStatusChanged.Broadcast(GetLinkStatus());
}

void FACESession::RecomputeAttributeEnchantments(FACEPlayerVitals& OutVitals)
{
	auto Reset = [](float& Mul, float& Add)
	{
		Mul = 1.f;
		Add = 0.f;
	};
	Reset(OutVitals.StrengthEnchantMul, OutVitals.StrengthEnchantAdd);
	Reset(OutVitals.EnduranceEnchantMul, OutVitals.EnduranceEnchantAdd);
	Reset(OutVitals.QuicknessEnchantMul, OutVitals.QuicknessEnchantAdd);
	Reset(OutVitals.CoordinationEnchantMul, OutVitals.CoordinationEnchantAdd);
	Reset(OutVitals.FocusEnchantMul, OutVitals.FocusEnchantAdd);
	Reset(OutVitals.SelfEnchantMul, OutVitals.SelfEnchantAdd);

	constexpr uint32 FlagAttribute = 0x0000001u;
	constexpr uint32 FlagMultiplicative = 0x0004000u;
	constexpr uint32 FlagAdditive = 0x0008000u;

	for (const FACEActiveEnchantment& E : ActiveEnchantments)
	{
		if (E.bCooldown || (static_cast<uint32>(E.StatModType) & FlagAttribute) == 0)
		{
			continue;
		}
		float* Mul = nullptr;
		float* Add = nullptr;
		switch (E.StatModKey)
		{
		case 1: Mul = &OutVitals.StrengthEnchantMul; Add = &OutVitals.StrengthEnchantAdd; break;
		case 2: Mul = &OutVitals.EnduranceEnchantMul; Add = &OutVitals.EnduranceEnchantAdd; break;
		case 3: Mul = &OutVitals.QuicknessEnchantMul; Add = &OutVitals.QuicknessEnchantAdd; break;
		case 4: Mul = &OutVitals.CoordinationEnchantMul; Add = &OutVitals.CoordinationEnchantAdd; break;
		case 5: Mul = &OutVitals.FocusEnchantMul; Add = &OutVitals.FocusEnchantAdd; break;
		case 6: Mul = &OutVitals.SelfEnchantMul; Add = &OutVitals.SelfEnchantAdd; break;
		default: break;
		}
		if (!Mul || !Add)
		{
			continue;
		}
		if ((static_cast<uint32>(E.StatModType) & FlagMultiplicative) != 0)
		{
			*Mul *= E.StatModValue;
		}
		else if ((static_cast<uint32>(E.StatModType) & FlagAdditive) != 0)
		{
			*Add += E.StatModValue;
		}
	}
}

void FACESession::HandleMagicUpdateEnchantment(FACEBinaryReader& Reader)
{
	FACEActiveEnchantment Entry;
	if (!ReadEnchantmentRecord(Reader, Entry))
	{
		return;
	}
	UpsertEnchantment(Entry);
	NotifyEnchantmentsChanged();
}

void FACESession::HandleMagicRemoveEnchantment(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(4))
	{
		return;
	}
	const uint16 SpellId = Reader.ReadUInt16();
	const uint16 Layer = Reader.ReadUInt16();
	const int32 Before = ActiveEnchantments.Num();
	RemoveEnchantment(SpellId, Layer);
	if (ActiveEnchantments.Num() != Before)
	{
		NotifyEnchantmentsChanged();
	}
}

void FACESession::HandleMagicUpdateMultipleEnchantments(FACEBinaryReader& Reader)
{
	TArray<FACEActiveEnchantment> Updates;
	if (!ReadEnchantmentList(Reader, Updates)) return;
	for (const auto& Entry : Updates) UpsertEnchantment(Entry);
	if (!Updates.IsEmpty()) NotifyEnchantmentsChanged();
}

void FACESession::HandleMagicRemoveMultipleEnchantments(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(4))
	{
		return;
	}
	const uint32 Count = Reader.ReadUInt32();
	if (Count > static_cast<uint32>(Reader.Remaining() / 4)) return;
	bool bChanged = false;
	for (uint32 i = 0; i < Count; ++i)
	{
		const uint16 SpellId = Reader.ReadUInt16();
		const uint16 Layer = Reader.ReadUInt16();
		const int32 Before = ActiveEnchantments.Num();
		RemoveEnchantment(SpellId, Layer);
		bChanged |= ActiveEnchantments.Num() != Before;
	}
	if (bChanged)
	{
		NotifyEnchantmentsChanged();
	}
}

void FACESession::HandleMagicPurgeEnchantments(FACEBinaryReader& /*Reader*/)
{
	if (ActiveEnchantments.Num() == 0)
	{
		return;
	}
	// CEnchantmentRegistry::PurgeEnchantmentList retains duration -1 effects.
	// Equipment enchantments remain active through death. Vitae and cooldowns
	// live in separate retail lists, so this purge must not touch those either.
	const int32 Before = ActiveEnchantments.Num();
	ActiveEnchantments.RemoveAll([](const FACEActiveEnchantment& E)
	{
		return E.Duration != -1.f && !E.bVitae && E.SpellId != 666
			&& !E.bCooldown && E.SpellId < 0x8000;
	});
	if (ActiveEnchantments.Num() != Before) NotifyEnchantmentsChanged();
}

void FACESession::HandleMagicPurgeBadEnchantments(FACEBinaryReader& /*Reader*/)
{
	const int32 Before = ActiveEnchantments.Num();
	ActiveEnchantments.RemoveAll([](const FACEActiveEnchantment& E)
	{
		// PurgeBadEnchantmentList has the same permanent-effect exception.
		return E.Duration != -1.f && !E.bBeneficial && !E.bVitae && !E.bCooldown
			&& E.SpellId != 666 && E.SpellId < 0x8000;
	});
	if (ActiveEnchantments.Num() != Before)
	{
		NotifyEnchantmentsChanged();
	}
}

void FACESession::HandleMagicDispelEnchantment(FACEBinaryReader& Reader)
{
	HandleMagicRemoveEnchantment(Reader);
}

void FACESession::HandleMagicDispelMultipleEnchantments(FACEBinaryReader& Reader)
{
	HandleMagicRemoveMultipleEnchantments(Reader);
}

bool FACESession::IsNearbyHealthObject(int32 Guid) const
{
	if (Guid == PlayerGuid) return true;
	const auto* Object = WorldObjects.Find(Guid);
	return Object && Object->bHasPosition && Object->Position.CellId && PlayerPosition.CellId
		&& FVector::DistSquared(Object->Position.ToUnrealLocation(1.f),PlayerPosition.ToUnrealLocation(1.f)) <= FMath::Square(192.f);
}

void FACESession::HandleUpdateHealth(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(8))
	{
		return;
	}
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const float Fraction = Reader.ReadFloat();
	if (!FMath::IsFinite(Fraction) || !IsNearbyHealthObject(Guid)) return;
	OnObjectHealth.Broadcast(Guid, Fraction);
	if (SelectedObject.bValid && SelectedObject.Guid == Guid)
	{
		if (!CanQueryObjectHealth(Guid)) return;
		SelectedObject.HealthFraction = FMath::Clamp(Fraction, 0.f, 1.f);
		SelectedObject.bShowHealth = true;
		SelectedObject.bShowMana = false;
		OnSelectionChanged.Broadcast(SelectedObject);
	}
}

void FACESession::HandleQueryItemManaResponse(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(12)) return;
	const int32 Guid = int32(Reader.ReadUInt32());
	const float Fraction = Reader.ReadFloat();
	const bool Success = Reader.ReadUInt32() != 0;
	if (!SelectedObject.bValid || Guid != SelectedObject.Guid || Guid != SelectedManaQueryGuid) return;
	const bool Show = Success && FMath::IsFinite(Fraction) && CanQueryItemMana(Guid);
	SelectedObject.bShowMana = Show;
	SelectedObject.ManaFraction = Show ? FMath::Clamp(Fraction, 0.f, 1.f) : 0.f;
	if (!Show)
	{
		// Retail stops polling non-magical items after the unsuccessful reply.
		SelectedManaQueryGuid = 0;
		FACEBinaryWriter Cancel;
		Cancel.WriteUInt32(0);
		SendGameAction(ACEGameAction::QueryItemMana, Cancel.GetData(), ACEQueue::WeenieQueue);
	}
	OnSelectionChanged.Broadcast(SelectedObject);
}

void FACESession::HandleIdentifyObjectResponse(FACEBinaryReader& Reader)
{
	// GameEventIdentifyObjectResponse: u32 objectGuid + AppraiseInfo
	if (!Reader.CanRead(12))
	{
		return;
	}
	FACEAppraisalInfo Info;
	Info.ObjectGuid = static_cast<int32>(Reader.ReadUInt32());
	if (Info.ObjectGuid==CursorAppraisalGuid) { CursorAppraisalGuid=0; CursorAppraisalUntil=0.; }
	const uint32 Flags = Reader.ReadUInt32();
	Info.bSuccess = Reader.ReadUInt32() != 0;

	FACEWorldObject Known;
	if (GetWorldObject(Info.ObjectGuid, Known))
	{
		Info.Name = Known.Name;
		Info.ObjectDescriptionFlags = Known.ObjectDescriptionFlags;
		// Retail capacity lines read the public weenie descriptor. These fields
		// need not be repeated in the appraisal, including on custom servers.
		Info.IntProperties.Add(6, Known.ItemsCapacity);
		Info.IntProperties.Add(7, Known.ContainersCapacity);
		// Retail also uses the public descriptor for weapon/armor classification;
		// servers need not repeat these fields in an appraisal. Explicit appraisal
		// qualities below take precedence over these descriptor defaults.
		Info.IntProperties.Add(9, static_cast<int32>(Known.ValidLocations));
		Info.IntProperties.Add(50, Known.AmmoType);
		Info.IntProperties.Add(4, Known.ClothingPriority);
		Info.IntProperties.Add(51, Known.CombatUse);
		// Material is public descriptor data even when the appraisal omits it.
		// Preserve it so every examination title uses the same retail item name.
		if (Known.MaterialType > 0) Info.IntProperties.Add(131, Known.MaterialType);
		Info.ItemType = Known.ItemType;
		Info.bIsCreature = (Known.ItemType & ACEItemType::Creature) != 0 || Known.bIsPlayer;
		if (Known.IconId != 0)
		{
			Info.IconDid = Known.IconId;
		}
	}

	auto ReadTableHeader = [&Reader]() -> int32
	{
		if (!Reader.CanRead(4)) { return -1; }
		const uint16 Count = Reader.ReadUInt16();
		Reader.ReadUInt16();
		return Count;
	};

	FString Summary;
	constexpr uint32 FlagInt = 0x0001;
	constexpr uint32 FlagBool = 0x0002;
	constexpr uint32 FlagFloat = 0x0004;
	constexpr uint32 FlagString = 0x0008;
	constexpr uint32 FlagSpellBook = 0x0010;
	constexpr uint32 FlagWeapon = 0x0020;
	constexpr uint32 FlagHook = 0x0040;
	constexpr uint32 FlagArmor = 0x0080;
	constexpr uint32 FlagCreature = 0x0100;
	constexpr uint32 FlagArmorEnc = 0x0200;
	constexpr uint32 FlagResistEnc = 0x0400;
	constexpr uint32 FlagWeaponEnc = 0x0800;
	constexpr uint32 FlagDid = 0x1000;
	constexpr uint32 FlagInt64 = 0x2000;
	constexpr uint32 FlagArmorLevels = 0x4000;

	if (Flags & FlagInt)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(8); ++i)
		{
			const uint32 Prop = Reader.ReadUInt32();
			const int32 Val = Reader.ReadInt32();
			Info.IntProperties.Add(Prop, Val);
			if (Prop == 1) { Info.ItemType = Val; }
			else if (Prop == 25) { Info.Level = Val; }
			else if (Prop == 2) { Info.CreatureType = Val; }
			else if (Prop == 19) { Info.Value = Val; Info.bHasValue = true; }
			else if (Prop == 5) { Info.Burden = Val; Info.bHasBurden = true; }
			else if (Prop == 33) // Bonded
			{
				if (FACEWorldObject* Obj = WorldObjects.Find(Info.ObjectGuid))
				{
					Obj->Bonded = Val;
				}
			}
			else if (Prop == 114) // Attuned
			{
				if (FACEWorldObject* Obj = WorldObjects.Find(Info.ObjectGuid))
				{
					Obj->Attuned = Val;
				}
			}
			else if (Prop == 18) // UiEffects
			{
				if (FACEWorldObject* Obj = WorldObjects.Find(Info.ObjectGuid))
				{
					Obj->UiEffects = Val;
				}
			}
			else if (Prop == 307) { Info.DamageRating = Val; }
			else if (Prop == 308) { Info.DamageResistRating = Val; }
			else if (Prop == 313) { Info.CritRating = Val; }
			else if (Prop == 314) { Info.CritDamageRating = Val; }
			else if (Prop == 315) { Info.CritResistRating = Val; }
			else if (Prop == 316) { Info.CritDamageResistRating = Val; }
		}
	}
	if (Flags & FlagInt64)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(12); ++i)
		{
			const uint32 Prop = Reader.ReadUInt32();
			Info.Int64Properties.Add(Prop, Reader.ReadUInt64());
		}
	}
	if (Flags & FlagBool)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(8); ++i)
		{
			const uint32 Prop = Reader.ReadUInt32();
			Info.BoolProperties.Add(Prop, Reader.ReadUInt32() != 0);
		}
	}
	if (Flags & FlagFloat)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(12); ++i)
		{
			const uint32 Prop = Reader.ReadUInt32();
			Info.FloatProperties.Add(Prop, Reader.ReadDouble());
		}
	}
	if (Flags & FlagString)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(4); ++i)
		{
			const uint32 Prop = Reader.ReadUInt32();
			const FString Val = Reader.ReadString16L();
			Info.StringProperties.Add(Prop, Val);
			if (Prop == 1) { Info.Name = Val; }
			else if (Prop == 7) // PropertyString.Inscription
			{
				Info.Inscription = Val;
			}
			else if ((Prop == 14 || Prop == 15 || Prop == 16) && !Val.IsEmpty()) { Summary += Val + TEXT("\n"); }
		}
	}
	if (Flags & FlagDid)
	{
		const int32 Count = ReadTableHeader();
		for (int32 i = 0; i < Count && Reader.CanRead(8); ++i)
		{
			const uint32 Prop = Reader.ReadUInt32();
			const uint32 Did = Reader.ReadUInt32();
			if (Prop == 8) // PropertyDataId.Icon (including an explicit clear)
			{
				Info.IconDid = static_cast<int32>(Did);
			}
			ApplyPropertyDataID(Info.ObjectGuid, Prop, Did);
		}
	}
	if (Flags & FlagSpellBook)
	{
		if (Reader.CanRead(4))
		{
			const uint32 SpellCount = Reader.ReadUInt32();
			for (uint32 i = 0; i < SpellCount && Reader.CanRead(4); ++i)
			{
				const uint32 Entry = Reader.ReadUInt32();
				Info.SpellBookEntries.Add(static_cast<int32>(Entry));
				const int32 SpellId = static_cast<int32>(Entry & 0x7FFFFFFF);
				Info.SpellIds.Add(SpellId);
			}
			if (Info.SpellIds.Num() > 0)
			{
				Summary += FString::Printf(TEXT("Spells (%d)\n"), Info.SpellIds.Num());
				for (int32 Sid : Info.SpellIds)
				{
					Summary += FString::Printf(TEXT("  Spell %d\n"), Sid);
				}
			}
		}
	}
	if (Flags & FlagArmor)
	{
		if (Reader.CanRead(32))
		{
			const float Slash = Reader.ReadFloat();
			const float Pierce = Reader.ReadFloat();
			const float Bludgeon = Reader.ReadFloat();
			const float Cold = Reader.ReadFloat();
			const float Fire = Reader.ReadFloat();
			const float Acid = Reader.ReadFloat();
			const float Nether = Reader.ReadFloat();
			const float Electric = Reader.ReadFloat();
			Info.ArmorResistances = {Slash, Pierce, Bludgeon, Cold, Fire, Acid, Nether, Electric};
		}
	}
	if (Flags & FlagCreature)
	{
		if (Reader.CanRead(12))
		{
			const uint32 CreatureFlags = Reader.ReadUInt32();
			Info.CreatureProfileFlags = CreatureFlags;
			Info.Health = static_cast<int32>(Reader.ReadUInt32());
			Info.MaxHealth = static_cast<int32>(Reader.ReadUInt32());
			Info.bHasCreatureHealth = true;
			Info.bIsCreature = true;
			if (CreatureFlags & 0x8) // ShowAttributes — 10 u32 attrs/vitals
			{
				if (Reader.CanRead(40))
				{
					Info.Strength = static_cast<int32>(Reader.ReadUInt32());
					Info.Endurance = static_cast<int32>(Reader.ReadUInt32());
					Info.Quickness = static_cast<int32>(Reader.ReadUInt32());
					Info.Coordination = static_cast<int32>(Reader.ReadUInt32());
					Info.Focus = static_cast<int32>(Reader.ReadUInt32());
					Info.Self = static_cast<int32>(Reader.ReadUInt32());
					Info.Stamina = static_cast<int32>(Reader.ReadUInt32());
					Info.Mana = static_cast<int32>(Reader.ReadUInt32());
					Info.MaxStamina = static_cast<int32>(Reader.ReadUInt32());
					Info.MaxMana = static_cast<int32>(Reader.ReadUInt32());
					Info.bHasCreatureAttributes = true;
					Summary += FString::Printf(
						TEXT("Str %d  End %d  Qui %d\nCoo %d  Foc %d  Sel %d\nStamina %d/%d  Mana %d/%d\n"),
						Info.Strength, Info.Endurance, Info.Quickness,
						Info.Coordination, Info.Focus, Info.Self,
						Info.Stamina, Info.MaxStamina, Info.Mana, Info.MaxMana);
				}
			}
			if (CreatureFlags & 0x1)
			{
				if (Reader.CanRead(4))
				{
					Info.AttributeHighlights = Reader.ReadUInt16();
					Info.AttributeColors = Reader.ReadUInt16();
				}
			}
		}
	}
	if (Flags & FlagWeapon)
	{
		if (Reader.CanRead(60))
		{
			Info.bHasWeaponProfile = true;
			Info.DamageType = static_cast<int32>(Reader.ReadUInt32());
			Info.WeaponTime = static_cast<int32>(Reader.ReadUInt32());
			Info.WeaponSkill = Reader.ReadUInt32();
			Info.Damage = static_cast<int32>(Reader.ReadUInt32());
			Info.DamageVariance = static_cast<float>(Reader.ReadDouble());
			Info.WeaponDamageMod = Reader.ReadDouble();
			Info.WeaponLength = Reader.ReadDouble();
			Info.WeaponMaxVelocity = Reader.ReadDouble();
			Info.WeaponOffense = static_cast<float>(Reader.ReadDouble());
			Info.bWeaponMaxVelocityEstimated = Reader.ReadUInt32() != 0;
			Summary += FString::Printf(TEXT("Damage %d  Speed %d  Offense %.2f\n"),
				Info.Damage, Info.WeaponTime, Info.WeaponOffense);
		}
	}
	if (Flags & FlagHook)
	{
		if (Reader.CanRead(12)) { Reader.Skip(12); }
	}
	if (Flags & FlagArmorEnc) { if (Reader.CanRead(4)) Info.ArmorEnchantments = Reader.ReadUInt32(); }
	if (Flags & FlagWeaponEnc) { if (Reader.CanRead(4)) Info.WeaponEnchantments = Reader.ReadUInt32(); }
	if (Flags & FlagResistEnc) { if (Reader.CanRead(4)) Info.ResistanceEnchantments = Reader.ReadUInt32(); }
	if (Flags & FlagArmorLevels)
	{
		if (Reader.CanRead(36))
		{
			Info.ArmorLevels.SetNum(9);
			for (int32 i = 0; i < 9; ++i)
			{
				Info.ArmorLevels[i] = static_cast<int32>(Reader.ReadUInt32());
			}
			Summary += FString::Printf(
				TEXT("Armor Levels\n  Head %d  Chest %d  Abdomen %d\n  UpperArm %d  LowerArm %d  Hand %d\n  UpperLeg %d  LowerLeg %d  Foot %d\n"),
				Info.ArmorLevels[0], Info.ArmorLevels[1], Info.ArmorLevels[2],
				Info.ArmorLevels[3], Info.ArmorLevels[4], Info.ArmorLevels[5],
				Info.ArmorLevels[6], Info.ArmorLevels[7], Info.ArmorLevels[8]);
		}
	}

	if (Info.Name.IsEmpty()) { Info.Name = FString::Printf(TEXT("Object 0x%08X"), Info.ObjectGuid); }
	// Mag-nus ItemExamineUI hosts Value/Burden in dedicated chrome (ItemValueText /
	// ItemBurdenText) — keep them out of the body Summary so they are not duplicated.
	FString Header;
	if (Info.Level > 0) { Header += FString::Printf(TEXT("Level %d\n"), Info.Level); }
	if (Info.bIsCreature && Info.MaxHealth > 0)
	{
		Header += FString::Printf(TEXT("Health  %d / %d\n"), Info.Health, Info.MaxHealth);
	}
	if (Info.DamageRating || Info.DamageResistRating || Info.CritRating || Info.CritDamageRating
		|| Info.CritResistRating || Info.CritDamageResistRating)
	{
		Header += FString::Printf(
			TEXT("Ratings  Dmg %d  DR %d  Crit %d  CritDmg %d\n  CritResist %d  CritDmgResist %d\n"),
			Info.DamageRating, Info.DamageResistRating, Info.CritRating, Info.CritDamageRating,
			Info.CritResistRating, Info.CritDamageResistRating);
	}
	Info.Summary = (Header + Summary).TrimStartAndEnd();
	if (Info.bSuccess) if (auto* Object = WorldObjects.Find(Info.ObjectGuid))
	{
		// An empty stone may omit ItemCurMana. A successful full appraisal
		// replaces the previous resource reading, including explicit absence.
		const int32* Current = Info.IntProperties.Find(107), *Maximum = Info.IntProperties.Find(108);
		Object->ItemCurrentMana = Current ? FMath::Max(0, *Current) : -2;
		Object->ItemMaximumMana = Maximum ? FMath::Max(0, *Maximum) : -2;
		if (HasPlayerInventoryState(*Object)) ++InventoryDataRevision;
	}
	OnAppraisal.Broadcast(Info);
}

void FACESession::HandleViewContents(FACEBinaryReader& Reader)
{
	// GameEventViewContents: u32 containerGuid, u32 itemCount, then (u32 itemGuid, u32 containerType)*
	if (!Reader.CanRead(8))
	{
		return;
	}
	const int32 ContainerGuid = static_cast<int32>(Reader.ReadUInt32());
	const uint32 ItemCount = Reader.ReadUInt32();
	// A partial list must not revoke known contents, alter ownership, or cancel
	// closed-container expiry. Bound before reserving or changing any live state.
	if (!ContainerGuid || ItemCount > static_cast<uint32>(Reader.Remaining() / 8)) return;
	TArray<FACEContainerItemRef> Items;
	Items.Reserve(static_cast<int32>(ItemCount));
	for (uint32 i = 0; i < ItemCount; ++i)
	{
		FACEContainerItemRef Entry;
		Entry.ItemGuid = static_cast<int32>(Reader.ReadUInt32());
		Entry.ContainerType = static_cast<int32>(Reader.ReadUInt32());
		Items.Add(Entry);
		// Retail OnViewContents removes the newly accessible items from the
		// destruction queue, including descriptions that have not arrived yet.
		ClosedContainerItemDeadlines.Remove(Entry.ItemGuid);

		// Keep ObjectCreate records consistent when ViewContents arrives first or without Container flag.
		if (FACEWorldObject* Existing = WorldObjects.Find(Entry.ItemGuid))
		{
			if (Existing->ContainerId == 0)
			{
				Existing->ContainerId = ContainerGuid;
			}

		}
	}
	ContainerContents.FindOrAdd(ContainerGuid) = MoveTemp(Items);
	RestampContainerListPlacements(ContainerGuid);

	// Contents for a bag inside a corpse are metadata for the same open root,
	// not a second container-open event. Resolve through lists as well as objects
	// because ACE sends root/bag ViewContents before the corresponding creates.
	const int32 RootGuid = ResolveContainerRoot(ContainerGuid);
	bool bIsPlayerInventory = (RootGuid == PlayerGuid);
	// ViewContents is also unsolicited inventory metadata at login. Retail
	// ClientUISystem::OnViewContents only opens the requested ground object.
	// In particular, bag A and B may arrive before PlayerDescription and their
	// CreateObjects: treating B as a new ground container discarded A's order.
	const FACEWorldObject* RootObject = WorldObjects.Find(RootGuid);
	const bool bWorldContainer = RootObject && RootObject->bHasPosition && !RootObject->ContainerId;
	if (!bIsPlayerInventory)
	{
		TArray<FACEWorldObject> Packs;
		GetPlayerPacks(Packs);
		for (const FACEWorldObject& Pack : Packs)
		{
			if (Pack.Guid == ContainerGuid)
			{
				bIsPlayerInventory = true;
				break;
			}
		}
	}

	bool bIsVendor = false;
	if (const FACEWorldObject* ContainerObj = WorldObjects.Find(ContainerGuid))
	{
		bIsVendor = ContainerObj->IsVendor();
	}

	// Vendors use ApproachVendor (0x0062) for UI — don't open ExternalContainer for them.
	if (bIsVendor)
	{
		return;
	}

	if (!bIsPlayerInventory && (bWorldContainer || IsPendingWorldUse(RootGuid)))
	{
		if (OpenExternalContainerGuid && OpenExternalContainerGuid != RootGuid)
		{
			const int32 Previous = OpenExternalContainerGuid;
			OnCloseGroundContainer.Broadcast(Previous);
			ClearContainerContents(Previous);
		}
		OpenExternalContainerGuid = RootGuid;
		OnViewContentsExternal.Broadcast(RootGuid);
	}
}

void FACESession::HandleCloseGroundContainer(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 ContainerGuid = static_cast<int32>(Reader.ReadUInt32());
	// Close the presentation before discarding its list. Never leave clickable
	// loot behind after the server has revoked access, even when still in range.
	if (OpenExternalContainerGuid == ContainerGuid)
	{
		OpenExternalContainerGuid = 0;
	}
	OnCloseGroundContainer.Broadcast(ContainerGuid);
	ClearContainerContents(ContainerGuid);
}

void FACESession::ClearContainerContents(int32 ContainerGuid)
{
	// A bag contents packet can precede ownership at login. Dismissing that
	// presentation must not discard the now-owned bag's authoritative item list.
	if (PlayerGuid && ResolveContainerRoot(ContainerGuid) == PlayerGuid) {ReconcileOpenContainerOwnership();return;}
	// Presentation closure must also end local access without a server close.
	if (OpenExternalContainerGuid == ContainerGuid) OpenExternalContainerGuid = 0;
	if (ContainerGuid != 0)
	{
		TArray<FACEContainerItemRef> Contents;
		ContainerContents.RemoveAndCopyValue(ContainerGuid, Contents);
		for (const auto& Ref : Contents)
		{
			if (!Ref.ItemGuid || Ref.ItemGuid == ContainerGuid || Ref.ItemGuid == PlayerGuid) continue;
			// ACE deliberately sends no ObjectDelete for leftover loot on close.
			// Match ClientUISystem::SetGroundObject / CObjectMaint::AddObjectToBeDestroyed
			// instead of retaining every corpse's descriptors for the entire login.
			const double Deadline = FPlatformTime::Seconds() + 25.;
			ClosedContainerItemDeadlines.Add(Ref.ItemGuid, Deadline);
			if (!NextClosedContainerExpiryAt || Deadline < NextClosedContainerExpiryAt) NextClosedContainerExpiryAt = Deadline;
			if (Ref.ContainerType != 0) ClearContainerContents(Ref.ItemGuid);
		}
	}
}

void FACESession::ExpireClosedContainerItems(double Now)
{
	if (ClosedContainerItemDeadlines.IsEmpty() || Now < NextClosedContainerExpiryAt) return;
	NextClosedContainerExpiryAt = 0.;
	TArray<int32> Expired;
	for (auto It = ClosedContainerItemDeadlines.CreateIterator(); It; ++It)
	{
		if (Now < It.Value())
		{
			if (!NextClosedContainerExpiryAt || It.Value() < NextClosedContainerExpiryAt) NextClosedContainerExpiryAt = It.Value();
			continue;
		}
		const int32 Guid = It.Key();
		It.RemoveCurrent();
		const auto* Object = WorldObjects.Find(Guid);
		if (!Object) continue;
		// UIQueue ownership may arrive after SmartboxQueue closes/deletes the
		// corpse. Recheck live ownership, including whole bags and equipped items,
		// before forgetting anything. A refreshed world drop also remains visible.
		const int32 Root = ResolveContainerRoot(Guid);
		if ((PlayerGuid && (Root == PlayerGuid || Object->WielderId == PlayerGuid || Object->ParentGuid == PlayerGuid))
			|| TradeSelfItems.Contains(Guid) || TradePartnerItems.Contains(Guid)
			|| (!Object->ContainerId && (Object->bHasPosition || Object->ParentGuid || Object->WielderId))) continue;
		Expired.Add(Guid);
	}
	// Evaluate every ownership chain before deleting parents. Broadcast ordinary
	// object removal so selection, presentation and plugin appraisal caches retire.
	for (int32 Guid : Expired) DeleteWorldObject(Guid);
}

bool FACESession::CanVendorBuyItem(const FACEWorldObject& Item) const
{
	// Retail VendorProfile::InqAcceptability uses the per-unit public value.
	if ((VendorItemTypes & static_cast<uint32>(Item.ItemType)) == 0
		|| (Item.ObjectDescriptionFlags & ACEObjectDescFlag::Retained) != 0) return false;
	const int32 UnitValue = Item.Value / FMath::Max(1, Item.StackSize);
	if (UnitValue <= 0) return false;
	if (VendorMaxValue != -1 && UnitValue > VendorMaxValue)
		return (Item.ItemType & ACEItemType::PromissoryNote) != 0;
	return VendorMinValue == -1 || UnitValue >= VendorMinValue;
}

void FACESession::HandleApproachVendor(FACEBinaryReader& Reader)
{
	// GameEventApproachVendor: vendorGuid + merchandise header + GameDataOnly item list.
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 VendorGuid = static_cast<int32>(Reader.ReadUInt32());
	OpenVendorGuid = VendorGuid;
	VendorMerchandise.Reset();
	VendorBuyRate = 1.f;
	VendorItemTypes = MAX_uint32;
	VendorMinValue = VendorMaxValue = -1;
	VendorSellRate = 1.f;

	// merchandiseItemTypes, minValue, maxValue, dealMagic, buyPrice, sellPrice, altCurrencyWcid
	VendorCurrencyWeenie = 0;
	if (Reader.CanRead(28))
	{
		VendorItemTypes = Reader.ReadUInt32();
		VendorMinValue = static_cast<int32>(Reader.ReadUInt32());
		VendorMaxValue = static_cast<int32>(Reader.ReadUInt32());
		Reader.ReadUInt32(); // DealMagicalItems
		VendorBuyRate = Reader.ReadFloat();
		VendorSellRate = Reader.ReadFloat();
		VendorCurrencyWeenie = Reader.ReadUInt32();
	}
	VendorCurrencyName.Reset(); VendorCurrencyCount = 0;
	VendorPurseRevision = MAX_uint64;
	// altCurrencyCount
	if (Reader.CanRead(4))
	{
		VendorCurrencyCount = Reader.ReadUInt32();
	}
	// altCurrency plural name (String16L)
	if (Reader.CanRead(4))
	{
		VendorCurrencyName = Reader.ReadString16L();
	}

	if (!Reader.CanRead(4))
	{
		OnApproachVendor.Broadcast(VendorGuid);
		return;
	}
	const int32 NumItems = static_cast<int32>(Reader.ReadUInt32());
	TArray<FACEContainerItemRef>& Contents = ContainerContents.FindOrAdd(VendorGuid);
	Contents.Reset();
	for (int32 i = 0; i < NumItems && Reader.CanRead(4); ++i)
	{
		// packed: (stackSize & 0xFFFFFF) | (pwdType << 24); pwdType always -1 (PublicWeenieDesc).
		const uint32 Supply = Reader.ReadUInt32() & 0x00FFFFFFu;
		FACEDecodedObject Decoded;
		if (!FACEObjectCreateParser::ParseGameDataOnly(Reader, Decoded) || !Decoded.bParseOk)
		{
			break;
		}
		FACEWorldObject Obj;
		Obj.bPhysicsDescriptionPending = true; // PublicWeenieDesc has no physics/model payload.
		Obj.Guid = Decoded.Guid;
		Obj.Name = Decoded.Name;
		Obj.PluralName = Decoded.PluralName;
		Obj.CreateFlags1 = Decoded.CreateFlags1; Obj.CreateFlags2 = Decoded.CreateFlags2;
		Obj.PublicPhysicsScript = Decoded.PublicPhysicsScript;
		Obj.WeenieClassId = Decoded.WeenieClassId;
		Obj.IconId = Decoded.IconId;
		Obj.IconOverlayId = Decoded.IconOverlayId;
		Obj.IconUnderlayId = Decoded.IconUnderlayId;
		Obj.CooldownId = Decoded.CooldownId;
		Obj.CooldownDuration = Decoded.CooldownDuration;
		Obj.UiEffects = static_cast<int32>(Decoded.UiEffects);
		Obj.ItemType = Decoded.ItemType;
		Obj.bIsHouseHook = Decoded.bIsHouseHook;
		Obj.HookItemTypes = Decoded.HookItemTypes;
		Obj.HookType = Decoded.HookType;
		Obj.HouseOwnerId = Decoded.HouseOwnerId;
		Obj.Value = Decoded.Value;
		Obj.Burden = Decoded.Burden;
		// Retail gmVendorUI::BuySingleItem requests one when the shop entry has
		// no positive stack quantity. Custom ACE create lists can advertise zero;
		// this does not make the entry unbuyable. Normalize at receipt so desktop,
		// VR and automation share that single-item fallback (not unlimited stock).
		Obj.VendorQuantityAvailable = Supply == 0x00FFFFFFu ? -1 : FMath::Max(1, static_cast<int32>(Supply));
		Obj.StackSize = Decoded.StackSize;
		Obj.MaxStackSize = Decoded.MaxStackSize;
		Obj.Structure = Decoded.Structure;
		Obj.MaxStructure = Decoded.MaxStructure;
		Obj.ItemUseable = Decoded.ItemUseable;
		Obj.TargetType = Decoded.TargetType;
		Obj.MaterialType = Decoded.MaterialType;
		Obj.SalvageWorkmanship = Decoded.SalvageWorkmanship;
		Obj.ValidLocations = static_cast<int64>(Decoded.ValidLocations);
		Obj.ObjectDescriptionFlags = Decoded.ObjectDescriptionFlags;
		Obj.PetOwnerId = Decoded.PetOwnerId;
		Obj.ContainerId = VendorGuid;
		// gmVendorUI::OpenVendor removes each advertised item from the deferred
		// destruction queue. A previous shop/corpse close must not erase this
		// renewed description while its merchandise icon is still displayed.
		ClosedContainerItemDeadlines.Remove(Obj.Guid);
		WorldObjects.Add(Obj.Guid, Obj);
		VendorMerchandise.Add(Obj);
		FACEContainerItemRef Ref;
		Ref.ItemGuid = Obj.Guid;
		Ref.ContainerType = 0;
		Contents.Add(Ref);
	}

	OnApproachVendor.Broadcast(VendorGuid);
}

void FACESession::HandleRegisterTrade(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(16))
	{
		return;
	}
	const int32 InitiatorGuid = static_cast<int32>(Reader.ReadUInt32());
	const int32 PartnerGuid = static_cast<int32>(Reader.ReadUInt32());
	const double Stamp = Reader.ReadDouble();
	if (!FMath::IsFinite(Stamp)) return;
	TradeStamp = Stamp;
	bTradeSelfAccepted = bTradePartnerAccepted = false;
	TradeSelfItems.Reset();
	TradeRemovedItems.Reset();
	TradePartnerItems.Reset();
	TradeInitiatorGuid = InitiatorGuid;
	if (InitiatorGuid == PlayerGuid)
	{
		TradePartnerGuid = PartnerGuid;
	}
	else
	{
		TradePartnerGuid = InitiatorGuid;
	}
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::RegisterTrade));
}

void FACESession::HandleOpenTrade(FACEBinaryReader& Reader)
{
	if (Reader.CanRead(4))
	{
		const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
		if (TradePartnerGuid == 0 && Guid != 0 && Guid != PlayerGuid)
		{
			TradePartnerGuid = Guid;
		}
	}
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::OpenTrade));
}

void FACESession::HandleCloseTrade(FACEBinaryReader& Reader)
{
	if (Reader.CanRead(4))
	{
		Reader.ReadUInt32(); // EndTradeReason
	}
	TradePartnerGuid = 0;
	TradeInitiatorGuid = 0;
	bTradeSelfAccepted = bTradePartnerAccepted = false;
	TradeStamp = 0.0;
	TradeSelfItems.Reset();
	TradeRemovedItems.Reset();
	TradePartnerItems.Reset();
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::CloseTrade));
}

void FACESession::HandleAcceptTradeEvent(FACEBinaryReader& Reader)
{
	if (!TradePartnerGuid || !Reader.CanRead(4)) return;
	const int32 Source = static_cast<int32>(Reader.ReadUInt32());
	if (Source == PlayerGuid) bTradeSelfAccepted = true;
	else if (Source == TradePartnerGuid) bTradePartnerAccepted = true;
	else if (Source == 0) bTradeSelfAccepted = false; // retail acknowledgement reset
	else return;
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::AcceptTrade));
}

void FACESession::HandleDeclineTradeEvent(FACEBinaryReader& Reader)
{
	if (!TradePartnerGuid || !Reader.CanRead(4)) return;
	const int32 Source = static_cast<int32>(Reader.ReadUInt32());
	if (Source == PlayerGuid) bTradeSelfAccepted = false;
	else if (Source == TradePartnerGuid) bTradePartnerAccepted = false;
	else return;
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::DeclineTrade));
}

void FACESession::HandleResetTradeEvent(FACEBinaryReader& Reader)
{
	if (Reader.CanRead(4))
	{
		Reader.ReadUInt32(); // whoReset
	}
	// Retail: resetting the trade clears both accept flags.
	bTradeSelfAccepted = bTradePartnerAccepted = false;
	TradeSelfItems.Reset();
	TradeRemovedItems.Reset();
	TradePartnerItems.Reset();
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::ResetTrade));
}

void FACESession::HandleAddToTradeEvent(FACEBinaryReader& Reader)
{
	// objectGuid, tradeSide (1=self, 2=partner), location
	if (!Reader.CanRead(12))
	{
		return;
	}
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	const int32 TradeSide = Reader.ReadInt32();
	const uint32 Location = Reader.ReadUInt32();
	if (!TradePartnerGuid || !ItemGuid || (TradeSide != 1 && TradeSide != 2)
		|| TradeSelfItems.Contains(ItemGuid) || TradePartnerItems.Contains(ItemGuid)) return;
	TArray<int32>& Side = (TradeSide == 2) ? TradePartnerItems : TradeSelfItems;
	if (Side.Num() >= 0x1A0A) return; // retail Trade::AddItem bound
	if (TradeSide != 2) TradeRemovedItems.Remove(ItemGuid);
	Side.Insert(ItemGuid, static_cast<int32>(FMath::Min(Location, static_cast<uint32>(Side.Num()))));
	bTradeSelfAccepted = bTradePartnerAccepted = false;
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::AddToTrade));
}

void FACESession::HandleRemoveFromTradeEvent(FACEBinaryReader& Reader)
{
	// objectGuid, tradeSide
	if (!TradePartnerGuid || !Reader.CanRead(8)) return;
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	const int32 TradeSide = Reader.ReadInt32();
	if (TradeSide != 1 && TradeSide != 2) return;
	TArray<int32>& Side = (TradeSide == 2) ? TradePartnerItems : TradeSelfItems;
	if (TradeSide == 1) TradeRemovedItems.Remove(ItemGuid);
	if (!Side.Remove(ItemGuid)) return;
	bTradeSelfAccepted = bTradePartnerAccepted = false;
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::RemoveFromTrade));
}

void FACESession::HandleTradeFailure(FACEBinaryReader& Reader)
{
	if (!TradePartnerGuid || !Reader.CanRead(8)) return;
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	Reader.ReadUInt32(); // reason
	// Retail removes a rejected item from the self offer.
	if (TradeSelfItems.Remove(ItemGuid)) bTradeSelfAccepted = bTradePartnerAccepted = false;
	TradeRemovedItems.Remove(ItemGuid);
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::TradeFailure));
}

void FACESession::HandleClearTradeAcceptance(FACEBinaryReader& Reader)
{
	(void)Reader;
	// ACE clears ItemsInTradeWindow when broadcasting ClearTradeAcceptance.
	bTradeSelfAccepted = bTradePartnerAccepted = false;
	TradeSelfItems.Reset();
	TradeRemovedItems.Reset();
	TradePartnerItems.Reset();
	OnTradeEvent.Broadcast(static_cast<int32>(ACEGameEvent::ClearTradeAcceptance));
}

void FACESession::HandleCharacterTitle(FACEBinaryReader& Reader)
{
	// status(1), displayTitleId, numTitles, titleId[]
	if (!Reader.CanRead(12))
	{
		return;
	}
	Reader.ReadUInt32(); // status
	DisplayTitleId = Reader.ReadUInt32();
	const uint32 Num = Reader.ReadUInt32();
	CharacterTitleIds.Reset();
	CharacterTitleIds.Reserve(static_cast<int32>(Num));
	for (uint32 i = 0; i < Num && Reader.CanRead(4); ++i)
	{
		CharacterTitleIds.Add(Reader.ReadUInt32());
	}
	OnCharacterTitlesChanged.Broadcast();
}

void FACESession::HandleUpdateTitle(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(8))
	{
		return;
	}
	const uint32 TitleId = Reader.ReadUInt32();
	const uint32 bSetDisplay = Reader.ReadUInt32();
	if (!CharacterTitleIds.Contains(TitleId))
	{
		CharacterTitleIds.Add(TitleId);
	}
	if (bSetDisplay != 0)
	{
		DisplayTitleId = TitleId;
	}
	OnCharacterTitlesChanged.Broadcast();
}

bool FACESession::IsCursorBusy() const
{
	if (State!=EACESessionState::InWorld) return false;
	const double Now=FPlatformTime::Seconds();
	return CursorAttackUntil>Now || CursorAppraisalUntil>Now || CursorAllegianceUntil>Now
		|| CursorUseWaits.ContainsByPredicate([Now](const auto& Wait){return Wait.Value>Now;});
}

void FACESession::ReleaseCursorUse(bool ItemOnly)
{
	const int32 Index=CursorUseWaits.IndexOfByPredicate([ItemOnly](const auto& Wait)
		{return !ItemOnly||Wait.Key==ACEGameAction::Use||Wait.Key==ACEGameAction::UseWithTarget;});
	if (Index!=INDEX_NONE) CursorUseWaits.RemoveAt(Index);
}

void FACESession::ResetCursorWaits()
{
	CursorUseWaits.Reset();
	CursorAttackUntil=CursorAppraisalUntil=CursorAllegianceUntil=0.;
	CursorAppraisalGuid=0;
}

void FACESession::BeginPendingUse(int32 Source, int32 Target)
{
	bUseBusy = true;
	UseStartedAt = FPlatformTime::Seconds();
	UseSourceGuid = Source;
	UseTargetGuid = Target;
	OnItemUseRequested.Broadcast(Source,Target);
}

void FACESession::ClearPendingUse()
{
	bUseBusy = false;
	UseStartedAt = 0.0;
	UseSourceGuid = UseTargetGuid = 0;
}

void FACESession::CancelPendingUse(const TCHAR* Reason)
{
	if (!bUseBusy) return;
	ReleaseCursorUse(true);
	Log(FString::Printf(TEXT("Release pending use (%s) source=0x%08X target=0x%08X age=%.1fs"),
		Reason, UseSourceGuid, UseTargetGuid, FPlatformTime::Seconds()-UseStartedAt));
	ClearPendingUse();
	// Release controller approach/freeze state too, without fabricating a server error.
	OnUseDone.Broadcast(0);
}

void FACESession::CancelWorldUseApproach()
{
	const FACEWorldObject* Source = WorldObjects.Find(UseSourceGuid);
	if (UseTargetGuid == 0 && Source && Source->bHasPosition
		&& Source->ContainerId == 0 && Source->WielderId == 0 && Source->Guid != PlayerGuid)
		CancelPendingUse(TEXT("world approach cancelled"));
}

void FACESession::CheckPendingUseTimeout(double Now)
{
	// A missing UseDone must not lock every later door/NPC interaction until
	// relog. Do not replay the use: quests, consumables and gifts have side effects.
	// This releases only our UI gate; the server still validates each new action.
	if (!bUseBusy || Now-UseStartedAt < 30.0) return;
	CancelPendingUse(TEXT("completion timeout"));
	EmitChatMessage(TEXT("The interaction did not finish. Please try again."), TEXT(""), ACEChatMessageType::TransientInfo);
}

void FACESession::HandleUseDone(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(4)) return;
	const uint32 Err = Reader.ReadUInt32();
	ReleaseCursorUse();
	ClearPendingUse();
	OnUseDone.Broadcast(Err);
	if (Err != 0)
	{
		const FString Msg = ACECombatChat::LookupWeenieError(Err);
		if (!Msg.IsEmpty())
		{
			EmitChatMessage(Msg, TEXT(""), ACECombatChat::WeenieErrorChatType(Err));
		}
	}
}

void FACESession::HandleItemAppraiseDone(FACEBinaryReader& Reader)
{
	(void)Reader;
	// Retail unlocks appraisal UI; IdentifyObjectResponse already delivered the payload.
}

const TArray<FACEContainerItemRef>* FACESession::GetContainerContents(int32 ContainerGuid) const
{
	return ContainerContents.Find(ContainerGuid);
}

void FACESession::RemoveFromContainerLists(int32 ItemGuid)
{
	for (TPair<int32, TArray<FACEContainerItemRef>>& Pair : ContainerContents)
	{
		Pair.Value.RemoveAll([ItemGuid](const FACEContainerItemRef& Ref)
		{
			return Ref.ItemGuid == ItemGuid;
		});
	}
}

bool FACESession::IsPackSlotItem(const FACEWorldObject& Obj)
{
	return Obj.ItemsCapacity > 0 || Obj.ContainersCapacity > 0
		|| (Obj.ItemType & ACEItemType::Container) != 0
		|| (Obj.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot) != 0;
}

int32 FACESession::ContentsInsertIndexForPlacement(const TArray<FACEContainerItemRef>& Contents,
	int32 Placement, int32 ContainerType)
{
	const bool bWantPack = ContainerType != 0;
	if (Placement < 0)
	{
		return Contents.Num();
	}
	int32 MatchingSeen = 0;
	for (int32 i = 0; i < Contents.Num(); ++i)
	{
		const bool bPack = Contents[i].ContainerType != 0;
		if (bPack != bWantPack)
		{
			continue;
		}
		if (MatchingSeen == Placement)
		{
			return i;
		}
		++MatchingSeen;
	}
	for (int32 i = Contents.Num() - 1; i >= 0; --i)
	{
		if ((Contents[i].ContainerType != 0) == bWantPack)
		{
			return i + 1;
		}
	}
	return Contents.Num();
}

void FACESession::ShiftContainerPlacementsAfterRemove(int32 ContainerGuid, int32 RemovedPlacement,
	bool bPackSlots)
{
	if (ContainerGuid == 0 || RemovedPlacement < 0)
	{
		return;
	}
	for (TPair<int32, FACEWorldObject>& Pair : WorldObjects)
	{
		FACEWorldObject& Obj = Pair.Value;
		// ContainerId remains authoritative until the ordered inventory receipt,
		// even if a physics attachment for another pending equip arrived first.
		if (Obj.ContainerId != ContainerGuid)
		{
			continue;
		}
		if (IsPackSlotItem(Obj) != bPackSlots)
		{
			continue;
		}
		if (Obj.PlacementPosition > RemovedPlacement)
		{
			--Obj.PlacementPosition;
		}
	}
}

void FACESession::ShiftContainerPlacementsForInsert(int32 ContainerGuid, int32 ItemGuid,
	int32 Placement, bool bPackSlots)
{
	if (ContainerGuid == 0 || ItemGuid == 0)
	{
		return;
	}
	int32 Count = 0;
	for (const auto& Pair : WorldObjects)
	{
		const auto& Other = Pair.Value;
		if (Other.Guid != ItemGuid && Other.ContainerId == ContainerGuid
			&& IsPackSlotItem(Other) == bPackSlots) ++Count;
	}
	if (const auto* Refs = ContainerContents.Find(ContainerGuid))
    {
        int32 RefCount = 0;
        for (const auto& Ref : *Refs)
            if (Ref.ItemGuid != ItemGuid && (Ref.ContainerType != 0) == bPackSlots) ++RefCount;
        Count = FMath::Max(Count, RefCount);
    }
    const int32 Place = FMath::Clamp(Placement, 0, Count);
	for (TPair<int32, FACEWorldObject>& Pair : WorldObjects)
	{
		FACEWorldObject& Obj = Pair.Value;
		if (Obj.Guid == ItemGuid)
		{
			continue;
		}
		if (Obj.ContainerId != ContainerGuid)
		{
			continue;
		}
		if (IsPackSlotItem(Obj) != bPackSlots)
		{
			continue;
		}
		if (Obj.PlacementPosition >= Place)
		{
			++Obj.PlacementPosition;
		}
	}
	if (FACEWorldObject* Moved = WorldObjects.Find(ItemGuid))
	{
		Moved->ContainerId = ContainerGuid;
		Moved->PlacementPosition = Place;
	}
}

void FACESession::NormalizeContainerPlacements(int32 ContainerGuid, bool bPackSlots,
	int32 InsertGuid, int32 InsertAt)
{
	if (ContainerGuid == 0)
	{
		return;
	}
	TArray<FACEWorldObject*> Items;
	FACEWorldObject* Inserted = nullptr;
	for (TPair<int32, FACEWorldObject>& Pair : WorldObjects)
	{
		FACEWorldObject& Obj = Pair.Value;
		if (Obj.ContainerId != ContainerGuid
			|| Obj.CurrentWieldedLocation != 0 || Obj.WielderId != 0 || Obj.ParentGuid != 0)
		{
			continue;
		}
		if (IsPackSlotItem(Obj) != bPackSlots)
		{
			continue;
		}
		if (InsertGuid != 0 && Obj.Guid == InsertGuid)
		{
			Inserted = &Obj;
			continue;
		}
		Items.Add(&Obj);
	}
	// TArray<T*>::Sort dereferences pointers before invoking the predicate.
	Items.Sort([](const FACEWorldObject& A, const FACEWorldObject& B)
	{
		const int32 Pa = A.PlacementPosition >= 0 ? A.PlacementPosition : MAX_int32;
		const int32 Pb = B.PlacementPosition >= 0 ? B.PlacementPosition : MAX_int32;
		if (Pa != Pb)
		{
			return Pa < Pb;
		}
		return A.Guid < B.Guid;
	});
	if (Inserted)
	{
		const int32 Ins = (InsertAt >= 0) ? FMath::Clamp(InsertAt, 0, Items.Num()) : Items.Num();
		Items.Insert(Inserted, Ins);
	}
	for (int32 i = 0; i < Items.Num(); ++i)
	{
		Items[i]->PlacementPosition = i;
	}
}

void FACESession::ApplyPlayerInventoryProfile()
{
    if (!PlayerGuid) return;
    if (auto* Pending = ContainerContents.Find(0))
    {
        auto Items = MoveTemp(*Pending);
        ContainerContents.Remove(0);
        ContainerContents.Add(PlayerGuid, MoveTemp(Items));
    }
    RestampContainerListPlacements(PlayerGuid);
    for (const auto& Entry : LoginEquipment)
    {
        const bool bNewStub = !WorldObjects.Contains(Entry.Key);
        FACEWorldObject& Obj = WorldObjects.FindOrAdd(Entry.Key);
        if (bNewStub) Obj.bPhysicsDescriptionPending = true;
        Obj.Guid = Entry.Key;
        Obj.ContainerId = 0;
        Obj.WielderId = PlayerGuid;
        Obj.CurrentWieldedLocation = Entry.Value.Key;
        Obj.ClothingPriority = Entry.Value.Value;
        Obj.PlacementPosition = INDEX_NONE;
    }
    LoginEquipment.Reset();
}

int32 FACESession::ResolveContainerRoot(int32 ContainerGuid) const
{
	TSet<int32> Seen;
	while (ContainerGuid && !Seen.Contains(ContainerGuid) && Seen.Num() < 64)
	{
		if (PlayerGuid && ContainerGuid == PlayerGuid) return PlayerGuid;
		Seen.Add(ContainerGuid);
		int32 Parent = 0;
		if (const auto* Object = WorldObjects.Find(ContainerGuid)) Parent = Object->ContainerId;
		if (!Parent) for (const auto& List : ContainerContents)
			if (List.Key != ContainerGuid && List.Value.ContainsByPredicate([&](const FACEContainerItemRef& Ref){return Ref.ItemGuid == ContainerGuid;}))
			{Parent = List.Key;break;}
		if (!Parent) break;
		ContainerGuid = Parent;
	}
	return ContainerGuid;
}

void FACESession::ReconcileOpenContainerOwnership()
{
	if (!OpenExternalContainerGuid || !PlayerGuid || ResolveContainerRoot(OpenExternalContainerGuid) != PlayerGuid) return;
	const int32 OldContainer = OpenExternalContainerGuid;
	OpenExternalContainerGuid = 0;
	// ClearContainerContents preserves owned lists if the UI close calls it.
	OnCloseGroundContainer.Broadcast(OldContainer);
}

void FACESession::RestampContainerListPlacements(int32 ContainerGuid)
{
	TArray<FACEContainerItemRef>* Contents = ContainerContents.Find(ContainerGuid);
	if (!Contents)
	{
		return;
	}
	int32 ItemPlace = 0;
	int32 PackPlace = 0;
	for (const FACEContainerItemRef& Ref : *Contents)
	{
		FACEWorldObject* Obj = WorldObjects.Find(Ref.ItemGuid);
		const bool bPack = Ref.ContainerType != 0 || (Obj && IsPackSlotItem(*Obj));
		const int32 Placement = bPack ? PackPlace++ : ItemPlace++;
		// Unknown objects still occupy their wire-list position. ObjectCreate may
		// arrive after this authoritative contents snapshot.
		if (Obj && ContainerGuid != 0)
        {
            Obj->PlacementPosition = Placement;
            Obj->ContainerId = ContainerGuid;
            Obj->WielderId = Obj->ParentGuid = 0;
            Obj->CurrentWieldedLocation = 0;
            Obj->ParentLocation = 0;
        }
	}
	ReconcileOpenContainerOwnership();
}

void FACESession::ApplyPropertyDataID(int32 ObjectGuid, uint32 PropertyId, uint32 Value)
{
	FACEWorldObject* Obj = WorldObjects.Find(ObjectGuid);
	if (!Obj) return;
	bool bChanged = false;
	const auto Assign = [&bChanged](auto& Field, auto NewValue)
	{
		if (Field != NewValue) { Field = NewValue; bChanged = true; }
	};
	// ACCWeenieObject::OnStatUpdated(DataID) accepts zero too: an explicit zero
	// clears an icon layer. SpellDID also drives the held caster's innate spell.
	switch (PropertyId)
	{
	case 8: // PropertyDataId.Icon
		Assign(Obj->IconId, static_cast<int32>(Value));
		break;
	case 28: // PropertyDataId.Spell — wand innate / item SpellDID
		Assign(Obj->SpellDID, static_cast<int32>(Value));
		break;
	case 44: // PropertyDataId.PhysicsScript (PublicWeenieDesc, not PhysicsDesc)
		Assign(Obj->PublicPhysicsScript, static_cast<uint16>(Value));
		break;
	case 50: // PropertyDataId.IconOverlay
		Assign(Obj->IconOverlayId, static_cast<int32>(Value));
		break;
	case 52: // PropertyDataId.IconUnderlay
		Assign(Obj->IconUnderlayId, static_cast<int32>(Value));
		break;
	default:
		break;
	}
	// UI reads these fields directly; Lua/UCM caches inventory by this revision.
	// Do not re-create actors or invalidate successful appraisals for icon changes.
	if (bChanged && (Obj->ContainerId || (PlayerGuid &&
		(Obj->WielderId == PlayerGuid || Obj->ParentGuid == PlayerGuid)))) ++InventoryDataRevision;
}

void FACESession::HandleInventoryPutObjInContainer(FACEBinaryReader& Reader)
{
	++InventoryDataRevision;
	// GameEventItemServerSaysContainId: itemGuid, containerGuid, placement, containerType.
	if (!Reader.CanRead(16))
	{
		return;
	}
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	const int32 ContainerGuid = static_cast<int32>(Reader.ReadUInt32());
	const int32 Placement = Reader.ReadInt32();
	const int32 ContainerType = Reader.ReadInt32();
	PendingInventoryTransfers.Remove(ItemGuid);
	EquipmentRemovalEvidence.Remove(ItemGuid);

	int32 PrevContainer = 0;
	int32 PrevPlacement = INDEX_NONE;
	bool bPackSlots = ContainerType != 0;
	if (FACEWorldObject* Existing = WorldObjects.Find(ItemGuid))
	{
		PrevContainer = Existing->ContainerId;
		PrevPlacement = Existing->PlacementPosition;
		bPackSlots = ContainerType != 0 || IsPackSlotItem(*Existing);
	}

	FACEWorldObject* Obj = WorldObjects.Find(ItemGuid);
	if (!Obj)
	{
		// ContainId can arrive after we forgot a foreign object's world entry; keep a stub for UI.
		FACEWorldObject Stub;
		Stub.Guid = ItemGuid;
		Stub.bPhysicsDescriptionPending = true;
		WorldObjects.Add(ItemGuid, Stub);
		Obj = WorldObjects.Find(ItemGuid);
	}
	// Always drop prior container membership (corpse/chest/pack) before re-homing.
	RemoveFromContainerLists(ItemGuid);
	Obj->CurrentWieldedLocation = 0;
	Obj->WielderId = 0;
	Obj->ParentGuid = 0;
	Obj->ParentLocation = 0;
	Obj->ContainerId = 0;
	if (PrevContainer != 0)
	{
		ShiftContainerPlacementsAfterRemove(PrevContainer, PrevPlacement, bPackSlots);
	}
	ShiftContainerPlacementsForInsert(ContainerGuid, ItemGuid, Placement, bPackSlots);

	// Item is now in a container — never leave a freestanding world mesh behind.
	RemoveObjectFromWorldVisual(ItemGuid);

	// Keep ContainerContents in sync when a list already exists (PlayerDescription /
	// ViewContents). Never FindOrAdd a side-pack list here — a 1-item invented list would
	// hide the rest of that pack's ObjectCreate inventory in GetPackItems. Main pack
	// GetPackItems uses the ObjectCreate ContainerId scan and does not need this list.
	if (TArray<FACEContainerItemRef>* Contents = ContainerContents.Find(ContainerGuid))
	{
		FACEContainerItemRef Ref;
		Ref.ItemGuid = ItemGuid;
		Ref.ContainerType = ContainerType;
		Contents->Insert(Ref,
			ContentsInsertIndexForPlacement(*Contents, Placement, Ref.ContainerType));
	}
    if (PrevContainer) RestampContainerListPlacements(PrevContainer);
    RestampContainerListPlacements(ContainerGuid);
	if (!PendingEquipmentRemovals.IsEmpty() && ItemGuid == PendingEquipmentRemovals[0])
	{
		if (ContainerGuid && ResolveContainerRoot(ContainerGuid) == PlayerGuid)
		{
			PendingEquipmentRemovals.RemoveAt(0);
			AdvanceEquipmentSwap();
		}
		else CancelEquipmentSwap();
	}

}

void FACESession::HandleWieldItem(FACEBinaryReader& Reader)
{
	++InventoryDataRevision;
	// GameEventWieldItem: itemGuid, wield location (EquipMask).
	if (!Reader.CanRead(8))
	{
		return;
	}
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	const int64 Location = static_cast<int64>(Reader.ReadInt32());
	EquipmentRemovalEvidence.Remove(ItemGuid);
	// The ordered receipt changes only this item. Other equipment has its own
	// ContainID/removal receipt; never guess that it was moved to the main pack.
	constexpr int64 HeldVisual = ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded
		| ACEEquipMask::MissileWeapon | ACEEquipMask::Held | ACEEquipMask::Shield;
	// A UI receipt can precede the item's Smartbox description, just as at login.
	if (!WorldObjects.Contains(ItemGuid))
	{
		FACEWorldObject& Stub = WorldObjects.Add(ItemGuid);
		Stub.Guid = ItemGuid;
		Stub.bPhysicsDescriptionPending = true;
	}
	if (FACEWorldObject* Obj = WorldObjects.Find(ItemGuid))
	{
		// ACCWeenieObject::ServerSaysMoveItem removes the source ID before
		// changing ownership. Keep the remaining pack indices dense, just as
		// RemoveContent/IDList::RemoveByID does in retail.
		const int32 PrevContainer = Obj->ContainerId;
		const int32 PrevPlacement = Obj->PlacementPosition;
		const bool bPackSlots = IsPackSlotItem(*Obj);
		Obj->CurrentWieldedLocation = Location;
		Obj->WielderId = PlayerGuid;
		Obj->ContainerId = 0;
		Obj->PlacementPosition = INDEX_NONE;
		RemoveFromContainerLists(ItemGuid);
		ShiftContainerPlacementsAfterRemove(PrevContainer, PrevPlacement, bPackSlots);
		RestampContainerListPlacements(PrevContainer);

		// Held/selectable gear needs Physics Parent + ParentLocation for the 3D attach.
		// Clothing/armor stay ParentLocation.None and update the wearer via ObjDescEvent.
		// Equipping an ammo stack does not attach a visible arrow. The server's
		// reload ParentEvent supplies its hand and placement once it is nocked.
		const bool bAmmoHasParent = (Location & ACEEquipMask::MissileAmmo) != 0
			&& Obj->ParentGuid == PlayerGuid && Obj->ParentLocation != 0;
		if ((Location & HeldVisual) != 0 || bAmmoHasParent)
		{
			Obj->ParentGuid = PlayerGuid;
			const bool bInferParent = Obj->ParentLocation == 0;
			if (bInferParent)
			{
				// Match ACE Creature_Equipment.GetPlacementLocation until ParentEvent.
				if ((Location & (ACEEquipMask::MeleeWeapon | ACEEquipMask::Held | ACEEquipMask::TwoHanded)) != 0)
				{
					Obj->ParentLocation = 1; // RightHand
				}
				else if ((Location & ACEEquipMask::Shield) != 0)
				{
					if ((Obj->ItemType & ACEItemType::Armor) != 0)
					{
						Obj->ParentLocation = 3; // Shield
					}
					else
					{
						Obj->ParentLocation = 8; // LeftWeapon (weapon in shield slot)
					}
				}
				else if ((Location & ACEEquipMask::MissileWeapon) != 0)
				{
					const int32 Style = ACECombatStance::InferCombatStyle(*Obj);
					const bool bBowLike = (Style & 0x00010) != 0 || (Style & 0x00020) != 0; // Bow | Crossbow
					Obj->ParentLocation = bBowLike ? 2 : 1; // LeftHand vs RightHand
				}
			}
			if (Obj->ParentLocation != 0)
			{
				const int32 HeldPlacement = ACEPlacementFromParentLocation(Obj->ParentLocation);
				if (HeldPlacement != 0 && bInferParent)
				{
					Obj->PlacementId = HeldPlacement;
				}
				FACEWorldObject AttachmentUpdate = *Obj; AttachmentUpdate.bAttachmentOnlyUpdate = true;
				OnObjectCreated.Broadcast(AttachmentUpdate);
			}
		}
		else
		{
			// Worn gear: never invent a held attachment (that piles a torso mesh at the feet).
			Obj->ParentLocation = 0;
			if ((Location & ACEEquipMask::MissileAmmo) != 0) Obj->ParentGuid = 0;
		}
	}
	if (ItemGuid == PendingEquipmentGuid && bPendingEquipmentWieldSent)
	{
		const uint32 RestoreMode = PendingEquipmentCombatMode;
		CancelEquipmentSwap();
		// A wand dequip legitimately passes through unarmed Melee. Restore the
		// requested stance only after the replacement is authoritatively equipped.
		if (RestoreMode != 0 && RestoreMode != static_cast<uint32>(PlayerVitals.CombatMode))
			SendChangeCombatMode(RestoreMode);
	}
}

void FACESession::HandleInventoryPutObjIn3D(FACEBinaryReader& Reader)
{
	// GameEventItemServerSaysMoveItem: itemGuid (item left our inventory for the world).
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	EquipmentRemovalEvidence.Remove(ItemGuid);
	PendingInventoryTransfers.Remove(ItemGuid);
	FACEPosition SpawnPos;
	bool bHavePos = false;
	int32 PrevContainer = 0;
	int32 PrevPlacement = INDEX_NONE;
	bool bPackSlots = false;
	if (FACEWorldObject* Obj = WorldObjects.Find(ItemGuid))
	{
		PrevContainer = Obj->ContainerId;
		PrevPlacement = Obj->PlacementPosition;
		bPackSlots = IsPackSlotItem(*Obj);
		Obj->ContainerId = 0;
		Obj->WielderId = 0;
		Obj->CurrentWieldedLocation = 0;
		Obj->ParentGuid = 0;
		Obj->ParentLocation = 0;
		Obj->PlacementPosition = INDEX_NONE;
		if (Obj->bHasPosition)
		{
			SpawnPos = Obj->Position;
			bHavePos = true;
		}
	}
	RemoveFromContainerLists(ItemGuid);
	if (PrevContainer != 0)
	{
		ShiftContainerPlacementsAfterRemove(PrevContainer, PrevPlacement, bPackSlots);
	}
	// UpdatePosition may have arrived before this ClearContainer — rebroadcast so the
	// world presenter spawns the freestanding mesh (critical for indoor drops).
	if (bHavePos)
	{
		OnPositionUpdate.Broadcast(ItemGuid, SpawnPos);
	}
}

void FACESession::RemoveObjectFromWorldVisual(int32 ObjectGuid)
{
	if (FACEWorldObject* Obj = WorldObjects.Find(ObjectGuid))
	{
		// Keep the last world Position for subsequent server corrections; clear the world flag.
		Obj->bHasPosition = false;
		Obj->ParentGuid = 0;
		Obj->ParentLocation = 0;
	}
	// Same signal WorldPresenter uses for ObjectDelete — destroys the actor only.
	// WorldObjects entry is intentionally kept for inventory / ContainId.
	OnObjectDeleted.Broadcast(ObjectGuid);
}

void FACESession::HandleInventoryServerSaveFailed(FACEBinaryReader& Reader)
{
	// GameEvent 0x00A0 — Mag-nus ACCWeenieObject::ServerSaysAttemptFailed.
	// Server rejected Put/Wield; keep the last acknowledged membership.
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	// A rejection completes this inventory request too. Leaving it pending
	// needlessly blocks another hand/F attempt until the timeout expires.
	PendingInventoryTransfers.Remove(ItemGuid);
	if (ItemGuid == PendingEquipmentGuid || PendingEquipmentRemovals.Contains(ItemGuid)) CancelEquipmentSwap();

	InventoryFailureItem = ItemGuid;
	InventoryFailureError = Reader.CanRead(4) ? Reader.ReadUInt32() : 0;
	++InventoryFailureSerial;
	Log(FString::Printf(TEXT("InventoryServerSaveFailed guid=0x%08X — retained server inventory"), ItemGuid));
}

void FACESession::HandlePickupEvent(FACEBinaryReader& Reader)
{
	const int32 PayloadStart = Reader.Tell();
	// Physics PickupEvent (0xF74A) removes a held/world visual. A missile
	// launch sends it for the equipped STACK before SetStackSize + reload.
	if (!Reader.CanRead(8)) return;
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	const uint16 IncomingInstance = Reader.ReadUInt16();
	const uint16 IncomingPosition = Reader.ReadUInt16();
	if (DeferObjectPhysicsEvent(ItemGuid, IncomingInstance, ACEOpcode::PickupEvent, Reader, PayloadStart)) return;
	if (FACEWorldObject* Obj = WorldObjects.Find(ItemGuid))
	{
		if (Obj->bHasPhysicsTimestamps
			&& !ACEPhysicsTimeStamp::IsNewer(Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Position], IncomingPosition)) return;
		Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Position] = IncomingPosition;
	}
	// SmartBox::DoPickupEvent only unparents/leaves the physical world. It
	// cannot erase a newer WieldItem received on the independent UI queue.
	// All equipment retains its inventory state until ContainId/wield/remove.
	RemoveObjectFromWorldVisual(ItemGuid);
}
void FACESession::HandleParentEvent(FACEBinaryReader& Reader)
{
	const int32 PayloadStart = Reader.Tell();
	// Physics ParentEvent (0xF749): parentGuid, childGuid, parentLocation, placement, seqs.
	if (!Reader.CanRead(20))
	{
		return;
	}
	const int32 ParentGuid = static_cast<int32>(Reader.ReadUInt32());
	const int32 ChildGuid = static_cast<int32>(Reader.ReadUInt32());
	const int32 ParentLocation = static_cast<int32>(Reader.ReadInt32());
	const int32 Placement = static_cast<int32>(Reader.ReadInt32());
	const uint16 IncomingInstance = Reader.ReadUInt16();
	const uint16 IncomingPosOrParent = Reader.ReadUInt16();
	// ParentEvent carries the parent's incarnation, but the child's position
	// sequence (SmartBox::HandleParentEvent / ACE GameMessageParentEvent).
	// Comparing this instance with the arrow's instance hides loaded ammunition
	// whenever the wielder has logged in more often than the inventory object.
	if (ParentGuid != 0 && DeferObjectPhysicsEvent(ParentGuid, IncomingInstance, ACEOpcode::ParentEvent, Reader, PayloadStart)) return;
	const FACEWorldObject* Child = WorldObjects.Find(ChildGuid);
	const uint16 ChildInstance = Child ? Child->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance] : 0;
	if (DeferObjectPhysicsEvent(ChildGuid, ChildInstance, ACEOpcode::ParentEvent, Reader, PayloadStart)) return;
	if (FACEWorldObject* Obj = WorldObjects.Find(ChildGuid))
	{
		if (Obj->bHasPhysicsTimestamps
			&& !ACEPhysicsTimeStamp::IsNewer(Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Position], IncomingPosOrParent)) return;
		Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Position] = IncomingPosOrParent;
		if (ParentGuid == 0 || ParentLocation == 0)
		{
			// Detach held mesh — do not clear ContainerId (ContainId / PickupEvent own that).
			Obj->ParentGuid = 0;
			Obj->ParentLocation = 0;
			RemoveObjectFromWorldVisual(ChildGuid);
			return;
		}
		Obj->ParentGuid = ParentGuid;
		Obj->ParentLocation = ParentLocation;
		if (Placement != 0)
		{
			Obj->PlacementId = Placement;
		}
		else if (ParentLocation != 0)
		{
			const int32 HeldPlacement = ACEPlacementFromParentLocation(ParentLocation);
			if (HeldPlacement != 0)
			{
				Obj->PlacementId = HeldPlacement;
			}
		}
		// Physics attachment can precede the ordered wield event. Preserve our
		// source membership so that event can compact the correct backpack.
		const bool bInOwnedPack = PlayerGuid && Obj->ContainerId && ResolveContainerRoot(ChildGuid) == PlayerGuid;
		if (!bInOwnedPack)
		{
			Obj->ContainerId = 0;
			RemoveFromContainerLists(ChildGuid);
		}
		Obj->bHasPosition = false; // world origin comes from parent attachment
		// Local wield ownership comes from WieldItem, not a physics attachment.
		FACEWorldObject AttachmentUpdate = *Obj; AttachmentUpdate.bAttachmentOnlyUpdate = true;
		OnObjectCreated.Broadcast(AttachmentUpdate);
	}
}

void FACESession::HandleInventoryRemoveObject(FACEBinaryReader& Reader)
{
	// 0x0024: itemGuid removed from our inventory (given away / consumed / salvaged).
	if (!Reader.CanRead(4))
	{
		return;
	}
	const int32 ItemGuid = static_cast<int32>(Reader.ReadUInt32());
	RemoveInventoryObject(ItemGuid);
}

void FACESession::RemoveInventoryObject(int32 ItemGuid)
{
	EquipmentRemovalEvidence.Remove(ItemGuid);
	PendingInventoryTransfers.Remove(ItemGuid);
	// Whole-item turn-ins have no SetStackSize. Invalidate cached inventories
	// just as partial-stack transfers do, before notifying observers.
	++InventoryDataRevision;
	int32 PrevContainer = 0;
	int32 PrevPlacement = INDEX_NONE;
	bool bPackSlots = false;
	if (FACEWorldObject* Obj = WorldObjects.Find(ItemGuid))
	{
		PrevContainer = Obj->ContainerId;
		PrevPlacement = Obj->PlacementPosition;
		bPackSlots = IsPackSlotItem(*Obj);
		Obj->ContainerId = 0;
	}
	RemoveFromContainerLists(ItemGuid);
	if (PrevContainer != 0)
	{
		ShiftContainerPlacementsAfterRemove(PrevContainer, PrevPlacement, bPackSlots);
	}
	WorldObjects.Remove(ItemGuid);
	PropertyStamps.Remove(ItemGuid);
	ObjectVisibilityDeadlines.Remove(ItemGuid);
	ClosedContainerItemDeadlines.Remove(ItemGuid);
	if (SelectedObject.Guid == ItemGuid) SelectObject(0);
	OnObjectDeleted.Broadcast(ItemGuid);
}

int32 FACESession::GetVendorCurrencyCount() const
{
	if (VendorPurseRevision == InventoryDataRevision && VendorPurseWeenie == VendorCurrencyWeenie) return VendorPurseCount;
	int64 Count = 0;
	for (const auto& Pair : WorldObjects)
	{
		const auto& Item = Pair.Value;
		// A zero alternate-currency WCID means ordinary Pyreals. The count in
		// ApproachVendor is only a snapshot and cannot confirm note redemption.
		const bool Currency = VendorCurrencyWeenie ? uint32(Item.WeenieClassId) == VendorCurrencyWeenie
			: (Item.ItemType & ACEItemType::Money) != 0;
		if (!Currency || Item.WielderId || Item.CurrentWieldedLocation) continue;
		int32 Container = Item.ContainerId;
		TSet<int32> Seen;
		while (Container && Container != PlayerGuid && !Seen.Contains(Container))
		{
			Seen.Add(Container);
			const auto* Parent = WorldObjects.Find(Container);
			Container = Parent ? Parent->ContainerId : 0;
		}
		if (Container == PlayerGuid && PlayerGuid) Count += Item.MaxStackSize > 0 ? FMath::Max(0, Item.StackSize) : 1;
	}
	// ApproachVendor deliberately includes currency already spent. Retail's
	// current purse follows inventory ownership/stack updates, not that snapshot.
	VendorPurseRevision = InventoryDataRevision; VendorPurseWeenie = VendorCurrencyWeenie;
	return VendorPurseCount = int32(FMath::Min<int64>(Count, MAX_int32));
}

void FACESession::HandleSetStackSize(FACEBinaryReader& Reader)
{
	++InventoryDataRevision;
	// GameMessageSetStackSize 0x0197: u8 seq, u32 guid, u32 stackSize, u32 value
	if (!Reader.CanRead(13))
	{
		return;
	}
	Reader.ReadUInt8();
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const int32 Stack = static_cast<int32>(Reader.ReadUInt32());
	const int32 Value = static_cast<int32>(Reader.ReadUInt32());
	if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
	{
		Obj->StackSize = Stack;
		if (Obj->MaxStackSize > 0)
		{
			Obj->StackSize = FMath::Clamp(Obj->StackSize, 0, Obj->MaxStackSize);
		}
		Obj->Value = Value;
		if (SelectedObject.bValid && SelectedObject.Guid == Guid)
		{
			SelectedObject.Name = Obj->Name;
			OnSelectionChanged.Broadcast(SelectedObject);
		}
	}
}

void FACESession::ApplyPlayerKillerStatus(int32 Guid, int32 Status)
{
	// Retail updates the public descriptor from quality 134, without waiting for
	// another ObjectCreate or Identify response. Never predict a /pkl success.
	if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
		Obj->ObjectDescriptionFlags = ACEPlayerKillerStatus::UpdateDescriptionFlags(Obj->ObjectDescriptionFlags, Status);
	if (Guid == PlayerGuid)
	{
		PlayerVitals.PlayerKillerStatus = Status;
		PlayerVitals.StatQualityInts.Add(134, Status);
		NotifyVitalsChanged();
	}
	OnPkStatusUpdated.Broadcast(Guid, Status);
	if (SelectedObject.bValid && SelectedObject.Guid == Guid)
		OnSelectionChanged.Broadcast(SelectedObject);
}

void FACESession::HandleUpdatePropertyBool(FACEBinaryReader& Reader, bool bPublic)
{
	// Qualities UIQueue: u8 sequence, optional GUID, u32 property, u32 BOOL.
	if (!Reader.CanRead(bPublic ? 13 : 9)) return;
	Reader.ReadUInt8();
	const int32 Guid = bPublic ? int32(Reader.ReadUInt32()) : PlayerGuid;
	const uint32 Property = Reader.ReadUInt32();
	const bool Value = Reader.ReadUInt32() != 0;
	if (Guid == PlayerGuid)
	{
		PlayerVitals.QualityBools.Add(static_cast<int32>(Property), Value);
		if (Property==110) bAfkMode=Value;
		NotifyVitalsChanged();
	}
	FACEWorldObject* Object = WorldObjects.Find(Guid);
	if (!Object) return;
	// ACCWeenieObject::OnStatUpdated(bool): flags are live qualities, independent
	// of PhysicsState. UIHidden must not hide the mesh or disable wall collision.
	int32 Mask = 0;
	bool Enabled = Value;
	switch (Property)
	{
	case 1: Mask = ACEObjectDescFlag::Stuck; break;
	case 3: Mask = ACEObjectDescFlag::Openable; Enabled = !Value; break;
	case 19: Mask = ACEObjectDescFlag::Attackable; break;
	case 22: Mask = 2; break; // Inscribable
	case 24: Mask = ACEObjectDescFlag::UiHidden; break;
	case 25: Mask = ACEObjectDescFlag::ImmuneCellRestrictions; break;
	case 26: Mask = ACEObjectDescFlag::HiddenAdmin; break;
	case 130: Mask = ACEObjectDescFlag::WieldLeft; break; // AutowieldLeft / Left-hand Tether
	default: return;
	}
	const int32 Flags = Enabled ? Object->ObjectDescriptionFlags | Mask : Object->ObjectDescriptionFlags & ~Mask;
	if (Flags == Object->ObjectDescriptionFlags) return;
	Object->ObjectDescriptionFlags = Flags;
	// An explicit server change from enemy to noncombat NPC is a role change,
	// unlike a late Ready motion from the defeated incarnation.
	if (Property == 19 && !Value && Object->bDying && !Object->IsCorpse() && (Object->ItemUseable & 0x20) != 0)
	{
		Object->bDying = false;
		Object->InitialMotionCommand = ACEMotion::Ready;
		Object->InitialMotionStyle = ACEMotion::StanceNonCombat;
	}
	if (Object->ContainerId || Object->WielderId == PlayerGuid) ++InventoryDataRevision;
	FACEWorldObject Update = *Object; Update.bAppearanceOnlyUpdate = true;
	OnObjectCreated.Broadcast(Update);
	if (SelectedObject.Guid == Guid)
	{
		if (Object->IsUiHidden()) SelectObject(0);
		else OnSelectionChanged.Broadcast(SelectedObject);
	}
}

void FACESession::HandlePublicUpdatePropertyInt(FACEBinaryReader& Reader)
{
	// 0x02CE: u8 sequence, u32 objectGuid, u32 propertyId, i32 value.
	if (!Reader.CanRead(13))
	{
		return;
	}
	Reader.ReadUInt8();
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const uint32 Prop = Reader.ReadUInt32();
	const int32 Value = Reader.ReadInt32();
	if (Guid == PlayerGuid)
	{
		ApplyPlayerIntQuality(Prop, Value);
	}
	if (Prop == 134)
	{
		if (Guid != PlayerGuid) ApplyPlayerKillerStatus(Guid, Value);
		return;
	}
	if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
	{
		if(Obj->ContainerId || Obj->WielderId==PlayerGuid || (Obj->CurrentWieldedLocation && Obj->ParentGuid==PlayerGuid))++InventoryDataRevision;
		if (Prop == 4) // PropertyInt.ClothingPriority
		{
			Obj->ClothingPriority = Value;
		}
		else if (Prop == 151) Obj->HookType = static_cast<uint16>(Value);
		else if (Prop == 152) Obj->HookItemTypes = static_cast<uint32>(Value);
		else if (Prop == 10) // PropertyInt.CurrentWieldedLocation
		{
			if (UpdateEquipmentRemovalEvidence(Guid, WieldLocationCleared, Value == 0)) return;
			// ACCWeenieObject::OnStatUpdated leaves owned/requested equipment to
			// the ordered inventory events, including ACE's preliminary zeroes.
			if (!HasPlayerInventoryState(*Obj)) Obj->CurrentWieldedLocation = static_cast<int64>(Value);
		}
		else if (Prop == 33) // PropertyInt.Bonded
		{
			Obj->Bonded = Value;
		}
		else if (Prop == 16) // PropertyInt.ItemUseable: quest/switch state can change it live.
		{
			Obj->ItemUseable = Value;
		}
		else if (Prop == 280) // PropertyInt.SharedCooldown
		{
			Obj->CooldownId = Value;
		}
		else if (Prop == 114) // PropertyInt.Attuned
		{
			Obj->Attuned = Value;
		}
		else if (Prop == 53) // PropertyInt.PlacementPosition
		{
			Obj->PlacementPosition = Value;
		}
		else if (Prop == 18) // PropertyInt.UiEffects
		{
			Obj->UiEffects = Value;
		}
		else if (Prop == 11) // PropertyInt.MaxStackSize
		{
			Obj->MaxStackSize = Value;
		}
		else if (Prop == 12) // PropertyInt.StackSize
		{
			Obj->StackSize = Value;
			if (Obj->MaxStackSize > 0)
			{
				Obj->StackSize = FMath::Clamp(Obj->StackSize, 0, Obj->MaxStackSize);
			}
		}
		else if (Prop == 91) // PropertyInt.MaxStructure
		{
			Obj->MaxStructure = Value;
		}
		else if (Prop == 92) // PropertyInt.Structure
		{
			Obj->Structure = Value;
		}
		else if (Prop == 107) Obj->ItemCurrentMana = FMath::Max(0, Value);
		else if (Prop == 108) Obj->ItemMaximumMana = FMath::Max(0, Value);
		else if (Prop == 95) // PropertyInt.RadarBlipColor (e.g. advocate items).
		{
			Obj->RadarBlipColor = static_cast<uint8>(Value);
		}
		else if (Prop == 131) // PropertyInt.MaterialType
		{
			Obj->MaterialType = Value;
		}
		else if (Prop == 46) // PropertyInt.DefaultCombatStyle
		{
			Obj->DefaultCombatStyle = Value;
		}
		else if (Prop == 51) // PropertyInt.CombatUse
		{
			Obj->CombatUse = Value;
		}
		else if (Prop == 5) // PropertyInt.EncumbranceVal
		{
			Obj->Burden = Value;
			if (Guid == PlayerGuid)
			{
				bHasPlayerEncumbrance = true;
				PlayerEncumbranceVal = Value;
			}
		}
	}
}

void FACESession::HandlePublicUpdatePropertyDataID(FACEBinaryReader& Reader)
{
	// 0x02D8: u8 sequence, u32 objectGuid, u32 propertyId, u32 did.
	if (!Reader.CanRead(13))
	{
		return;
	}
	Reader.ReadUInt8();
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const uint32 Prop = Reader.ReadUInt32();
	const uint32 Value = Reader.ReadUInt32();
	ApplyPropertyDataID(Guid, Prop, Value);
}

void FACESession::HandlePrivateUpdatePropertyDataID(FACEBinaryReader& Reader)
{
	// 0x02D7: u8 sequence, u32 propertyId, u32 did (player-only).
	if (!Reader.CanRead(9))
	{
		return;
	}
	Reader.ReadUInt8();
	const uint32 Prop = Reader.ReadUInt32();
	const uint32 Value = Reader.ReadUInt32();
	if (PlayerGuid != 0)
	{
		ApplyPropertyDataID(PlayerGuid, Prop, Value);
	}
}

bool FACESession::HasPlayerInventoryState(const FACEWorldObject& Object) const
{
	return PlayerGuid && (Object.WielderId == PlayerGuid
		|| ResolveContainerRoot(Object.Guid) == PlayerGuid
		|| PendingInventoryTransfers.Contains(Object.Guid)
		|| PendingEquipmentGuid == Object.Guid || PendingEquipmentRemovals.Contains(Object.Guid));
}

bool FACESession::UpdateEquipmentRemovalEvidence(int32 Guid, uint8 Evidence, bool bSet)
{
	const auto* Object = WorldObjects.Find(Guid);
	if (!Object || !PlayerGuid || Object->WielderId != PlayerGuid || Object->ContainerId)
	{
		EquipmentRemovalEvidence.Remove(Guid);
		return false;
	}
	uint8 Flags = EquipmentRemovalEvidence.FindRef(Guid);
	Flags = bSet ? Flags | Evidence : Flags & ~Evidence;
	if (Flags == (WielderCleared | WieldLocationCleared | WorldDeleted))
	{
		// Player.UpdateAmmoAfterLaunch -> ConsumeItem has no ContainID or
		// InventoryRemoveObject receipt. The two quality clears and delete are
		// its authoritative removal; Pickup alone just detaches each projectile.
		RemoveInventoryObject(Guid);
		return true;
	}
	if (Flags) EquipmentRemovalEvidence.Add(Guid, Flags);
	else EquipmentRemovalEvidence.Remove(Guid);
	return false;
}

void FACESession::HandlePublicUpdateInstanceId(FACEBinaryReader& Reader)
{
	// 0x02DA: u8 sequence, u32 objectGuid, u32 propertyId, u32 instanceGuid.
	if (!Reader.CanRead(13))
	{
		return;
	}
	Reader.ReadUInt8();
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const uint32 Prop = Reader.ReadUInt32();
	const int32 Value = static_cast<int32>(Reader.ReadUInt32());
	ApplyPropertyInstanceId(Guid, Prop, Value);
}

void FACESession::HandlePrivateUpdateInstanceId(FACEBinaryReader& Reader)
{
	// 0x02D9: the same IID quality, implicitly owned by the player.
	if (!Reader.CanRead(9)) return;
	Reader.ReadUInt8();
	const uint32 Prop = Reader.ReadUInt32();
	const int32 Value = Reader.ReadInt32();
	if (PlayerGuid) ApplyPropertyInstanceId(PlayerGuid, Prop, Value);
}

void FACESession::ApplyPropertyInstanceId(int32 Guid, uint32 Prop, int32 Value)
{
	if (Guid == PlayerGuid && Prop == 11)
	{
		// ACE sends this only when the attacker changes, including on evade or
		// resist. Repeated incoming hits maintain the separate retail timer.
		LastAttackerGuid = Value;
	}
	if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
	{
		if (Prop == 2) // PropertyInstanceId.Container
		{
			// The ordered inventory event owns removal and insertion, including
			// Container=0 before a gift/drop. Clearing ownership here loses the
			// source placement, leaving gaps and duplicate indices on later drags.
			if (Value != 0 || (PlayerGuid && ResolveContainerRoot(Guid) == PlayerGuid)) return;
			Obj->ContainerId = Value;
		}
		else if (Prop == 3) // PropertyInstanceId.Wielder
		{
			if (UpdateEquipmentRemovalEvidence(Guid, WielderCleared, Value == 0)) return;
			if (HasPlayerInventoryState(*Obj)) return;
			Obj->WielderId = Value;
			if (Value != 0 && !(PlayerGuid && Obj->ContainerId && ResolveContainerRoot(Guid) == PlayerGuid))
			{
				Obj->ContainerId = 0;
			}
		}
		else if (Prop == 26) // PropertyInstanceId.Monarch
		{
			Obj->MonarchGuid = Value;
		}
		else if (Prop == 44) // PropertyInstanceId.PetOwner
		{
			Obj->PetOwnerId = Value;
		}
		else if (Prop == 32) // PropertyInstanceId.HouseOwner
		{
			Obj->HouseOwnerId = Value;
		}
	}
}

void FACESession::GetEquippedItems(TArray<FACEWorldObject>& Out) const
{
	Out.Reset();
	for (const TPair<int32, FACEWorldObject>& Pair : WorldObjects)
	{
		const FACEWorldObject& Obj = Pair.Value;
		if (Obj.CurrentWieldedLocation == 0)
		{
			continue;
		}
		if (Obj.WielderId == PlayerGuid || Obj.ParentGuid == PlayerGuid)
		{
			Out.Add(Obj);
		}
	}
}

const FACEWorldObject* FACESession::FindEquippedItem(int64 LocationMask, int32 ItemTypeMask, int32 AmmoTypeMask) const
{
	if (PlayerGuid == 0 || LocationMask == 0) return nullptr;
	for (const auto& Pair : WorldObjects)
	{
		const auto& Item = Pair.Value;
		if ((Item.CurrentWieldedLocation & LocationMask) != 0
			&& (Item.WielderId == PlayerGuid || Item.ParentGuid == PlayerGuid)
			&& (ItemTypeMask == 0 || (Item.ItemType & ItemTypeMask) != 0)
			&& (AmmoTypeMask == 0 || Item.AmmoType == 0 || (Item.AmmoType & AmmoTypeMask) != 0))
			return &Item;
	}
	return nullptr;
}

void FACESession::GetPackItemCounts(TMap<int32, int32>& InOutCounts) const
{
	auto IsPack = [](const FACEWorldObject& O)
	{
		return O.ItemsCapacity > 0 || O.ContainersCapacity > 0
			|| (O.ItemType & ACEItemType::Container) != 0
			|| (O.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot) != 0;
	};
	TSet<int32> MainSeen;
	TSet<int32> ScanContainers;
	for (auto& Count : InOutCounts)
	{
		Count.Value = 0;
		const bool bMain = Count.Key == PlayerGuid;
		const auto* Contents = ContainerContents.Find(Count.Key);
		if (bMain || !Contents) ScanContainers.Add(Count.Key);
		if (!Contents) continue;
		for (const auto& Ref : *Contents)
		{
			if (Ref.ContainerType != 0) continue;
			const auto* O = WorldObjects.Find(Ref.ItemGuid);
			if (O)
			{
				if (bMain) MainSeen.Add(Ref.ItemGuid);
				if (!IsPack(*O)) ++Count.Value;
			}
			// Side packs reserve a slot before ObjectCreate; main pack waits for it.
			else if (!bMain) ++Count.Value;
		}
	}
	if (ScanContainers.IsEmpty()) return;
	// One world scan for every unopened pack, instead of one scan per pack.
	for (const auto& Pair : WorldObjects)
	{
		const auto& O = Pair.Value;
		if (!ScanContainers.Contains(O.ContainerId)) continue;
		if (O.ContainerId == PlayerGuid && (MainSeen.Contains(O.Guid) || IsPack(O))) continue;
		++InOutCounts.FindChecked(O.ContainerId);
	}
}

namespace
{
    void SortInventoryGuidsByPlacement(TArray<int32>& Guids, const TMap<int32, FACEWorldObject>& Objects)
    {
        Guids.Sort([&Objects](int32 GuidA, int32 GuidB)
        {
            const auto* A = Objects.Find(GuidA);
            const auto* B = Objects.Find(GuidB);
            const int32 Pa = A && A->PlacementPosition >= 0 ? A->PlacementPosition : MAX_int32;
            const int32 Pb = B && B->PlacementPosition >= 0 ? B->PlacementPosition : MAX_int32;
            return Pa != Pb ? Pa < Pb : GuidA < GuidB;
        });
    }
}

void FACESession::GetPackItems(int32 ContainerGuid, TArray<FACEWorldObject>& Out) const
{
    TArray<int32> Guids;
    GetPackItemGuids(ContainerGuid, Guids);
    Out.Reset(Guids.Num());
    for (int32 Guid : Guids)
    {
        if (const auto* Item = WorldObjects.Find(Guid)) Out.Add(*Item);
        else
        {
            auto& Stub = Out.AddDefaulted_GetRef();
            Stub.Guid = Guid;
            Stub.ContainerId = ContainerGuid;
            Stub.Name = FString::Printf(TEXT("Item 0x%08X"), Guid);
        }
    }
}

void FACESession::GetPlayerPacks(TArray<FACEWorldObject>& Out) const
{
    TArray<int32> Guids;
    GetPlayerPackGuids(Guids);
    Out.Reset(Guids.Num());
    for (int32 Guid : Guids) Out.Add(WorldObjects.FindChecked(Guid));
}

void FACESession::GetPackItemGuids(int32 ContainerGuid, TArray<int32>& Out) const
{
	Out.Reset();
	TSet<int32> Seen;

	auto IsPackObject = [](const FACEWorldObject& Obj) -> bool
	{
		return Obj.ItemsCapacity > 0 || Obj.ContainersCapacity > 0
			|| (Obj.ItemType & ACEItemType::Container) != 0
			|| (Obj.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot) != 0;
	};

	// Player main pack: PlayerDescription footer order is retail grid order (not TMap scan).
	if (ContainerGuid == PlayerGuid)
	{
		if (const TArray<FACEContainerItemRef>* Contents = ContainerContents.Find(PlayerGuid))
		{
			for (const FACEContainerItemRef& Ref : *Contents)
			{
				if (Ref.ContainerType != 0)
				{
					continue;
				}
				const FACEWorldObject* Obj = WorldObjects.Find(Ref.ItemGuid);
				if (!Obj)
				{
					continue;
				}
				if (IsPackObject(*Obj))
				{
					Seen.Add(Ref.ItemGuid);
					continue;
				}
				Out.Add(Obj->Guid);
				Seen.Add(Ref.ItemGuid);
			}
		}
		// Append late creates directly, then sort the combined list only once.
		for (const TPair<int32, FACEWorldObject>& Pair : WorldObjects)
		{
			const FACEWorldObject& Obj = Pair.Value;
			if (Seen.Contains(Obj.Guid))
			{
				continue;
			}
			if (Obj.ContainerId == PlayerGuid && !IsPackObject(Obj))
			{
				Out.Add(Obj.Guid);
			}
		}
		SortInventoryGuidsByPlacement(Out, WorldObjects);
		return;
	}

	// ViewContents is authoritative for corpses / side packs.
	if (const TArray<FACEContainerItemRef>* Contents = ContainerContents.Find(ContainerGuid))
	{
		for (const FACEContainerItemRef& Ref : *Contents)
		{
			if (const FACEWorldObject* Obj = WorldObjects.Find(Ref.ItemGuid))
			{
				const bool bIsPack = IsPackObject(*Obj) || Ref.ContainerType != 0;
				if (bIsPack)
				{
					Seen.Add(Ref.ItemGuid);
					continue;
				}
				Out.Add(Obj->Guid);
			}
			else if (Ref.ContainerType == 0)
			{
				Out.Add(Ref.ItemGuid);
			}
			Seen.Add(Ref.ItemGuid);
		}
		// ACCObjectMaint::ViewObjectContents preserves wire order in IDList.
		// Missing descriptions still reserve their exact position; sorting by
		// cached qualities would move those IDs as CreateObject packets arrive.
		return;
	}

	// ObjectCreate ContainerId scan (main pack, or pack before ViewContents).
	for (const TPair<int32, FACEWorldObject>& Pair : WorldObjects)
	{
		const FACEWorldObject& Obj = Pair.Value;
		if (Seen.Contains(Obj.Guid))
		{
			continue;
		}
		// Physical attachment may arrive before the ordered wield receipt.
		// Until then, match the authoritative list path above and retain this ID.
		if (Obj.ContainerId == ContainerGuid)
		{
			const bool bIsPack = Obj.ItemsCapacity > 0 || Obj.ContainersCapacity > 0
				|| (Obj.ItemType & ACEItemType::Container) != 0
				|| (Obj.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot) != 0;
			if (bIsPack && ContainerGuid == PlayerGuid)
			{
				continue;
			}
			Out.Add(Obj.Guid);
		}
	}

	// Stable-sort ObjectCreate fallback (TMap iteration order is unstable).
	SortInventoryGuidsByPlacement(Out, WorldObjects);
}

void FACESession::GetPlayerPackGuids(TArray<int32>& Out) const
{
	GetContainerPackGuids(PlayerGuid, Out);
}

void FACESession::GetContainerPackGuids(int32 ContainerGuid, TArray<int32>& Out) const
{
	Out.Reset();
	TSet<int32> Seen;
	// Retail side slots = UseBackpackSlot items (Container + Foci) ordered by PlacementPosition.
	// GameEventViewContents tags them with ContainerType 1/2. The inventory handlers
	// reconcile PlacementPosition before these IDs are sorted for presentation.
	constexpr int32 ContainerType_Container = 1;
	constexpr int32 ContainerType_Foci = 2;
	if (const TArray<FACEContainerItemRef>* Contents = ContainerContents.Find(ContainerGuid))
	{
		for (const FACEContainerItemRef& Ref : *Contents)
		{
			if (Ref.ContainerType != ContainerType_Container && Ref.ContainerType != ContainerType_Foci)
			{
				continue;
			}
			const auto* Obj = WorldObjects.Find(Ref.ItemGuid);
			if (!Obj)
			{
				continue;
			}
			if (Obj->CurrentWieldedLocation != 0)
			{
				continue;
			}
			Out.Add(Obj->Guid);
			Seen.Add(Obj->Guid);
		}
	}
	for (const TPair<int32, FACEWorldObject>& Pair : WorldObjects)
	{
		const FACEWorldObject& Obj = Pair.Value;
		if (Seen.Contains(Obj.Guid))
		{
			continue;
		}
		const bool bInContainer = Obj.ContainerId == ContainerGuid || Obj.ParentGuid == ContainerGuid;
		const bool bIsPack = Obj.ItemsCapacity > 0 || Obj.ContainersCapacity > 0
			|| (Obj.ItemType & ACEItemType::Container) != 0
			|| (Obj.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot) != 0;
		if (bInContainer && bIsPack && Obj.CurrentWieldedLocation == 0 && Obj.Guid != ContainerGuid)
		{
			Out.Add(Obj.Guid);
		}
	}
	SortInventoryGuidsByPlacement(Out, WorldObjects);
}

void FACESession::ApplyServerTime(double ServerTicks, double ReceivedAt, const TCHAR* Source)
{
	// Zero is a valid ACE epoch (Morningthaw 1, PY 10), not an unknown clock.
	if (!FMath::IsFinite(ServerTicks) || ServerTicks < 0.0) return;
	const double Now = FPlatformTime::Seconds();
	const double SampleNow = ServerTicks + FMath::Max(0.0, Now - ReceivedAt);
	const double Correction = bHasServerTime ? SampleNow - GetPortalYearTicks() : 0.0;
	if (!bHasServerTime || FMath::Abs(Correction) >= 1.0)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEClock: %s ticks=%.3f correction=%+.3fs queued=%.3fs"),
			Source, ServerTicks, Correction, Now - ReceivedAt);
	}
	else
	{
		UE_LOG(LogTemp, Verbose, TEXT("ACEClock: %s ticks=%.3f correction=%+.3fs"), Source, ServerTicks, Correction);
	}
	bHasServerTime = true;
	PortalYearTicksAtConnect = ServerTicks;
	// Preserve arrival time when authenticated packets wait for a missing sequence.
	PortalYearTicksRealtime = ReceivedAt;
}

double FACESession::GetPortalYearTicks() const
{
	if (!bHasServerTime)
	{
		return 0.0;
	}
	return PortalYearTicksAtConnect + (FPlatformTime::Seconds() - PortalYearTicksRealtime);
}

bool FACESession::GetWorldObject(int32 Guid, FACEWorldObject& Out) const
{
	if (const FACEWorldObject* Found = WorldObjects.Find(Guid))
	{
		Out = *Found;
		return true;
	}
	return false;
}

void FACESession::HandleUpdatePosition(FACEBinaryReader& Reader)
{
	const int32 PayloadStart = Reader.Tell();
	if (!Reader.CanRead(8)) return;
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const uint32 PosFlags = Reader.ReadUInt32();
	// PositionPack always includes timestamps. Never treat a truncated packet
	// as an unsequenced correction (or synthesize coordinates from failed reads).
	int32 BodyBytes = 16 + 8;
	for (uint32 Omitted : {0x08u, 0x10u, 0x20u, 0x40u}) if (!(PosFlags & Omitted)) BodyBytes += 4;
	if (PosFlags & 1u) BodyBytes += 12;
	if (PosFlags & 2u) BodyBytes += 4;
	if (!Reader.CanRead(BodyBytes)) return;
	FACEPosition Pos;
	Pos.CellId = static_cast<int32>(Reader.ReadUInt32());
	Pos.Location.X = Reader.ReadFloat();
	Pos.Location.Y = Reader.ReadFloat();
	Pos.Location.Z = Reader.ReadFloat();

	constexpr uint32 OrientationHasNoW = 0x08;
	constexpr uint32 OrientationHasNoX = 0x10;
	constexpr uint32 OrientationHasNoY = 0x20;
	constexpr uint32 OrientationHasNoZ = 0x40;
	constexpr uint32 HasVelocity = 0x01;
	constexpr uint32 HasPlacementID = 0x02;
	constexpr uint32 IsGrounded = 0x04;

	Pos.RotationW = (PosFlags & OrientationHasNoW) ? 0.f : Reader.ReadFloat();
	Pos.RotationXYZ.X = (PosFlags & OrientationHasNoX) ? 0.f : Reader.ReadFloat();
	Pos.RotationXYZ.Y = (PosFlags & OrientationHasNoY) ? 0.f : Reader.ReadFloat();
	Pos.RotationXYZ.Z = (PosFlags & OrientationHasNoZ) ? 0.f : Reader.ReadFloat();
	Pos.ReconstructOmittedRotation(PosFlags);
	Pos.bIsGrounded = (PosFlags & IsGrounded) != 0;
	Pos.bHasContactState = true;

	if (PosFlags & HasVelocity)
	{
		Pos.Velocity.X = Reader.ReadFloat();
		Pos.Velocity.Y = Reader.ReadFloat();
		Pos.Velocity.Z = Reader.ReadFloat();
		Pos.bHasVelocity = true;
	}
	// PositionPack::UnPack defaults an omitted placement to zero. Do not keep
	// an inventory/held pose when the server supplies the dropped item's frame.
	const int32 PlacementId = (PosFlags & HasPlacementID) ? static_cast<int32>(Reader.ReadUInt32()) : 0;
	if (Pos.Location.ContainsNaN() || Pos.RotationXYZ.ContainsNaN()
		|| !FMath::IsFinite(Pos.RotationW) || Pos.Velocity.ContainsNaN()) return;
	if (Reader.CanRead(8))
	{
		const uint16 IncomingInstance = Reader.ReadUInt16();
		const uint16 IncomingPosition = Reader.ReadUInt16();
		const uint16 IncomingTeleport = Reader.ReadUInt16();
		const uint16 IncomingForce = Reader.ReadUInt16();
		if (DeferObjectPhysicsEvent(Guid, IncomingInstance, ACEOpcode::UpdatePosition, Reader, PayloadStart)) return;
		Pos.bHasTeleportSequence = true;
		Pos.TeleportSequence = IncomingTeleport;

		if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
		{
			if (Obj->bHasPhysicsTimestamps)
			{
				// SmartBox::UnpackPositionEvent requires the matching incarnation.
				// A newer position number from a previous visit cannot move this object.
				if (IncomingInstance != Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance]
					|| ACEPhysicsTimeStamp::IsNewer(IncomingTeleport, Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Teleport]))
					return;
				const bool bNewPosition = ACEPhysicsTimeStamp::IsNewer(
					Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Position], IncomingPosition);
				const bool bForceSelf = Guid == PlayerGuid && ACEPhysicsTimeStamp::IsNewer(
					Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::ForcePosition], IncomingForce);
				if (!bNewPosition && !bForceSelf) return;
			}
			// POSITION_TS can advance while an older FORCE_POSITION_TS is still
			// in flight. Retail newer_event never rolls the force epoch backwards.
			if (!Obj->bHasPhysicsTimestamps || ACEPhysicsTimeStamp::IsNewer(
				Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::ForcePosition], IncomingForce))
				Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::ForcePosition] = IncomingForce;
			Obj->bHasPhysicsTimestamps = true;
			Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance] = IncomingInstance;
			Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Position] = IncomingPosition;
			Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Teleport] = IncomingTeleport;
			Obj->Position = Pos;
			Obj->bHasPosition = true;
			Obj->PlacementId = PlacementId;
			// SmartBox::HandleReceivedPosition unsets the physics parent before
			// applying placement. Inventory ownership is still resolved by UI events.
			Obj->ParentGuid = 0;
			Obj->ParentLocation = 0;
		}

		if (Guid == PlayerGuid)
		{
			InstanceSeq = IncomingInstance;
			if (TeleportSeq != IncomingTeleport) SelectObject(0);
			TeleportSeq = IncomingTeleport;
			if (const auto* Obj = WorldObjects.Find(Guid))
				ForcePositionSeq = Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::ForcePosition];
			else if (ACEPhysicsTimeStamp::IsNewer(ForcePositionSeq, IncomingForce))
				ForcePositionSeq = IncomingForce;
		}
	}
	else if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
	{
		Obj->Position = Pos;
		Obj->bHasPosition = true;
	}

	if (Guid == PlayerGuid)
	{
		PlayerPosition = Pos;
		if (const auto* Selected = WorldObjects.Find(SelectedObject.Guid);
			Selected && Selected->bHasPosition && !IsNearbyHealthObject(SelectedObject.Guid)) SelectObject(0);
		MaybeEnterWorldComplete();
	}

	OnPositionUpdate.Broadcast(Guid, Pos);
}

void FACESession::HandleUpdateMotion(FACEBinaryReader& Reader)
{
	const int32 PayloadStart = Reader.Tell();
	// GameMessageUpdateMotion: guid, instance sequence, MovementData header/body.
	if (!Reader.CanRead(16))
	{
		return;
	}

	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const uint16 IncomingInstance = Reader.ReadUInt16();
	const uint16 IncomingMovement = Reader.ReadUInt16();
	const uint16 IncomingServerControl = Reader.ReadUInt16();
	const bool bAutonomous = Reader.ReadUInt8() != 0;
	Reader.Align();

	if (FACEWorldObject* Obj = WorldObjects.Find(Guid); Obj && !Obj->bPhysicsDescriptionPending)
	{
		if (Obj->bHasPhysicsTimestamps && IncomingInstance == Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance])
		{
			// CPhysics::SetObjectMovement requires a newer movement number AND
			// a server-control epoch that has not gone backwards.
			if (!ACEPhysicsTimeStamp::IsNewer(Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Movement], IncomingMovement)
				|| ACEPhysicsTimeStamp::IsNewer(IncomingServerControl, Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::ServerControl]))
			{
				return;
			}
		}
	}

	if (!Reader.CanRead(4))
	{
		return;
	}

	const uint8 MovementType = Reader.ReadUInt8();
	const uint8 MotionFlags = Reader.ReadUInt8();
	const uint16 HeaderStyleRaw = Reader.ReadUInt16(); // MovementData.CurrentStyle (MotionStance)

	FACEObjectMotionState Motion;
	Motion.MovementType = MovementType;
	uint16 ActionStamp=0;
	bool bHaveActionStamp=false;
	if(const auto* Obj=WorldObjects.Find(Guid);Obj && !Obj->bPhysicsDescriptionPending
		&& (!Obj->bHasPhysicsTimestamps || Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance]==IncomingInstance))
	{ActionStamp=Obj->ReceivedActionStamp;bHaveActionStamp=Obj->bHasReceivedActionStamp;}
	if (HeaderStyleRaw != 0)
	{
		Motion.CurrentStyle = static_cast<int32>(ACEMotion::ExpandPackedCommand(HeaderStyleRaw));
	}

	if (MovementType == 0) // MovementType.Invalid/general interpreted state
	{
		if (!Reader.CanRead(4))
		{
			return;
		}

		const uint32 Packed = Reader.ReadUInt32();
		const uint32 Flags = Packed & 0x7Fu;
		const uint32 CommandCount = Packed >> 7;
		uint16 ForwardCommand = 0x0003; // Ready
		uint16 SideStepCommand = 0;
		float ForwardSpeed = 1.f;
		float SideStepSpeed = 1.f;
		float TurnSpeed = 1.f;

		auto ReadU16If = [&](uint32 Flag, uint16& Out) -> bool
		{
			if ((Flags & Flag) == 0)
			{
				return true;
			}
			if (!Reader.CanRead(2))
			{
				return false;
			}
			Out = Reader.ReadUInt16();
			return true;
		};
		auto ReadFloatIf = [&](uint32 Flag, float& Out) -> bool
		{
			if ((Flags & Flag) == 0)
			{
				return true;
			}
			if (!Reader.CanRead(4))
			{
				return false;
			}
			Out = Reader.ReadFloat();
			return FMath::IsFinite(Out);
		};

		uint16 StyleRaw = 0;
		uint16 TurnCommand = 0;
		if (!ReadU16If(0x01u, StyleRaw)
			|| !ReadU16If(0x02u, ForwardCommand)
			|| !ReadU16If(0x08u, SideStepCommand)
			|| !ReadU16If(0x20u, TurnCommand)
			|| !ReadFloatIf(0x04u, ForwardSpeed)
			|| !ReadFloatIf(0x10u, SideStepSpeed)
			|| !ReadFloatIf(0x40u, TurnSpeed))
		{
			return;
		}

		if (StyleRaw != 0)
		{
			Motion.CurrentStyle = static_cast<int32>(ACEMotion::ExpandPackedCommand(StyleRaw));
		}

		// MotionItem = command u16 + sequence u16 + speed f32. Melee attacks / FastTick windups live here.
		const int64 CommandBytes = static_cast<int64>(CommandCount) * 8;
		if (CommandBytes > MAX_int32 || !Reader.CanRead(static_cast<int32>(CommandBytes)))
		{
			return;
		}
		auto IsLocoOrDoorOrReady = [](uint16 Low) -> bool
		{
			return Low == 0x0003 || Low == 0x0005 || Low == 0x0006 || Low == 0x0007
				|| Low == 0x000D || Low == 0x000E || Low == 0x000F || Low == 0x0010
				|| Low == 0x000B || Low == 0x000C;
		};
		Motion.ActionFollowups.Reset();
		for (uint32 i = 0; i < CommandCount; ++i)
		{
			const uint16 RawCmd = Reader.ReadUInt16();
			const uint16 PackedActionStamp=Reader.ReadUInt16(); // 15-bit stamp + autonomous bit
			const float Speed = Reader.ReadFloat();
			if (!FMath::IsFinite(Speed)) return;
			// CMotionInterp::HandleMovement processes each action once, independent
			// of the movement packet sequence, and retains that action's own speed.
			const uint16 IncomingAction=PackedActionStamp & 0x7FFFu;
			const uint16 ActionDelta=(IncomingAction-ActionStamp) & 0x7FFFu;
			if ((Guid==PlayerGuid && (PackedActionStamp & 0x8000u))
				|| (bHaveActionStamp && (ActionDelta==0 || ActionDelta>=0x4000u))) continue;
			ActionStamp=IncomingAction;bHaveActionStamp=true;
			const uint32 Full = ACEMotion::ExpandPackedCommand(RawCmd);
			// FastTick magic packs every scarab windup into one CommandList — keep order
			// (first → ActionCommand, rest → ActionFollowups). Do not keep only the last.
			const uint16 Low = static_cast<uint16>(Full);
			const bool bStance = (Full & 0xFF000000u) == 0x80000000u;
			if (!IsLocoOrDoorOrReady(Low) && !bStance)
			{
				// A zero rate pauses the authored motion; do not invent a minimum
				// playback speed for a server-controlled action.
				const float ClampedSpeed = FMath::Max(0.f, Speed);
				if (Motion.ActionCommand == 0)
				{
					Motion.ActionCommand = static_cast<int32>(Full);
					Motion.bNewAction = true;
					Motion.ActionSpeed = ClampedSpeed;
				}
				else
				{
					Motion.ActionFollowups.Add(static_cast<int32>(Full));
					Motion.ActionFollowupSpeeds.Add(ClampedSpeed);
				}
			}
		}
		Reader.Align();
		if ((MotionFlags & 0x01u) != 0)
		{
			if (!Reader.CanRead(4)) return;
			Motion.StickyTargetGuid = static_cast<int32>(Reader.ReadUInt32());
		}

		Motion.ForwardCommand = static_cast<int32>(ForwardCommand);
		switch (ForwardCommand)
		{
		case 0x0005: // WalkForward; observers use negative speed for backpedal
			Motion.Forward = FMath::Clamp(ForwardSpeed, -1.f, 1.f);
			Motion.ForwardUnitsPerSecond = 3.1199999f * ForwardSpeed;
			Motion.AnimPlayRate = FMath::Max(0.05f, FMath::Abs(ForwardSpeed));
			// This is already an interpreted command. Speed is the animation
			// multiplier, not permission to reinterpret WalkForward as RunForward.
			Motion.bRunning = false;
			break;
		case 0x0006: // WalkBackwards
			Motion.Forward = -FMath::Clamp(FMath::Abs(ForwardSpeed), 0.f, 1.f);
			Motion.ForwardUnitsPerSecond = -3.1199999f * FMath::Abs(ForwardSpeed) * 0.65f;
			Motion.AnimPlayRate = FMath::Max(0.05f, FMath::Abs(ForwardSpeed) * 0.65f);
			break;
		case 0x0007: // RunForward — ForwardSpeed is GetRunRate() (often > 1)
			Motion.Forward = ForwardSpeed >= 0.f ? 1.f : -1.f;
			Motion.ForwardUnitsPerSecond = 4.f * ForwardSpeed;
			Motion.AnimPlayRate = FMath::Max(0.05f, FMath::Abs(ForwardSpeed));
			Motion.bRunning = true;
			break;
		case ACEMotion::OnCommandU16:
		case ACEMotion::OffCommandU16:
			// Door / switch one-shot — not locomotion.
			break;
		default:
		{
			// Cast / MagicPowerUp / emotes are often ForwardCommand (EnqueueMotionMagic),
			// not MotionItems. Promote them to ActionCommand when Commands was empty.
			if (Motion.ActionCommand == 0 && ForwardCommand != 0)
			{
				const uint32 Full = ACEMotion::ExpandPackedCommand(ForwardCommand);
				const uint16 Low = static_cast<uint16>(Full);
				const bool bStance = (Full & 0xFF000000u) == 0x80000000u;
				if (!IsLocoOrDoorOrReady(Low) && !bStance)
				{
					Motion.ActionCommand = static_cast<int32>(Full);
					// Interpreted forward speed belongs to this substate, including
					// held emotes. Retail passes it through to DoInterpretedMotion.
					Motion.ActionSpeed = FMath::Max(0.f, ForwardSpeed);
				}
			}
			break;
		}
		}

		switch (SideStepCommand)
		{
		case 0x000f: // SideStepRight; negative speed means left
			Motion.Strafe = FMath::Clamp(SideStepSpeed, -1.f, 1.f);
			Motion.StrafeUnitsPerSecond = 1.25f * SideStepSpeed;
			if (FMath::IsNearlyZero(Motion.Forward))
			{
				Motion.AnimPlayRate = FMath::Max(0.05f, FMath::Abs(SideStepSpeed));
			}
			break;
		case 0x0010: // SideStepLeft
			Motion.Strafe = -FMath::Clamp(FMath::Abs(SideStepSpeed), 0.f, 1.f);
			Motion.StrafeUnitsPerSecond = -1.25f * FMath::Abs(SideStepSpeed);
			if (FMath::IsNearlyZero(Motion.Forward))
			{
				Motion.AnimPlayRate = FMath::Max(0.05f, FMath::Abs(SideStepSpeed));
			}
			break;
		default:
			break;
		}
		// Preserve direction ratios when interpreted rates exceed one (fast VR
		// strafing), while keeping the full physical speeds for prediction.
		const float DominantSpeed = FMath::Max(FMath::Abs(Motion.ForwardUnitsPerSecond), FMath::Abs(Motion.StrafeUnitsPerSecond));
		if (DominantSpeed > KINDA_SMALL_NUMBER)
		{
			Motion.Forward = Motion.ForwardUnitsPerSecond / DominantSpeed;
			Motion.Strafe = Motion.StrafeUnitsPerSecond / DominantSpeed;
			// CMotionInterp applies the forward cycle and sidestep modifier at
			// independent rates. A faster strafe must not accelerate the forward
			// animation/root motion (and make every F748 pull the player back).
		}
		if (TurnCommand == 0x000d || TurnCommand == 0x000e)
		{
			Motion.Turn = TurnCommand == 0x000e
				? -FMath::Abs(TurnSpeed)
				: TurnSpeed;
		}
	}
	else if (MovementType == 6 || MovementType == 7) // MoveToObject / MoveToPosition
	{
		if (MovementType == 6)
		{
			if (!Reader.CanRead(4))
			{
				return;
			}
			Motion.MoveToTargetGuid = static_cast<int32>(Reader.ReadUInt32());
		}
		// Origin (cell + xyz), MoveToParameters (flags + six floats), RunRate.
		if (!Reader.CanRead(16 + 28 + 4))
		{
			return;
		}
		Motion.MoveToCellId = static_cast<int32>(Reader.ReadUInt32());
		Motion.MoveToLocalAce.X = Reader.ReadFloat();
		Motion.MoveToLocalAce.Y = Reader.ReadFloat();
		Motion.MoveToLocalAce.Z = Reader.ReadFloat();
		Motion.bHaveMoveToTarget = Motion.MoveToCellId != 0;
		Motion.MoveToFlags = Reader.ReadUInt32(); // MovementParameters flags
		Motion.MoveToDistance = Reader.ReadFloat(); // distance / use radius
		Motion.MoveToMinDistance = Reader.ReadFloat();
		Motion.MoveToFailDistance = Reader.ReadFloat(); // fail distance (melee charge = 15; Use = MaxValue)
		// MaxValue means "no fail" on the server — treat as 0 for local checks.
		if (!FMath::IsFinite(Motion.MoveToFailDistance) || Motion.MoveToFailDistance == MAX_flt
			|| Motion.MoveToFailDistance < 0.f)
		{
			Motion.MoveToFailDistance = 0.f;
		}
		const float MoveSpeed = Reader.ReadFloat();
		Motion.MoveToSpeed = MoveSpeed;
		Motion.MoveToWalkRunThreshold = Reader.ReadFloat();
		Motion.MoveToDesiredHeading = Reader.ReadFloat();
		const float RunRate = Reader.ReadFloat();
		Motion.Forward = FMath::Clamp(FMath::Abs(MoveSpeed), 0.f, 1.f);
		Motion.MoveToRunRate = RunRate;
		FACEPosition MoveDestination; MoveDestination.CellId = Motion.MoveToCellId; MoveDestination.Location = Motion.MoveToLocalAce;
		const FACEWorldObject* Mover = WorldObjects.Find(Guid);
		const float Distance = Mover && Mover->bHasPosition
			? FVector::Dist2D(Mover->Position.ToUnrealLocation(1.f), MoveDestination.ToUnrealLocation(1.f)) : 0.f;
		Motion.UpdateMoveToGait(Distance);
		// Appearance keys off Forward amount + run flag — ensure a non-zero loco signal
		// whenever the server is actually moving us (Speed can be tiny but RunRate high).
		if (FMath::IsNearlyZero(Motion.Forward) && Motion.ForwardUnitsPerSecond > KINDA_SMALL_NUMBER)
		{
			Motion.Forward = 1.f;
		}
		Motion.ForwardCommand = Motion.bRunning
			? static_cast<int32>(0x0007) // RunForward packed
			: static_cast<int32>(0x0005); // WalkForward packed
	}
	else if (MovementType == 8) // TurnToObject
	{
		// TargetGuid, DesiredHeading, TurnToParameters (flags, speed, desiredHeading).
		if (!Reader.CanRead(4 + 4 + 4 + 4 + 4))
		{
			return;
		}
		Motion.MoveToTargetGuid = static_cast<int32>(Reader.ReadUInt32());
		Motion.TurnToFallbackHeading = Reader.ReadFloat();
		Motion.TurnToFlags = Reader.ReadUInt32();
		Motion.TurnToSpeed = Reader.ReadFloat();
		Motion.MoveToDesiredHeading = Reader.ReadFloat();
	}
	else if (MovementType == 9) // TurnToHeading
	{
		if (!Reader.CanRead(4 + 4 + 4))
		{
			return;
		}
		Motion.TurnToFlags = Reader.ReadUInt32();
		Motion.TurnToSpeed = Reader.ReadFloat();
		Motion.MoveToDesiredHeading = Reader.ReadFloat();
	}

	else return; // Unknown movement bodies must not consume a valid sequence.

	if (Motion.MoveToLocalAce.ContainsNaN() || !FMath::IsFinite(Motion.MoveToDistance)
		|| !FMath::IsFinite(Motion.MoveToMinDistance)
		|| !FMath::IsFinite(Motion.MoveToSpeed) || !FMath::IsFinite(Motion.MoveToRunRate)
		|| !FMath::IsFinite(Motion.MoveToWalkRunThreshold) || !FMath::IsFinite(Motion.MoveToDesiredHeading)
		|| !FMath::IsFinite(Motion.TurnToSpeed) || Motion.TurnToSpeed < 0.f
		|| !FMath::IsFinite(Motion.TurnToFallbackHeading)) return;
	if (DeferObjectPhysicsEvent(Guid, IncomingInstance, ACEOpcode::UpdateMotion, Reader, PayloadStart)) return;
	// Commit only after the entire movement body has validated. A malformed
	// update must not prevent a valid retransmission with the same timestamps.
	if (FACEWorldObject* Obj = WorldObjects.Find(Guid); Obj && Obj->bHasPhysicsTimestamps)
	{
		for (const auto& Stamp : {TPair<int32, uint16>(ACEPhysicsTimeStamp::Movement, IncomingMovement),
			TPair<int32, uint16>(ACEPhysicsTimeStamp::ServerControl, IncomingServerControl)})
			if (ACEPhysicsTimeStamp::IsNewer(Obj->PhysicsTimestamps[Stamp.Key], Stamp.Value))
				Obj->PhysicsTimestamps[Stamp.Key] = Stamp.Value;
	}

	if (Guid == PlayerGuid)
	{
		ServerControlSeq = IncomingServerControl;
		// Retail acknowledges its own autonomous echo without restarting local
		// movement. Remote players still need the same autonomous motion body.
		if (bAutonomous) return;
		if (Motion.MovementType == 0)
			CurrentForwardCommand = ACEMotion::NormalizeCommand(Motion.ForwardCommand);
	}
	if(auto* Obj=WorldObjects.Find(Guid))
	{Obj->ReceivedActionStamp=ActionStamp;Obj->bHasReceivedActionStamp=bHaveActionStamp;}

	Motion.bMoving = !FMath::IsNearlyZero(Motion.Forward)
		|| !FMath::IsNearlyZero(Motion.Strafe)
		|| !FMath::IsNearlyZero(Motion.Turn)
		|| Motion.MovementType == 6
		|| Motion.MovementType == 7;

	// Echo server stance back in our MoveToState so combat/magic mode is not wiped to NonCombat.
	// SwitchCombatStyles() briefly broadcasts NonCombat mid-transition — do NOT adopt that into
	// CurrentStance while CombatMode is still melee/missile/magic, or MoveToState undoes Magic.
	if (Guid == PlayerGuid && Motion.CurrentStyle != 0)
	{
		const uint32 Style = static_cast<uint32>(Motion.CurrentStyle);
		const bool bStyleNonCombat = Style == ACEMotion::StanceNonCombat;
		const bool bCombatModeActive = PlayerVitals.bValid
			&& PlayerVitals.CombatMode != static_cast<int32>(ACECombatMode::NonCombat)
			&& PlayerVitals.CombatMode != 0;
		if (!bStyleNonCombat || !bCombatModeActive)
		{
			SetCurrentStance(Style);
		}
	}

	// Persist door/chest resting pose for later ObjectCreate-equivalent recreates.
	if (Motion.ForwardCommand == ACEMotion::OnCommandU16
		|| Motion.ForwardCommand == ACEMotion::OffCommandU16)
	{
		if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
		{
			Obj->InitialMotionCommand = Motion.ForwardCommand;
		}
	}

	if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
	{
		const bool bDeath = Motion.IsDeathMotion();
		if (Obj->bDying && !Obj->bIsPlayer && !bDeath) return;
		Obj->bDying = bDeath && !Obj->IsCorpse() && !Obj->AllowsDeadPoseInteraction();
		if (bDeath)
		{
			Obj->InitialMotionCommand = ACEMotion::Dead;
			Obj->InitialMotionStyle = ACEMotion::StanceNonCombat;
			Obj->Velocity = FVector::ZeroVector;
			Obj->bHasVelocity = false;
			Motion.bMoving = false;
			Motion.Forward = Motion.Strafe = Motion.Turn = Motion.ForwardUnitsPerSecond = 0.f;
			if (Obj->bDying && SelectedObject.Guid == Guid) SelectObject(0);
		}
		else if (ACEMotion::NormalizeCommand(Obj->InitialMotionCommand) == ACEMotion::Dead)
		{
			Obj->InitialMotionCommand = ACEMotion::Ready;
		}
	}
	OnMotionUpdate.Broadcast(Guid, Motion);
}

void FACESession::HandleVectorUpdate(FACEBinaryReader& Reader)
{
	const int32 PayloadStart = Reader.Tell();
	// GameMessageVectorUpdate: guid, velocity xyz, omega xyz, instance seq, vector seq.
	if (!Reader.CanRead(4 + 12 + 12 + 4))
	{
		return;
	}
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	FVector AceVelocity;
	AceVelocity.X = Reader.ReadFloat();
	AceVelocity.Y = Reader.ReadFloat();
	AceVelocity.Z = Reader.ReadFloat();
	FVector AceOmega;
	AceOmega.X = Reader.ReadFloat();
	AceOmega.Y = Reader.ReadFloat();
	AceOmega.Z = Reader.ReadFloat();
	if (AceVelocity.ContainsNaN() || AceOmega.ContainsNaN()) return;
	const uint16 Instance = Reader.ReadUInt16();
	const uint16 VectorSequence = Reader.ReadUInt16();
	if (DeferObjectPhysicsEvent(Guid, Instance, ACEOpcode::VectorUpdate, Reader, PayloadStart)) return;

	if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
	{
		if (Obj->bHasPhysicsTimestamps && (Instance != Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance]
			|| !ACEPhysicsTimeStamp::IsNewer(Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Vector],VectorSequence))) return;
		Obj->bHasPhysicsTimestamps = true;
		Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Instance] = Instance;
		Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::Vector] = VectorSequence;
		Obj->Velocity = AceVelocity;
		Obj->bHasVelocity = !AceVelocity.IsNearlyZero();
		Obj->Omega = AceOmega;
	}
	OnVectorUpdate.Broadcast(Guid, AceVelocity, AceOmega);
}

void FACESession::HandleSetState(FACEBinaryReader& Reader)
{
	const int32 PayloadStart = Reader.Tell();
	if (!Reader.CanRead(12))
	{
		return;
	}
	const int32 Guid = static_cast<int32>(Reader.ReadUInt32());
	const int32 PhysicsState = static_cast<int32>(Reader.ReadUInt32());
	const uint16 IncomingInstance = Reader.ReadUInt16();
	const uint16 IncomingState = Reader.ReadUInt16();
	if (DeferObjectPhysicsEvent(Guid, IncomingInstance, ACEOpcode::SetState, Reader, PayloadStart)) return;
	if (FACEWorldObject* Obj = WorldObjects.Find(Guid))
	{
		if (Obj->bHasPhysicsTimestamps
			&& !ACEPhysicsTimeStamp::IsNewer(Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::State], IncomingState)) return;
		Obj->PhysicsTimestamps[ACEPhysicsTimeStamp::State] = IncomingState;
		// Forks can defer LoginComplete while destination objects load, ignoring
		// our first arrival pose. Re-report once their materialize acknowledgement
		// arrives, even if the player has remained completely still since then.
		if (Guid == PlayerGuid && (Obj->PhysicsState & ACEPhysicsState::Hidden)
			&& !(PhysicsState & ACEPhysicsState::Hidden))
			bHaveReportedPosition = false;
		Obj->PhysicsState = PhysicsState;
	}
	OnPhysicsStateUpdate.Broadcast(Guid, PhysicsState);
}

void FACESession::SendUseItem(int32 ObjectGuid)
{
	if (State != EACESessionState::InWorld || ObjectGuid == 0)
	{
		return;
	}
	if (const FACEWorldObject* Object = WorldObjects.Find(ObjectGuid); Object && Object->IsGameBoard())
	{
		SendChessJoin(ObjectGuid); // CM_Game::Event_Join, same approach/interaction as retail.
		return;
	}
	// Never turn a puzzle/quest door into a player-operated door. Switches and keys
	// use their own server actions; only the server may grant/open the target door.
	if (const FACEWorldObject* Object = WorldObjects.Find(ObjectGuid); Object && Object->IsDirectDoorUseBlocked())
	{
		EmitChatMessage(TEXT("This door cannot be activated from here."), TEXT(""), ACEChatMessageType::TransientInfo);
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(ObjectGuid));
	SendGameAction(ACEGameAction::Use, W.GetData(), ACEQueue::WeenieQueue);
	BeginPendingUse(ObjectGuid, 0);
	Log(FString::Printf(TEXT("Use item guid=0x%08X"), ObjectGuid));
}

void FACESession::SendChangeCombatMode(uint32 CombatMode)
{
	// An explicit stance change during a pending swap supersedes its old intent.
	if (PendingEquipmentGuid) PendingEquipmentCombatMode = CombatMode;
	if (State != EACESessionState::InWorld)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(CombatMode);
	SendGameAction(ACEGameAction::ChangeCombatMode, W.GetData(), ACEQueue::WeenieQueue);
	// Optimistic CombatMode so SwitchCombatStyles' transient NonCombat UpdateMotion does not
	// overwrite CurrentStance / wipe Magic before PropertyInt.CombatMode arrives.
	PlayerVitals.CombatMode = static_cast<int32>(CombatMode);
	DeferredServerCombatMode = INDEX_NONE;
	if (Creds.bGDLE || CombatMode == ACECombatMode::NonCombat)
	{
		PendingCombatMode = 0;
		PendingCombatModeUntil = 0.0;
	}
	else
	{
		PendingCombatMode = CombatMode;
		PendingCombatModeUntil = FPlatformTime::Seconds() + 5.0;
	}
	// GDLE ChangeCombatMode(mode, playerRequested=true) omits CombatMode on
	// success, and may omit motion too when already in that stance. Retail uses
	// the requested mode immediately; the following action shares this reliable,
	// ordered weenie queue. Preserve the received quality and honor subsequent
	// corrections normally, including an immediate server rejection to peace.
	if (Creds.bGDLE) ++CombatModeRevision;
	TArray<FACEWorldObject> Equipped;
	GetEquippedItems(Equipped);
	const uint32 Stance = ACECombatStance::ResolveForCombatMode(Equipped, CombatMode);
	SetCurrentStance(Stance);
	Log(FString::Printf(TEXT("ChangeCombatMode=%u stance=0x%08X"), CombatMode, Stance));
	// Controller actions also change stance outside the retail binder. Notify
	// all UI consumers now so its mode icon and the copied wrist panel agree.
	OnVitalsUpdated.Broadcast(PlayerVitals);
}

void FACESession::SendTargetedMeleeAttack(int32 TargetGuid, uint32 AttackHeight, float PowerLevel)
{
	if (State != EACESessionState::InWorld || TargetGuid == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(TargetGuid));
	W.WriteUInt32(AttackHeight);
	W.WriteFloat(FMath::Clamp(PowerLevel, 0.f, 1.f));
	SendGameAction(ACEGameAction::TargetedMeleeAttack, W.GetData(), ACEQueue::WeenieQueue);
	bServerAttackInProgress = true;
	LastAttackError = 0;
	++CombatEventRevision;
}

void FACESession::SendTargetedMissileAttack(int32 TargetGuid, uint32 AttackHeight, float AccuracyLevel)
{
	if (State != EACESessionState::InWorld || TargetGuid == 0)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(TargetGuid));
	W.WriteUInt32(AttackHeight);
	W.WriteFloat(FMath::Clamp(AccuracyLevel, 0.f, 1.f));
	SendGameAction(ACEGameAction::TargetedMissileAttack, W.GetData(), ACEQueue::WeenieQueue);
	bServerAttackInProgress = true;
	LastAttackError = 0;
	++CombatEventRevision;
}

bool FACESession::CreateCharacter(const FACECGSelection& Selection)
{
    if (State != EACESessionState::CharacterSelect || !SocketC2S || !ServerC2SAddr.IsValid() || !IssacClient || bCharacterCreationPending || PendingCharacterMutation != 0 || Characters.Num() >= CharacterSlotCount) return false;
    FACEBinaryWriter Payload; Payload.WriteString16L(AccountName); Selection.Write(Payload);
    bCharacterCreationPending = true;
    SendGameMessage(0xF656, Payload.GetData(), ACEQueue::UIQueue, true);
    return true;
}
void FACESession::HandleCharacterCreated(FACEBinaryReader& Reader)
{
    if (!bCharacterCreationPending || State != EACESessionState::CharacterSelect || !Reader.CanRead(4)) return;
    const uint32 Result = Reader.ReadUInt32();
    if (Result == 2) return; // Verification pending: retain the draft and prevent duplicate sends.
    FACECharacterInfo Info;
    if (Result == 1)
    {
        if (!Reader.CanRead(6)) return;
        Info.CharacterId = Reader.ReadUInt32();
        const int32 StringStart = Reader.Tell(); const uint16 Length = Reader.ReadUInt16();
        Reader.Seek(StringStart);
        if (!Reader.CanRead(((Length + 2 + 3) & ~3) + 4)) return;
        Info.Name = Reader.ReadString16L(); Reader.ReadUInt32();
        if (!Info.CharacterId || Info.Name.IsEmpty()) return;
        Characters.RemoveAll([&](const FACECharacterInfo& C) { return C.CharacterId == Info.CharacterId; });
        Characters.Add(Info);
        PreferredCharacterId = Info.CharacterId;
    }
    bCharacterCreationPending = false;
    OnCharacterCreated.Broadcast(Result, Info);
}

bool FACESession::BuildCharacterMutation(int32 CharacterId, bool bRestore, TArray<uint8>& Payload) const
{
	Payload.Reset();
	if (State != EACESessionState::CharacterSelect || PendingCharacterMutation || bCharacterCreationPending) return false;
	const int32 Slot = Characters.IndexOfByPredicate([CharacterId](const FACECharacterInfo& C) { return C.CharacterId == CharacterId; });
	if (Slot == INDEX_NONE || (Characters[Slot].DeleteSeconds != 0) != bRestore || AccountName.IsEmpty()) return false;
	FACEBinaryWriter Writer;
	// CPlayerSystem::DeleteCharacter sends account + original server-list slot;
	// RestoreCharacter sends the GUID. Never sort the backing roster.
	if (bRestore) Writer.WriteUInt32(static_cast<uint32>(CharacterId));
	else { Writer.WriteString16L(AccountName); Writer.WriteUInt32(static_cast<uint32>(Slot)); }
	Payload = Writer.GetData();
	return true;
}

bool FACESession::DeleteCharacter(int32 CharacterId)
{
	TArray<uint8> Payload;
	if (!SocketC2S || !ServerC2SAddr.IsValid() || !IssacClient || !BuildCharacterMutation(CharacterId, false, Payload)) return false;
	PendingCharacterMutation = CharacterId; bRestoringCharacter = false;
	CharacterMutationSentAt = FPlatformTime::Seconds(); CharacterManagementError.Reset();
	SendGameMessage(0xF655, Payload, ACEQueue::UIQueue, true);
	return true;
}

bool FACESession::RestoreCharacter(int32 CharacterId)
{
	TArray<uint8> Payload;
	if (!SocketC2S || !ServerC2SAddr.IsValid() || !IssacClient || !BuildCharacterMutation(CharacterId, true, Payload)) return false;
	PendingCharacterMutation = CharacterId; bRestoringCharacter = true;
	CharacterMutationSentAt = FPlatformTime::Seconds(); CharacterManagementError.Reset();
	SendGameMessage(0xF7D9, Payload, ACEQueue::UIQueue, true);
	return true;
}

void FACESession::HandleCharacterRestored(FACEBinaryReader& Reader)
{
	if (State != EACESessionState::CharacterSelect || !PendingCharacterMutation || !bRestoringCharacter || !Reader.CanRead(4)) return;
	const uint32 Result = Reader.ReadUInt32();
	if (Result == 2) return; // Verification still in progress.
	if (Result != 1)
	{
		CharacterManagementError = Result == 3 ? TEXT("That character name is now in use. The character could not be restored.")
			: TEXT("The server could not restore this character.");
		PendingCharacterMutation = 0;
		return;
	}
	if (!Reader.CanRead(6)) return;
	FACECharacterInfo Info; Info.CharacterId = Reader.ReadUInt32();
	const int32 Start = Reader.Tell(); const uint16 Length = Reader.ReadUInt16(); Reader.Seek(Start);
	if (!Reader.CanRead(((Length + 2 + 3) & ~3) + 4)) return;
	Info.Name = Reader.ReadString16L(); Info.DeleteSeconds = Reader.ReadUInt32();
	if (Info.CharacterId != PendingCharacterMutation || Info.Name.IsEmpty()) return;
	if (auto* Existing = Characters.FindByPredicate([&](const FACECharacterInfo& C) { return C.CharacterId == Info.CharacterId; })) *Existing = Info;
	PendingCharacterMutation = 0; CharacterManagementError.Reset();
	OnCharacterList.Broadcast(Characters, ServerName);
}

bool FACESession::EnterWorld(int32 CharacterId)
{
	if (State != EACESessionState::CharacterSelect || PendingCharacterMutation) return false;
	const auto* Character=Characters.FindByPredicate([CharacterId](const FACECharacterInfo& C)
		{ return C.CharacterId==CharacterId; });
	if (!Character || Character->DeleteSeconds!=0) return false;
	PendingEnterCharacterId = CharacterId;
	bEnteredWorldSent = false;
	SetState(EACESessionState::EnteringWorld);
	SendGameMessage(ACEOpcode::CharacterEnterWorldRequest, {}, ACEQueue::UIQueue, true);
	Log(FString::Printf(TEXT("EnterWorld request character=%d — awaiting server ready"), CharacterId));
	return true;
}

bool FACESession::EnterWorldByName(const FString& CharacterName)
{
	for (const FACECharacterInfo& C : Characters)
	{
		if (C.Name.Equals(CharacterName, ESearchCase::IgnoreCase) ||
			C.Name.Equals(TEXT("+") + CharacterName, ESearchCase::IgnoreCase))
		{
			return EnterWorld(C.CharacterId);
		}
	}
	Log(FString::Printf(TEXT("Character '%s' not found"), *CharacterName));
	return false;
}

void FACESession::SendLoginComplete()
{
	// Safe to call on every portal exit — ACE OnTeleportComplete clears Teleporting each time.
	SendGameAction(ACEGameAction::LoginComplete, {}, ACEQueue::ControlQueue);
	bLoginCompleteSent = true;
	bPositionReportingSuspended = false;
	bHaveReportedPosition = false;
	Log(TEXT("LoginComplete (exited portal space)"));
	MaybeEnterWorldComplete();
}

void FACESession::SetCurrentStance(uint32 Stance)
{
	uint32 Expanded = Stance;
	if (Expanded != 0 && Expanded <= 0xFFFFu)
	{
		Expanded = ACEMotion::ExpandPackedCommand(static_cast<uint16>(Expanded));
	}
	if ((Expanded & 0xFF000000u) == 0x80000000u)
	{
		if (CurrentStance != Expanded) ClearHeldForwardCommand();
		CurrentStance = Expanded;
	}
}

void FACESession::SendMoveToState(float Forward, float Strafe, float Turn, bool bRunning, bool bContact,
	bool bStandingLongJump, bool bUniformVRMovement, bool bMouseTurn)
{
	if (State != EACESessionState::InWorld || bLogOffPending)
	{
		return;
	}

	// Same outdoor Z guard as AutonomousPosition — bad MoveToState poses desync movement.
	{
		const uint32 Cell = static_cast<uint32>(PlayerPosition.CellId);
		if ((Cell & 0xFFFFu) < 0x0100u && PlayerPosition.Location.Z < -40.f)
		{
			return;
		}
	}

	bUniformVRMovement &= SupportsUniformVRMovement();
	if (bUniformVRMovement)
	{
		const FVector2D Stick = ACEVRLocomotion::Input(Forward, Strafe);
		Forward = Stick.Y; Strafe = Stick.X;
	}
	uint32 Flags = ACERawMotionFlags::CurrentHoldKey | ACERawMotionFlags::CurrentStyle;
	uint32 ForwardCommand = ACEMotion::Ready;
	float ForwardSpeed = 0.f;
	uint32 SidestepCommand = 0;
	float SidestepSpeed = 0.f;
	uint32 TurnCommand = 0;
	float TurnSpeed = 0.f;

	if (!FMath::IsNearlyZero(Forward))
	{
		Flags |= ACERawMotionFlags::ForwardCommand | ACERawMotionFlags::ForwardHoldKey | ACERawMotionFlags::ForwardSpeed;
		// Retail sends WalkForward/WalkBackwards and uses CurrentHoldKey=Run for sprint.
		// Sending RunForward directly skips MovementData's ForwardSpeed=GetRunRate() path.
		if (Forward > 0.f)
		{
			ForwardCommand = ACEMotion::WalkForward;
		}
		else
		{
			ForwardCommand = ACEMotion::WalkBackwards;
		}
		ForwardSpeed = FMath::Clamp(FMath::Abs(Forward), 0.f, 1.f);
	}
	else if (ACEMotion::IsHeldPoseSubstate(CurrentForwardCommand))
	{
		Flags |= ACERawMotionFlags::ForwardCommand | ACERawMotionFlags::ForwardHoldKey | ACERawMotionFlags::ForwardSpeed;
		ForwardCommand = CurrentForwardCommand;
		ForwardSpeed = 1.f;
	}

	if (!FMath::IsNearlyZero(Strafe))
	{
		Flags |= ACERawMotionFlags::SideStepCommand | ACERawMotionFlags::SideStepHoldKey | ACERawMotionFlags::SideStepSpeed;
		SidestepCommand = Strafe > 0.f ? ACEMotion::SideStepRight : ACEMotion::SideStepLeft;
		SidestepSpeed = FMath::Clamp(FMath::Abs(Strafe), 0.f, 1.f);
	}

	if (!FMath::IsNearlyZero(Turn))
	{
		Flags |= ACERawMotionFlags::TurnCommand | ACERawMotionFlags::TurnHoldKey | ACERawMotionFlags::TurnSpeed;
		TurnCommand = ACEMotion::TurnRight;
		TurnSpeed = FMath::Clamp(Turn, bMouseTurn ? -1.5f : -1.f, bMouseTurn ? 1.5f : 1.f);
	}

	FACEBinaryWriter W;
	W.WriteUInt32(Flags);
	W.WriteUInt32(bRunning ? ACEMotion::HoldKeyRun : ACEMotion::HoldKeyNone);
	// Must match server CurrentMotionState.Stance — hardcoding NonCombat forced peace mode
	// on every MoveToState (retail sends the active MotionStance here).
	W.WriteUInt32(CurrentStance != 0 ? CurrentStance : ACEMotion::StanceNonCombat);

	if (Flags & ACERawMotionFlags::ForwardCommand) W.WriteUInt32(ForwardCommand);
	if (Flags & ACERawMotionFlags::ForwardHoldKey) W.WriteUInt32(ACEMotion::HoldKeyInvalid);
	if (Flags & ACERawMotionFlags::ForwardSpeed) W.WriteFloat(ForwardSpeed);
	if (Flags & ACERawMotionFlags::SideStepCommand) W.WriteUInt32(SidestepCommand);
	if (Flags & ACERawMotionFlags::SideStepHoldKey) W.WriteUInt32(ACEMotion::HoldKeyInvalid);
	if (Flags & ACERawMotionFlags::SideStepSpeed) W.WriteFloat(SidestepSpeed);
	if (Flags & ACERawMotionFlags::TurnCommand) W.WriteUInt32(TurnCommand);
	if (Flags & ACERawMotionFlags::TurnHoldKey) W.WriteUInt32(bMouseTurn ? ACEMotion::HoldKeyRun : ACEMotion::HoldKeyInvalid);
	if (Flags & ACERawMotionFlags::TurnSpeed) W.WriteFloat(TurnSpeed);

	// Position
	W.WriteUInt32(static_cast<uint32>(PlayerPosition.CellId));
	W.WriteFloat(PlayerPosition.Location.X);
	W.WriteFloat(PlayerPosition.Location.Y);
	W.WriteFloat(PlayerPosition.Location.Z);
	W.WriteFloat(PlayerPosition.RotationW);
	W.WriteFloat(PlayerPosition.RotationXYZ.X);
	W.WriteFloat(PlayerPosition.RotationXYZ.Y);
	W.WriteFloat(PlayerPosition.RotationXYZ.Z);

	W.WriteUInt16(InstanceSeq);
	W.WriteUInt16(ServerControlSeq);
	W.WriteUInt16(TeleportSeq);
	W.WriteUInt16(ForcePositionSeq);
	uint8 ContactFlags = 0;
	if (bContact)
	{
		ContactFlags |= 0x01;
	}
	if (bStandingLongJump)
	{
		ContactFlags |= 0x02;
	}
	W.WriteUInt8(ContactFlags);
	W.Align();
	// Only negotiated servers consume this extension; the retail packet is unchanged otherwise.
	if (bUniformVRMovement) W.WriteUInt32(ACEVRLocomotion::MoveMarker);

	SendGameAction(ACEGameAction::MoveToState, W.GetData(), ACEQueue::WeenieQueue);
	CurrentForwardCommand = ForwardCommand;
	// Retail SendMovementEvent carries the current position and resets the
	// periodic position timer. Do not immediately duplicate it with AutoPos.
	AutoPosTimer = 0.f;
	// Airborne physics remains active even with zero input. Ordinary position
	// reports resume on landing; negotiated VR tracking reports its own feet.
	bMoving = !FMath::IsNearlyZero(Forward) || !FMath::IsNearlyZero(Strafe)
		|| !FMath::IsNearlyZero(Turn) || !bContact;
	bAutoPosContact = bContact;
}

void FACESession::SendJump(float Extent, const FVector& LocalAceVelocity)
{
	if (State != EACESessionState::InWorld || bLogOffPending)
	{
		return;
	}
	FACEBinaryWriter W;
	W.WriteFloat(FMath::Clamp(Extent, 0.f, 1.f));
	W.WriteFloat(LocalAceVelocity.X);
	W.WriteFloat(LocalAceVelocity.Y);
	W.WriteFloat(LocalAceVelocity.Z);
	W.WriteUInt16(InstanceSeq);
	W.WriteUInt16(ServerControlSeq);
	W.WriteUInt16(TeleportSeq);
	W.WriteUInt16(ForcePositionSeq);
	// ACE.Server GameActionJump reads these after JumpPack (unused; retail pads them).
	W.WriteUInt32(0);
	W.WriteUInt32(0);
	SendGameAction(ACEGameAction::Jump, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::SendStopMovement()
{
	ClearHeldForwardCommand();
	// Retail CmdInterp::send_move_to_state derives contact from physics, not
	// the input axes. Cancelling a use/route while falling is not a landing.
	SendMoveToState(0.f, 0.f, 0.f, false, bAutoPosContact, false);
}

void FACESession::SendAutonomousPosition(bool bContact, bool bTrackedVR)
{
	if (!bContact && !(bTrackedVR && (SupportsVRPoses() || SupportsVRCombat()))) return;
	if (State != EACESessionState::InWorld || !PlayerPosition.IsValid() || bLogOffPending || bPositionReportingSuspended)
	{
		return;
	}

	// Outdoor: never report a deep underground Z — client fall-through poisons the
	// server pose and causes desync / bad re-login spawn (CharacterError / rubber-band).
	const uint32 Cell = static_cast<uint32>(PlayerPosition.CellId);
	if ((Cell & 0xFFFFu) < 0x0100u && PlayerPosition.Location.Z < -40.f)
	{
		return;
	}

	FACEBinaryWriter W;
	W.WriteUInt32(static_cast<uint32>(PlayerPosition.CellId));
	W.WriteFloat(PlayerPosition.Location.X);
	W.WriteFloat(PlayerPosition.Location.Y);
	W.WriteFloat(PlayerPosition.Location.Z);
	W.WriteFloat(PlayerPosition.RotationW);
	W.WriteFloat(PlayerPosition.RotationXYZ.X);
	W.WriteFloat(PlayerPosition.RotationXYZ.Y);
	W.WriteFloat(PlayerPosition.RotationXYZ.Z);
	W.WriteUInt16(InstanceSeq);
	W.WriteUInt16(ServerControlSeq);
	W.WriteUInt16(TeleportSeq);
	W.WriteUInt16(ForcePositionSeq);
	W.WriteUInt8(bContact ? 1 : 0);
	W.Align();

	SendGameAction(ACEGameAction::AutonomousPosition, W.GetData(), ACEQueue::SecureWeenieQueue);
	LastReportedPosition = PlayerPosition;
	bHaveReportedPosition = true;
	bLastReportedContact = bContact;
	AutoPosTimer = 0.f;
}

void FACESession::FlushAutonomousPosition(bool bContact, bool bTrackedVR)
{
	bAutoPosContact = bContact;
	SendAutonomousPosition(bContact, bTrackedVR);
	AutoPosTimer = 0.f;
}

bool FACESession::ApplyPlayerInventoryCapacity(FACEWorldObject& Object) const
{
	if (!PlayerGuid || Object.Guid != PlayerGuid) return false;
	bool Changed = false;
	auto Apply = [&](int32 Property, int32& Capacity)
	{
		if (const int32* Value = PlayerVitals.StatQualityInts.Find(Property); Value && Capacity != *Value)
		{
			Capacity = *Value;
			Changed = true;
		}
	};
	// ACE grants the pack augmentation with a private ContainersCapacity update.
	// Keep the inventory/UI/pickup view in sync, including PlayerDescription and
	// CreateObject arriving in either order. The server supplies the final capacity;
	// adding AugmentationExtraPackSlot here would count the bonus twice.
	Apply(6, Object.ItemsCapacity);
	Apply(7, Object.ContainersCapacity);
	return Changed;
}

void FACESession::NotifyVitalsChanged()
{
	if (FACEWorldObject* Self = WorldObjects.Find(PlayerGuid); Self && ApplyPlayerInventoryCapacity(*Self))
		++InventoryDataRevision;
	if (StatResolver) { StatResolver(PlayerVitals, ActiveEnchantments); }
	OnVitalsUpdated.Broadcast(PlayerVitals);
}
