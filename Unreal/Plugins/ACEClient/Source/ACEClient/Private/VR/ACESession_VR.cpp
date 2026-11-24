#include "ACESession.h"
#include "VR/ACEVRLocomotion.h"

void FACESession::HandleVRCasting(FACEBinaryReader& R)
{
	if (State != EACESessionState::InWorld || !SupportsVRCasting() || R.Remaining() != 24) return;
	const uint32 Sequence=R.ReadUInt32(), Epoch=R.ReadUInt32(), Spell=R.ReadUInt32(), Phase=R.ReadUInt32();
	const float Remaining=R.ReadFloat(), Duration=R.ReadFloat();
	if (!Sequence || int32(Sequence-VRSequence)>0 || int32(Sequence-VRCastSequence)<0 || Epoch!=TeleportSeq || Phase>2
		|| !FMath::IsFinite(Remaining) || !FMath::IsFinite(Duration) || Remaining<0 || Duration<Remaining || Duration>60) return;
	// Same-cast packets advance monotonically: windup -> recoil -> ready.
	if (Sequence==VRCastSequence && (Phase==1 || VRCastPhase==0)) return;
	VRCastSequence=Sequence; VRCastEpoch=Epoch; VRCastPhase=Phase;
	VRCastDuration=Duration; VRCastReadyAt=FPlatformTime::Seconds()+Remaining;
	if (VRAimCastSequence==Sequence) VRAimUntil=Phase==1 ? VRCastReadyAt+2. : 0.;
}

uint32 FACESession::GetVRCastPhase() const
{
	return State==EACESessionState::InWorld && SupportsVRCasting() && VRCastEpoch==TeleportSeq
		&& FPlatformTime::Seconds()<VRCastReadyAt+5. ? VRCastPhase : 0;
}
float FACESession::GetVRCastRemaining() const
{
	return GetVRCastPhase()==1 ? FMath::Max(0.,VRCastReadyAt-FPlatformTime::Seconds()) : 0.f;
}
float FACESession::GetVRCastProgress() const
{
	return VRCastDuration>0 ? FMath::Clamp(1.f-GetVRCastRemaining()/VRCastDuration,0.f,1.f) : 1.f;
}

void FACESession::SendVRAim(uint32 Cell, int32 Weapon, const FVector& Origin, const FVector& Direction)
{
	const double Now=FPlatformTime::Seconds();
	if (!SupportsVRCasting() || State!=EACESessionState::InWorld || Weapon!=VRAimWeapon || VRCastEpoch!=TeleportSeq
		|| Now>=VRAimUntil || Now<VRNextAim || Origin.ContainsNaN() || Direction.ContainsNaN()) return;
	VRNextAim=Now+.05;
	FACEBinaryWriter W;
	for (uint32 Value : {1u,7u,++VRSequence,Cell,uint32(TeleportSeq),uint32(Weapon),uint32(VRAimSpell),VRAimCastSequence}) W.WriteUInt32(Value);
	for (const auto& V : {Origin,Direction}) { W.WriteFloat(V.X); W.WriteFloat(V.Y); W.WriteFloat(V.Z); }
	W.WriteFloat(0); W.WriteFloat(0);
	SendGameAction(0xF7D0,W.GetData(),ACEQueue::WeenieQueue);
}

void FACESession::HandleVRRecovery(FACEBinaryReader& R)
{
	if (State != EACESessionState::InWorld || !SupportsVRRecovery() || R.Remaining() != 16) return;
	const uint32 Sequence = R.ReadUInt32(), Epoch = R.ReadUInt32();
	const float Remaining = R.ReadFloat(), Duration = R.ReadFloat();
	if (!Sequence || int32(Sequence - VRSequence) > 0 || int32(Sequence - VRRecoverySequence) <= 0
		|| Epoch != TeleportSeq || !FMath::IsFinite(Remaining) || !FMath::IsFinite(Duration)
		|| Remaining < 0 || Duration <= 0 || Remaining > Duration || Duration > 60) return;
	VRRecoverySequence = Sequence; VRRecoveryTeleport = Epoch;
	VRRecoveryDuration = Duration; VRRecoveryReadyAt = FPlatformTime::Seconds() + Remaining;
}

float FACESession::GetVRRecoveryRemaining() const
{
	return State == EACESessionState::InWorld && SupportsVRRecovery() && VRRecoveryTeleport == TeleportSeq
		? FMath::Max(0., VRRecoveryReadyAt - FPlatformTime::Seconds()) : 0.f;
}

float FACESession::GetVRRecoveryProgress() const
{
	return VRRecoveryDuration > 0 ? FMath::Clamp(1.f - GetVRRecoveryRemaining() / VRRecoveryDuration, 0.f, 1.f) : 1.f;
}

bool FACESession::SendVRDrop(uint32 Cell, int32 Item, int32 SplitAmount, const FVector& OriginAc)
{
	if (State != EACESessionState::InWorld || !SupportsVRDrops() || !Item || SplitAmount < 0
		|| OriginAc.ContainsNaN() || OriginAc.SizeSquared2D() > FMath::Square(1.75f)
		|| OriginAc.Z < .1f || OriginAc.Z > 2.5f) return false;
	FACEBinaryWriter W;
	W.WriteUInt32(1); W.WriteUInt32(6); W.WriteUInt32(++VRSequence); W.WriteUInt32(Cell);
	W.WriteUInt32(TeleportSeq); W.WriteUInt32(Item); W.WriteUInt32(SplitAmount);
	W.WriteFloat(OriginAc.X); W.WriteFloat(OriginAc.Y); W.WriteFloat(OriginAc.Z);
	FlushAutonomousPosition(bAutoPosContact, true);
	SendGameAction(0xF7D0, W.GetData(), ACEQueue::WeenieQueue);
	return true;
}

bool FACESession::SendVRPose(FACEVRPose Pose)
{
	if (State != EACESessionState::InWorld || !SupportsVRPoses()) return false;
	const uint32 Version=(VRCapabilities & 32768u) ? 2u : 1u;
	FACEBinaryWriter W; W.WriteUInt32(Version); W.WriteUInt32(++VRPoseSequence); W.WriteUInt32(Pose.Cell);
	W.WriteUInt32(TeleportSeq); W.WriteUInt32(Pose.Flags); W.WriteFloat(Pose.EyeHeight); W.WriteFloat(Pose.Draw);
	for (int32 I=0;I<(Version==2 ? 5 : 3);++I)
	{
		if (I==3) { W.WriteUInt32(Pose.Weapon); W.WriteUInt32(Pose.Ammo); }
		const FTransform& T=Pose.Poses[I];
		const FVector P = T.GetLocation(); const FQuat Q = T.GetRotation();
		if (P.ContainsNaN() || Q.ContainsNaN()) return false;
		for (double V : {P.X, P.Y, P.Z, Q.X, Q.Y, Q.Z, Q.W}) W.WriteFloat(V);
	}
	// Keep the authoritative feet report on the pose cadence. Independent
	// timers can otherwise attach fresh hands to the previous position.
	FlushAutonomousPosition(bAutoPosContact, true);
	SendGameAction(0xF7D1, W.GetData(), ACEQueue::WeenieQueue);
	LastVRPoseSent=FPlatformTime::Seconds();
	if (VRPoseSequence == 1) Log(TEXT("VR pose stream started: head and both hands, 20 Hz"));
	return true;
}

void FACESession::HandleVRPose(FACEBinaryReader& R)
{
	if (State != EACESessionState::InWorld || !SupportsVRPoses() || (R.Remaining() != 116 && R.Remaining()!=192)) return;
	const int32 Size=R.Remaining();
	const int32 Guid = static_cast<int32>(R.ReadUInt32());
	const auto* Object = WorldObjects.Find(Guid);
	const uint32 Version=R.ReadUInt32();
	if (!Object || !Object->bIsPlayer || Guid == PlayerGuid || (Version!=1 && Version!=2)
		|| Size!=(Version==2 ? 192 : 116)) return;
	FACEVRPose P; P.Version=Version; P.Sequence = R.ReadUInt32(); P.Cell = R.ReadUInt32(); P.Teleport = R.ReadUInt32(); P.Flags = R.ReadUInt32();
	P.EyeHeight = R.ReadFloat(); P.Draw = R.ReadFloat();
	if ((P.Flags & ~31u) || !FMath::IsFinite(P.EyeHeight) || P.EyeHeight < 1 || P.EyeHeight > 2.1f || !FMath::IsFinite(P.Draw)) return;
	for (int32 I=0;I<(Version==2 ? 5 : 3);++I)
	{
		if (I==3) { P.Weapon=R.ReadInt32(); P.Ammo=R.ReadInt32(); }
		FTransform& T=P.Poses[I];
		const float X = R.ReadFloat(), Y = R.ReadFloat(), Z = R.ReadFloat();
		const float QX = R.ReadFloat(), QY = R.ReadFloat(), QZ = R.ReadFloat(), QW = R.ReadFloat();
		FQuat Q(QX, QY, QZ, QW); FVector V(X, Y, Z);
		if (V.ContainsNaN() || V.SizeSquared() > 25 || Q.ContainsNaN() || !FMath::IsWithinInclusive(Q.SizeSquared(), .8, 1.2)) return;
		Q.Normalize(); T = FTransform(Q, V);
	}
	if (Version==2)
	{
		const float X=R.ReadFloat(),Y=R.ReadFloat(),Z=R.ReadFloat(); P.Root=FVector(X,Y,Z);
		if (P.Root.ContainsNaN() || P.Root.GetAbsMax()>100000.f) return;
	}
	P.ReceivedAt = FPlatformTime::Seconds();
	auto& Samples = VRPoses.FindOrAdd(Guid);
	if (Samples.Current.ReceivedAt == 0) Log(FString::Printf(TEXT("VR pose stream received from 0x%08X"), Guid));
	Samples.Add(P);
}

bool FACESession::GetVRPose(int32 Guid, FACEVRPose& P) const
{
	const auto* S = VRPoses.Find(Guid); const auto* Object = WorldObjects.Find(Guid);
	const double Now = FPlatformTime::Seconds();
	if (!S || !Object || !Object->bHasPosition || !(S->Current.Flags & 4u) || Now - S->Current.ReceivedAt > .5) return false;
	if (Object->bHasPhysicsTimestamps && Object->PhysicsTimestamps[ACEPhysicsTimeStamp::Teleport] != S->Current.Teleport) return false;
	P = S->Sample(Now);
	return true;
}

void FACESession::ApplyVRWorldSnapshot(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(8)) return;
	const uint32 Epoch = Reader.ReadUInt32(), Count = Reader.ReadUInt32();
	const uint32 Trailer = (VRCapabilities & 32u) ? 8u : 0u;
	if (Epoch != TeleportSeq || Count > 10000u || Reader.Remaining() != Count * 4u + Trailer) return;
	TSet<int32> Live;
	for (uint32 I = 0; I < Count; ++I) Live.Add(static_cast<int32>(Reader.ReadUInt32()));
	auto Owned = [&](const FACEWorldObject& Object)
	{
		// Containers/vendor stock are sent by UI events, not the physics known-object
		// list. Membership reconciliation must not empty an open shop or loot bag.
		if (Object.ContainerId != 0) return true;
		if (Object.Guid == PlayerGuid || Object.WielderId == PlayerGuid || Object.ParentGuid == PlayerGuid) return true;
		return Live.Contains(Object.WielderId) || Live.Contains(Object.ParentGuid);
	};
	TArray<int32> Removed;
	for (const auto& Pair : WorldObjects) if (!Live.Contains(Pair.Key) && !Owned(Pair.Value)) Removed.Add(Pair.Key);
	for (int32 Guid : Removed)
	{
		if (SelectedObject.Guid == Guid) SelectObject(0);
		VRPoses.Remove(Guid);
		WorldObjects.Remove(Guid); RemoveFromContainerLists(Guid); OnObjectDeleted.Broadcast(Guid);
	}
	Log(FString::Printf(TEXT("VR world snapshot: live=%d removed=%d epoch=%u"), Live.Num(), Removed.Num(), Epoch));
}
#include "Protocol/ACEBinaryWriter.h"

void FACESession::SendCancelAttack()
{
	if (State == EACESessionState::InWorld) SendGameAction(ACEGameAction::CancelAttack, {}, ACEQueue::WeenieQueue);
}

void FACESession::SendVRSubscriptions()
{
	if (State != EACESessionState::InWorld) return;
	uint32 Features = 0;
	if (bLocalVRFeedback)
		Features = (SupportsHealthFeedback() ? 1u : 0u) | (SupportsVRRecovery() ? 2u : 0u)
			| (SupportsVRCasting() ? 4u : 0u) | ((VRCapabilities & 8192u) ? 8u : 0u);
	if (SupportsVRPoses() && (VRCapabilities & 131072u))
		Features |= 16u | ((VRCapabilities & 32768u) ? 32u : 0u);
	if (SupportsUniformVRMovement()) Features |= ACEVRLocomotion::Subscription;
	if (!Features) return;
	FACEBinaryWriter W; W.WriteUInt32(1); W.WriteUInt32(4); W.WriteUInt32(Features);
	SendGameAction(0xF7D0, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::RequestVRCapabilities(bool bLocalVR)
{
	if (State != EACESessionState::InWorld) return;
	if (bLocalVR && !bLocalVRFeedback)
	{
		// A headset can activate after the shared receive-only handshake. Enable
		// its UI feedback immediately, even if the server rate-limits this hello.
		bLocalVRFeedback = true;
		SendVRSubscriptions();
	}
	if (!VRCapabilities)
	{
		// Vanilla ACE handles PingRequest and ignores its payload. Discover the
		// extension once without sending an unknown action on ordinary shards.
		// Extension servers recognize the optional ACUE/version trailer and reply
		// with capabilities; only then may refreshes use the extension opcode.
		if (bVRDiscoveryRequested) return;
		bVRDiscoveryRequested = true;
		FACEBinaryWriter Discovery; Discovery.WriteUInt32(0x45554341); Discovery.WriteUInt32(1);
		SendGameAction(ACEGameAction::PingRequest, Discovery.GetData(), ACEQueue::UIQueue);
		return;
	}
	FACEBinaryWriter W; W.WriteUInt32(1); W.WriteUInt32(0);
	SendGameAction(0xF7D0, W.GetData(), ACEQueue::WeenieQueue);
}

void FACESession::RequestVRSpellProfile(int32 Spell)
{
	if (State != EACESessionState::InWorld || !(VRCapabilities & 256u) || Spell <= 0 || VRSpellProfiles.Contains(Spell)) return;
	FACEBinaryWriter W; W.WriteUInt32(1); W.WriteUInt32(5); W.WriteUInt32(Spell);
	if (VRCapabilities & 512u) W.WriteUInt32(1); // opt in; older VR clients still receive 12 bytes
	SendGameAction(0xF7D0, W.GetData(), ACEQueue::WeenieQueue);
}

bool FACESession::GetVRSpellProfile(int32 Spell, float& Speed, bool& Gravity, float* Radius) const
{
	const auto* Profile = VRSpellProfiles.Find(Spell);
	if (!Profile) return false;
	Speed = Profile->X; Gravity = Profile->Y != 0; if (Radius) *Radius = Profile->Z; return true;
}

bool FACESession::SendVRCombat(uint32 Kind, uint32 Cell, int32 Weapon, int32 Subject, int32 Target,
	const FVector& OriginAc, const FVector& DirectionOrEndAc, float Amount, float Duration, float RequestedPower)
{
	if (Kind == 3 && IsMissileMovementBlocked()) return false;
	if (State != EACESessionState::InWorld || !SupportsVRCombat() || Kind < 1 || Kind > 3
		|| (Weapon == 0 && (Kind != 2 || !SupportsVRUnarmed() || Subject < 0 || Subject > 1))
		|| OriginAc.ContainsNaN() || DirectionOrEndAc.ContainsNaN() || !FMath::IsFinite(Amount) || !FMath::IsFinite(Duration)
		|| !FMath::IsFinite(RequestedPower) || RequestedPower < -1.f || RequestedPower > 1.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE VR action rejected locally: kind=%u state=%d capabilities=0x%X weapon=0x%08X origin=%s vector=%s"),
			Kind, int32(State), VRCapabilities, Weapon, *OriginAc.ToString(), *DirectionOrEndAc.ToString());
		return false;
	}
	FACEBinaryWriter W;
	W.WriteUInt32(1); W.WriteUInt32(Kind); W.WriteUInt32(++VRSequence); W.WriteUInt32(Cell);
	W.WriteUInt32(TeleportSeq); W.WriteUInt32(Weapon); W.WriteUInt32(Subject); W.WriteUInt32(Target);
	for (const FVector& V : {OriginAc, DirectionOrEndAc})
	{ W.WriteFloat(V.X); W.WriteFloat(V.Y); W.WriteFloat(V.Z); }
	W.WriteFloat(FMath::Clamp(Amount, 0.f, 1.f)); W.WriteFloat(FMath::Clamp(Duration, 0.f, 2.f));
	if (Kind==2 && (VRCapabilities & 16384u))
	{ W.WriteFloat(VRMeleeBody.X); W.WriteFloat(VRMeleeBody.Y); W.WriteFloat(VRMeleeBody.Z); }
	// Keep physical draw/swing samples separate from the retail slider. Older
	// servers receive their original packet size and retain their gesture rules.
	if ((Kind==2 || Kind==3) && RequestedPower>=0.f && SupportsVRCombatPower()) W.WriteFloat(RequestedPower);
	if (Kind==1 && SupportsVRCasting())
	{
		VRAimCastSequence=VRSequence; VRAimSpell=Subject; VRAimWeapon=Weapon;
		VRCastEpoch=TeleportSeq; VRAimUntil=FPlatformTime::Seconds()+3.; VRNextAim=0.;
	}
	// Casting must report actual collision contact, not claim a midair player
	// has landed. The controller refreshes this after resolving each movement step.
	FlushAutonomousPosition(bAutoPosContact, true);
	SendGameAction(0xF7D0, W.GetData(), ACEQueue::WeenieQueue);
	return true;
}

void FACESession::HandleHealthFeedback(FACEBinaryReader& R)
{
    if (State != EACESessionState::InWorld || !SupportsHealthFeedback() || R.Remaining() != 12) return;
    const int32 Guid = R.ReadInt32(), Change = R.ReadInt32();
    const uint32 Flags = R.ReadUInt32();
    if (!Guid || !Change || Change == MIN_int32 || (Flags & ~3u) != 0) return;
	if (!IsNearbyHealthObject(Guid)) return;
    // A stale packet after portal transition must not create a phantom target.
    const auto* Object = WorldObjects.Find(Guid);
    if (Guid != PlayerGuid && (!Object || (Object->ItemType & ACEItemType::Creature) == 0)) return;
    OnHealthFeedback.Broadcast(Guid, Change, Flags);
}
