#include "ACEWorldPresenterComponent.h"
#include "ACEProfiling.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEDatSubsystem.h"
#include "ACETerrainPresenterComponent.h"
#include "ACEWorldEntityActor.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEScriptComponent.h"
#include "ACEOpcodes.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformTime.h"

namespace
{
	// Necklace, both bracelets, and both rings are stat-only equipment in retail.
	constexpr uint32 NonVisualJewelryMask = 0x000F8000u;

	bool IsNonVisualJewelry(const FACEWorldObject& Object)
	{
		return (static_cast<uint32>(Object.CurrentWieldedLocation) & NonVisualJewelryMask) != 0;
	}

	/** PlayScript body-buff FX (attrib/skill/health/regen/shield/enchant/vitae/trans). */
	bool IsBodySpellPlayScript(int32 ScriptType)
	{
		return (ScriptType >= 0x06 && ScriptType <= 0x50)
			|| (ScriptType >= 0x8B && ScriptType <= 0x8F && ScriptType != 0x8D)
			|| (ScriptType >= 0x92 && ScriptType <= 0x94);
	}

	/** HiddenAdmin needs /adminvision, not merely Admin/Sentinel. Cloaked still uses Admin bit. */
	bool ShouldSkipAdminOnlyMesh(const FACEWorldObject& Object, bool bViewerAdmin)
	{
		if (Object.bIsSelf)
		{
			return false;
		}
		if (Object.IsHiddenAdmin())
		{
			return true;
		}
		// SpellProjectile::ProjectileImpact cloaks the body before sending Explode.
		// Its emitter host must survive until ObjectDelete; the body stays NoDraw.
		return !bViewerAdmin && Object.IsCloaked() && !(Object.PhysicsState & ACEPhysicsState::Missile);
	}
}

UACEWorldPresenterComponent::UACEWorldPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	EntityClass = AACEWorldEntityActor::StaticClass();
}

void UACEWorldPresenterComponent::BeginPlay()
{
	Super::BeginPlay();
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			Client = GI->GetSubsystem<UACEClientSubsystem>();
			if (Client)
			{
				Client->OnObjectCreated.AddDynamic(this, &UACEWorldPresenterComponent::HandleObjectCreated);
				Client->OnObjectDeleted.AddDynamic(this, &UACEWorldPresenterComponent::HandleObjectDeleted);
				Client->OnPositionUpdate.AddDynamic(this, &UACEWorldPresenterComponent::HandlePositionUpdate);
				Client->OnMotionUpdate.AddDynamic(this, &UACEWorldPresenterComponent::HandleMotionUpdate);
				Client->OnVectorUpdate.AddDynamic(this, &UACEWorldPresenterComponent::HandleVectorUpdate);
				Client->OnPhysicsStateUpdate.AddDynamic(this, &UACEWorldPresenterComponent::HandlePhysicsStateUpdate);
				Client->OnPkStatusUpdated.AddDynamic(this, &UACEWorldPresenterComponent::HandlePkStatusUpdated);
				Client->OnPlayScriptId.AddDynamic(this, &UACEWorldPresenterComponent::HandlePlayScriptId);
				Client->OnPlayEffect.AddDynamic(this, &UACEWorldPresenterComponent::HandlePlayEffect);
				Client->OnSound.AddDynamic(this, &UACEWorldPresenterComponent::HandleSound);
				Client->OnSessionStateChanged.AddDynamic(this, &UACEWorldPresenterComponent::HandleLogout);
				Client->OnEnteredWorld.AddDynamic(this, &UACEWorldPresenterComponent::HandleEnteredWorld);
			}
		}
	}
}

void UACEWorldPresenterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Client)
	{
		Client->OnObjectCreated.RemoveAll(this);
		Client->OnObjectDeleted.RemoveAll(this);
		Client->OnPositionUpdate.RemoveAll(this);
		Client->OnMotionUpdate.RemoveAll(this);
		Client->OnVectorUpdate.RemoveAll(this);
		Client->OnPhysicsStateUpdate.RemoveAll(this);
		Client->OnPkStatusUpdated.RemoveAll(this);
		Client->OnPlayScriptId.RemoveAll(this);
		Client->OnPlayEffect.RemoveAll(this);
		Client->OnSound.RemoveAll(this);
		Client->OnSessionStateChanged.RemoveAll(this);
		Client->OnEnteredWorld.RemoveAll(this);
	}
	ClearSpawned();
	Super::EndPlay(EndPlayReason);
}

void UACEWorldPresenterComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	ACE_PROFILE_SCOPE(World);
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    ExpireProjectileVisuals(FPlatformTime::Seconds());

	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			{
				if (!Dat->IsWorldStreamingAllowed())
				{
					return;
				}
			}
		}
	}

	// PlayerCreate may arrive after ObjectCreate already spawned a world copy of self.
	if (bSkipSelf && Client)
	{
		DestroySpawnedSelf(Client->GetPlayerGuid());
	}
	DrainPendingSpawns();
	DrainPendingAttachments();
	DrainPendingDatAppearance();

	static int32 WearPurgeStagger = 0;
	if ((++WearPurgeStagger % 24) == 0)
	{
		TArray<int32> ToDestroy;
		for (const auto& Pair : Spawned)
		{
			AACEWorldEntityActor* Actor = Pair.Value.Get();
			if (Actor && ((Actor->GetParentGuid() != 0 && Actor->ParentLocation == 0) ||
				(static_cast<uint32>(Actor->CurrentWieldedLocation) & NonVisualJewelryMask) != 0))
			{
				ToDestroy.Add(Pair.Key);
			}
		}
		for (int32 Guid : ToDestroy)
		{
			if (TObjectPtr<AACEWorldEntityActor>* Found = Spawned.Find(Guid))
			{
				if (*Found)
				{
					(*Found)->Destroy();
				}
				Spawned.Remove(Guid);
			}
			PendingAttachments.Remove(Guid);
			PendingDatAppearance.Remove(Guid);
		}
	}

	static double LastPerfLog = 0.0;
	const double Now = FPlatformTime::Seconds();
	if (Now - LastPerfLog >= 5.0)
	{
		LastPerfLog = Now;
		int32 ScriptTicks = 0;
		int32 ActorTicks = 0;
		for (const auto& Pair : Spawned)
		{
			if (AACEWorldEntityActor* A = Pair.Value.Get())
			{
				if (A->IsActorTickEnabled())
				{
					++ActorTicks;
				}
				if (UACEScriptComponent* S = A->FindComponentByClass<UACEScriptComponent>())
				{
					if (S->IsComponentTickEnabled())
					{
						++ScriptTicks;
					}
				}
			}
		}
		UE_LOG(LogTemp, Verbose,
			TEXT("ACE perf: spawned=%d tickingActors=%d tickingScripts=%d pendingSpawn=%d dt=%.1fms"),
			Spawned.Num(), ActorTicks, ScriptTicks, PendingSpawns.Num(), DeltaTime * 1000.f);
	}
}

AACEWorldEntityActor* UACEWorldPresenterComponent::FindEntityActor(int32 Guid) const
{
	const auto* Entry = Spawned.Find(Guid);
	auto* Actor = Entry ? Entry->Get() : nullptr;
	return IsValid(Actor) && !Actor->IsActorBeingDestroyed() ? Actor : nullptr;
}

void UACEWorldPresenterComponent::RefreshCellVisibility()
{
	const auto* Terrain = GetOwner() ? GetOwner()->FindComponentByClass<UACETerrainPresenterComponent>() : nullptr;
	if (!Terrain) return;
	for (const auto& Pair : Spawned)
	{
		auto* Actor = Pair.Value.Get();
		if (!Actor) continue;
		const auto* Occupant = Actor;
		for (int32 Depth=0; Occupant->GetParentGuid()!=0 && Depth<8; ++Depth)
		{
			const auto* Parent = Spawned.FindRef(Occupant->GetParentGuid()).Get();
			if (!Parent) break;
			Occupant = Parent;
		}
		const bool bSelfAttachment = Client && Client->GetPlayerGuid()!=0
			&& Occupant->GetParentGuid()==Client->GetPlayerGuid();
		bool bVisible = bSelfAttachment || Terrain->IsWorldCellVisible(Occupant->GetLastAceCellId());
		if (!bVisible)
		{
			FBox Bounds(ForceInit);
			if (Occupant->Appearance) Occupant->Appearance->GetVisualWorldBounds(Bounds);
			if (!Bounds.IsValid) Bounds = FBox::BuildAABB(Occupant->GetActorLocation(), FVector(1));
			bVisible = Terrain->IsWorldObjectVisible(Occupant->GetLastAceCellId(), Bounds);
		}
		Actor->SetCellVisible(bVisible);
		// Static occupants can arrive before their room mesh. Recheck lighting on
		// admission, including held items whose own position has no cell.
		if (bVisible && Actor->Appearance)
			Actor->Appearance->UpdateCellLighting(bSelfAttachment ? Client->GetPlayerPosition().CellId : Occupant->GetLastAceCellId());
		// Camera/portal culling only controls presentation. A radar or fellowship
		// selection must survive turning away from the target's room.
	}
}

void UACEWorldPresenterComponent::HandlePkStatusUpdated(int32 ObjectGuid, int32 Status)
{
	if (auto* Actor = Spawned.FindRef(ObjectGuid).Get())
	{
		Actor->ObjectDescriptionFlags = ACEPlayerKillerStatus::UpdateDescriptionFlags(Actor->ObjectDescriptionFlags, Status);
		Actor->ApplyPhysicsState(Actor->PhysicsState);
	}
}

void UACEWorldPresenterComponent::ExpireProjectileVisuals(double Now)
{
    TArray<int32,TInlineAllocator<16>> Expired;
    for (const auto& Pair:ProjectileFlights)
        if (!Pair.Value.bExpired && Now>=Pair.Value.ExpiresAt) Expired.Add(Pair.Key);
    for (int32 Guid:Expired)
    {
        auto Flight=ProjectileFlights[Guid];
        HandleObjectDeleted(Guid); // Stops actor components and clears all queued effects.
        Flight.bExpired=true;
        // Keep a tombstone until the server deletes this object. A keep-ring
        // refresh or late vector packet must not resurrect the same projectile.
        ProjectileFlights.Add(Guid,Flight);
    }
}

void UACEWorldPresenterComponent::ClearSpawned()
{
    ProjectileFlights.Reset();
	PendingSpawns.Reset();
	PendingDatAppearance.Reset();
	PendingAttachments.Reset();
	AppearanceRetryCounts.Reset();
	LatestMotion.Reset();
	PendingEffects.Reset();
	PendingScriptIds.Reset();
	PendingSounds.Reset();
	for (auto& Pair : Spawned)
	{
		if (Pair.Value)
		{
			Pair.Value->Destroy();
		}
	}
	Spawned.Reset();
	LastEntityKeepLbs.Reset();
}

namespace
{
	uint32 AceLandblockKey(int32 CellId)
	{
		return static_cast<uint32>(CellId) & 0xFFFF0000u;
	}

	bool IsMissileLike(const FACEWorldObject& Object)
	{
		return (Object.PhysicsState & ACEPhysicsState::Missile) != 0;
	}
}

bool UACEWorldPresenterComponent::ShouldStreamObject(const FACEWorldObject& Object) const
{
    if (const auto* Flight=ProjectileFlights.Find(Object.Guid); Flight && Flight->bExpired) return false;
	if (ShouldSkipSelf(Object) || Object.bIsSelf)
	{
		return true;
	}
	const int32 PlayerGuid = Client ? Client->GetPlayerGuid() : 0;
	if (PlayerGuid != 0 && (Object.Guid == PlayerGuid || Object.ParentGuid == PlayerGuid
		|| Object.WielderId == PlayerGuid))
	{
		return true;
	}
	if (Object.ContainerId != 0 && Object.ParentGuid == 0 && !Object.bHasPosition)
	{
		return false;
	}
	if (IsMissileLike(Object))
	{
		return true;
	}
	if (Object.ParentGuid != 0)
	{
		if (PlayerGuid != 0 && Object.ParentGuid == PlayerGuid)
		{
			return true;
		}
		// The parent's ObjectCreate can be queued for rendering when its weapon
		// arrives. Stream against the parent's network pose, not actor existence.
		FACEWorldObject Parent;
		return !Client || !Client->GetWorldObject(Object.ParentGuid, Parent)
			|| Parent.ParentGuid != 0 || ShouldStreamObject(Parent);
	}
	if (!Object.bHasPosition)
	{
		return false;
	}
	if (AActor* Owner = GetOwner())
	{
		if (const UACETerrainPresenterComponent* Terrain = Owner->FindComponentByClass<UACETerrainPresenterComponent>())
		{
			// Retail CObjectMaint draws every object the server sent for loaded landblocks.
			// Waiting on the Unreal landblock *actor* hid NPCs/mobs until you walked onto
			// their block (staged radius 0). Keep-ring matches LScape residency.
			return Terrain->IsLandblockKept(AceLandblockKey(Object.Position.CellId));
		}
	}
	return true;
}

void UACEWorldPresenterComponent::DestroySpawnedGuid(int32 Guid)
{
	if (Client && Client->GetSelectedObject().Guid == Guid) Client->SelectObject(0);
	TArray<int32> Children;
	for (const auto& Pair : Spawned)
	{
		AACEWorldEntityActor* Child = Pair.Value.Get();
		if (Child && Pair.Key != Guid && Child->GetParentGuid() == Guid)
		{
			Children.Add(Pair.Key);
		}
	}
	HandleObjectDeleted(Guid);
	for (int32 ChildGuid : Children)
	{
		HandleObjectDeleted(ChildGuid);
	}
}

void UACEWorldPresenterComponent::SyncEntitiesToKeepRing(const TSet<int32>& OutdoorKeepLbs)
{
    TSet<int32> KeepLbs = OutdoorKeepLbs;
    if (Client && OutdoorKeepLbs.Num() > 0)
    {
        const FACEPosition Position = Client->GetPlayerPosition();
        if (Position.IsValid()) KeepLbs.Add(static_cast<int32>(AceLandblockKey(Position.CellId)));
    }
	if (LastEntityKeepLbs.Num() == KeepLbs.Num())
	{
		bool bSame = true;
		for (int32 Key : KeepLbs)
		{
			if (!LastEntityKeepLbs.Contains(Key))
			{
				bSame = false;
				break;
			}
		}
		if (bSame)
		{
			return;
		}
	}
	LastEntityKeepLbs = KeepLbs;

	TArray<int32> ToDestroy;
	for (const auto& Pair : Spawned)
	{
		AACEWorldEntityActor* Actor = Pair.Value.Get();
		if (!Actor)
		{
			ToDestroy.Add(Pair.Key);
			continue;
		}
		if (Client && Pair.Key == Client->GetPlayerGuid())
		{
			continue;
		}
		if (Actor->IsWieldedWorldItem() && Client
			&& (Actor->GetParentGuid() == Client->GetPlayerGuid()
				|| Actor->GetWielderId() == Client->GetPlayerGuid()))
		{
			continue;
		}
		const bool bMissile = (Actor->PhysicsState & ACEPhysicsState::Missile) != 0;
		if (bMissile)
		{
			continue;
		}
		int32 CellId = Actor->GetLastAceCellId();
		if (CellId == 0 && Client)
		{
			FACEWorldObject Obj;
			if (Client->GetWorldObject(Pair.Key, Obj) && Obj.bHasPosition)
			{
				CellId = Obj.Position.CellId;
			}
		}
		if (CellId == 0)
		{
			continue;
		}
		if (!KeepLbs.Contains(static_cast<int32>(AceLandblockKey(CellId))))
		{
			ToDestroy.Add(Pair.Key);
		}
	}
	for (int32 Guid : ToDestroy)
	{
		if (AACEWorldEntityActor* Actor = Spawned.FindRef(Guid))
		{
			UE_LOG(LogTemp, Log, TEXT("ACE: WorldPresenter unload weenie guid=0x%08X cell=0x%08X (left keep ring)"),
				Guid, Actor->GetLastAceCellId());
		}
		DestroySpawnedGuid(Guid);
	}

	PendingSpawns.RemoveAll([this](const FACEWorldObject& O)
	{
		return !ShouldStreamObject(O);
	});

	int32 Respawned = 0;
	if (KeepLbs.Num() > 0 && Client)
	{
		if (TSharedPtr<FACESession> Session = Client->GetSession())
		{
			for (const TPair<int32, FACEWorldObject>& Pair : Session->GetWorldObjects())
			{
				const FACEWorldObject& Obj = Pair.Value;
				if (Obj.ParentGuid == 0 && (!Obj.bHasPosition || Obj.ContainerId != 0))
				{
					continue;
				}
				if (!ShouldStreamObject(Obj) || ShouldSkipSelf(Obj))
				{
					continue;
				}
				if (Spawned.Contains(Obj.Guid))
				{
					continue;
				}
				bool bPending = false;
				for (const FACEWorldObject& Pending : PendingSpawns)
				{
					if (Pending.Guid == Obj.Guid)
					{
						bPending = true;
						break;
					}
				}
				if (bPending)
				{
					continue;
				}
				HandleObjectCreated(Obj);
				++Respawned;
			}
		}
	}
	if (ToDestroy.Num() > 0 || Respawned > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("ACE: WorldPresenter keep-ring cull destroyed=%d respawnQueued=%d keep=%d spawned=%d"),
			ToDestroy.Num(), Respawned, KeepLbs.Num(), Spawned.Num());
	}
}

void UACEWorldPresenterComponent::HandleLogout(EACESessionState NewState)
{
	if (NewState == EACESessionState::Disconnected
		|| NewState == EACESessionState::Failed
		|| NewState == EACESessionState::CharacterSelect)
	{
		ClearSpawned();
	}
}

void UACEWorldPresenterComponent::HandleObjectCreated(const FACEWorldObject& Object)
{
    const bool bFlying=IsMissileLike(Object) && Object.ParentGuid==0 && Object.ContainerId==0 && Object.WielderId==0;
    if (bFlying)
    {
        // ACE expires missiles after 30s. Allow the normal 5s impact tail, then
        // remove orphaned visuals even if interest changes hid ObjectDelete.
        if (!ProjectileFlights.Contains(Object.Guid))
            ProjectileFlights.Add(Object.Guid,{FPlatformTime::Seconds()+35.,false});
    }
    else ProjectileFlights.Remove(Object.Guid);
	if (ShouldSkipSelf(Object))
	{
		// ObjectCreate can arrive before PlayerCreate marks bIsSelf — still purge if we
		// already spawned a world copy of the local player.
		DestroySpawnedSelf(Object.Guid);
		return;
	}
	// Worn clothing/armor/cloak: server may send Physics Parent with ParentLocation.None.
	// Those are ObjDesc-only — never spawn a separate mesh (duplicate torso at the wearer's feet).
	if (IsNonVisualJewelry(Object) || (Object.ParentGuid != 0 && Object.ParentLocation == 0))
	{
		if (TObjectPtr<AACEWorldEntityActor>* Existing = Spawned.Find(Object.Guid))
		{
			if (*Existing)
			{
				(*Existing)->Destroy();
			}
			Spawned.Remove(Object.Guid);
		}
		PendingSpawns.RemoveAll([&Object](const FACEWorldObject& O) { return O.Guid == Object.Guid; });
		PendingDatAppearance.Remove(Object.Guid);
		PendingAttachments.Remove(Object.Guid);
		return;
	}
	// HiddenAdmin / destination diamonds: retail draws these only with /adminvision, not
	// merely because the viewer is Admin/Sentinel. Cloaked players still use the Admin bit.
	const bool bViewerAdmin = Client && Client->GetSession() && Client->GetSession()->IsLocalPlayerAdmin();
	if (ShouldSkipAdminOnlyMesh(Object, bViewerAdmin))
	{
		if (TObjectPtr<AACEWorldEntityActor>* Existing = Spawned.Find(Object.Guid))
		{
			if (*Existing)
			{
				(*Existing)->Destroy();
			}
			Spawned.Remove(Object.Guid);
		}
		PendingSpawns.RemoveAll([&Object](const FACEWorldObject& O) { return O.Guid == Object.Guid; });
		PendingDatAppearance.Remove(Object.Guid);
		PendingAttachments.Remove(Object.Guid);
		return;
	}
	// Contained inventory items are UI-only — never spawn in the 3D world.
	// Require !bHasPosition so a false inventory ContainerId cannot erase world props
	// (lifestone/chest) that still have a landblock pose.
	if (Object.ContainerId != 0 && Object.ParentGuid == 0 && !Object.bHasPosition)
	{
		if (TObjectPtr<AACEWorldEntityActor>* Existing = Spawned.Find(Object.Guid))
		{
			if (*Existing)
			{
				(*Existing)->Destroy();
			}
			Spawned.Remove(Object.Guid);
		}
		PendingSpawns.RemoveAll([&Object](const FACEWorldObject& O) { return O.Guid == Object.Guid; });
		return;
	}
	// Equipped items (Parent set) may omit a usable world position — still spawn them.
	if (bRequirePosition && !Object.bHasPosition && Object.ParentGuid == 0)
	{
		return;
	}
	if (!ShouldStreamObject(Object))
	{
		return;
	}

	// Projectiles / missiles are short-lived — spawn immediately so they aren't stuck
	// behind the enter-world ObjectCreate budget until after ObjectDelete.
	const bool bProjectile = IsMissileLike(Object);
	if (bProjectile)
	{
		SpawnOrUpdateEntity(Object, /*bImmediateDatAppearance*/ true);
		return;
	}

	for (FACEWorldObject& Pending : PendingSpawns)
	{
		if (Pending.Guid == Object.Guid)
		{
			Pending = Object;
			return;
		}
	}
	PendingSpawns.Add(Object);
}

bool UACEWorldPresenterComponent::ShouldSkipSelf(const FACEWorldObject& Object) const
{
	if (!bSkipSelf)
	{
		return false;
	}
	if (Object.bIsSelf)
	{
		return true;
	}
	return Client && Client->GetPlayerGuid() != 0 && Object.Guid == Client->GetPlayerGuid();
}

void UACEWorldPresenterComponent::DestroySpawnedSelf(int32 PlayerGuid)
{
	if (PlayerGuid == 0)
	{
		return;
	}
	PendingSpawns.RemoveAll([PlayerGuid](const FACEWorldObject& O)
	{
		return O.Guid == PlayerGuid || O.bIsSelf;
	});
	PendingDatAppearance.Remove(PlayerGuid);
	// This check runs every frame and on repeated self descriptors. Re-home
	// children only when there is an actual duplicate to remove. Detaching the
	// legitimate pawn's equipment here recreates its physics bodies every tick.
	if (!Spawned.Contains(PlayerGuid)) return;

	// Equipped items often attach to a temporary duplicate-self actor. Re-home them onto the
	// possessed pawn before destroying that duplicate, or they orphan as a mesh blob at the feet.
	TArray<int32> ChildGuids;
	for (const auto& Pair : Spawned)
	{
		AACEWorldEntityActor* Child = Pair.Value.Get();
		if (!Child || Child->GetACEGuid() == PlayerGuid)
		{
			continue;
		}
		if (Child->GetParentGuid() == PlayerGuid)
		{
			ChildGuids.Add(Pair.Key);
			Child->ClearAttachedState();
		}
	}

	if (TObjectPtr<AACEWorldEntityActor>* Found = Spawned.Find(PlayerGuid))
	{
		if (*Found)
		{
			UE_LOG(LogTemp, Log, TEXT("ACE: destroying duplicate self world entity guid=0x%08X (%d children to re-home)"),
				PlayerGuid, ChildGuids.Num());
			(*Found)->Destroy();
		}
		Spawned.Remove(PlayerGuid);
	}

	for (int32 ChildGuid : ChildGuids)
	{
		PendingAttachments.AddUnique(ChildGuid);
		AACEWorldEntityActor* Child = Spawned.FindRef(ChildGuid);
		if (Child)
		{
			TryAttachToParent(Child, PlayerGuid, Child->ParentLocation);
		}
	}
}

void UACEWorldPresenterComponent::HandleEnteredWorld(int32 PlayerGuid, const FACEPosition& /*SpawnPos*/)
{
	DestroySpawnedSelf(PlayerGuid);
	RehomePlayerChildren(PlayerGuid);
}

bool UACEWorldPresenterComponent::IsAreaObjectsReady(const FACEPosition& AreaCenter, float RadiusAc) const
{
	const uint32 CenterLB = static_cast<uint32>(AreaCenter.CellId) & 0xFFFF0000u;
	const float RadiusSq = FMath::Square(FMath::Max(1.f, RadiusAc));
	const FVector CenterAc(AreaCenter.Location.X, AreaCenter.Location.Y, AreaCenter.Location.Z);

	auto IsNearby = [&](const FACEPosition& Pos) -> bool
	{
		const uint32 ObjLB = static_cast<uint32>(Pos.CellId) & 0xFFFF0000u;
		if (ObjLB == CenterLB)
		{
			return true;
		}
		const FVector ObjAc(Pos.Location.X, Pos.Location.Y, Pos.Location.Z);
		return FVector::DistSquared(CenterAc, ObjAc) <= RadiusSq;
	};

	// Only block enter-world on unspawned placeholders. DAT appearance drains in the background.
	for (const FACEWorldObject& Pending : PendingSpawns)
	{
		if (Pending.ParentGuid != 0 || !Pending.bHasPosition)
		{
			continue;
		}
		if (IsNearby(Pending.Position))
		{
			return false;
		}
	}

	return true;
}

bool UACEWorldPresenterComponent::IsAreaAppearancesReady(const FACEPosition& AreaCenter, float RadiusAc) const
{
	if (!IsAreaObjectsReady(AreaCenter, RadiusAc))
	{
		return false;
	}

	const uint32 CenterLB = static_cast<uint32>(AreaCenter.CellId) & 0xFFFF0000u;
	const float RadiusSq = FMath::Square(FMath::Max(1.f, RadiusAc));
	const FVector CenterAc(AreaCenter.Location.X, AreaCenter.Location.Y, AreaCenter.Location.Z);

	auto IsNearby = [&](const FACEPosition& Pos) -> bool
	{
		const uint32 ObjLB = static_cast<uint32>(Pos.CellId) & 0xFFFF0000u;
		if (ObjLB == CenterLB)
		{
			return true;
		}
		const FVector ObjAc(Pos.Location.X, Pos.Location.Y, Pos.Location.Z);
		return FVector::DistSquared(CenterAc, ObjAc) <= RadiusSq;
	};

	auto GaveUp = [this](int32 Guid) -> bool
	{
		if (const int32* Retries = AppearanceRetryCounts.Find(Guid))
		{
			return *Retries > 20;
		}
		return false;
	};

	for (int32 Guid : PendingDatAppearance)
	{
		FACEWorldObject Obj;
		if (!Client || !Client->GetWorldObject(Guid, Obj) || !Obj.bHasPosition || Obj.ParentGuid != 0)
		{
			continue;
		}
		if (IsNearby(Obj.Position) && !GaveUp(Guid))
		{
			return false;
		}
	}

	for (const auto& Pair : Spawned)
	{
		const AACEWorldEntityActor* Actor = Pair.Value.Get();
		if (!Actor || Actor->GetParentGuid() != 0)
		{
			continue;
		}
		if (Actor->bUsingDatMesh || (Actor->Appearance && Actor->Appearance->HasAppearance()))
		{
			continue;
		}
		if (GaveUp(Actor->GetACEGuid()))
		{
			continue;
		}
		FACEWorldObject Obj;
		if (!Client || !Client->GetWorldObject(Actor->GetACEGuid(), Obj) || !Obj.bHasPosition)
		{
			continue;
		}
		if (IsNearby(Obj.Position))
		{
			return false;
		}
	}
	return true;
}

void UACEWorldPresenterComponent::RehomePlayerChildren(int32 PlayerGuid)
{
	if (PlayerGuid == 0)
	{
		return;
	}
	for (const auto& Pair : Spawned)
	{
		AACEWorldEntityActor* Child = Pair.Value.Get();
		if (!Child || Child->GetParentGuid() != PlayerGuid)
		{
			continue;
		}
		// Prefer session ParentLocation — actor can go stale after ObjDesc rebuilds.
		int32 HoldLoc = Child->ParentLocation;
		if (Client)
		{
			FACEWorldObject Obj;
			if (Client->GetWorldObject(Child->GetACEGuid(), Obj) && Obj.ParentLocation != 0)
			{
				HoldLoc = Obj.ParentLocation;
				Child->ParentGuid = Obj.ParentGuid != 0 ? Obj.ParentGuid : PlayerGuid;
				Child->ParentLocation = HoldLoc;
			}
		}
		if (HoldLoc == 0)
		{
			continue;
		}
		if (!Child->IsAttachedToParent())
		{
			Child->ClearAttachedState();
			if (!TryAttachToParent(Child, PlayerGuid, HoldLoc))
			{
				PendingAttachments.AddUnique(Child->GetACEGuid());
			}
		}
		else
		{
			// Refresh hold frame now that parent bind poses may exist.
			TryAttachToParent(Child, PlayerGuid, HoldLoc);
		}
	}

	// ObjDesc / equip can leave held gear in session without a spawned actor — ensure meshes.
	if (Client)
	{
		for (const FACEWorldObject& Obj : Client->GetWorldObjects())
		{
			if (Obj.ParentGuid != PlayerGuid || Obj.ParentLocation == 0)
			{
				continue;
			}
			if (Spawned.Contains(Obj.Guid))
			{
				continue;
			}
			HandleObjectCreated(Obj);
		}
	}
}

bool UACEWorldPresenterComponent::TryAttachToParent(AACEWorldEntityActor* Child, int32 ParentGuid, int32 ParentLocation)
{
	if (!Child || ParentGuid == 0)
	{
		return false;
	}

	AActor* ParentActor = nullptr;
	if (TObjectPtr<AACEWorldEntityActor>* Found = Spawned.Find(ParentGuid))
	{
		ParentActor = Found->Get();
	}

	// Local player is skipped from Spawned (bSkipSelf) — attach to the possessed pawn.
	if (!ParentActor && Client && ParentGuid == Client->GetPlayerGuid())
	{
		if (UWorld* World = GetWorld())
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				ParentActor = PC->GetPawn();
			}
		}
	}

	// Last resort: any pawn currently possessed (covers PlayerGuid not ready yet).
	if (!ParentActor)
	{
		if (UWorld* World = GetWorld())
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				if (APawn* Pawn = PC->GetPawn())
				{
					if (Client && (Client->GetPlayerGuid() == 0 || ParentGuid == Client->GetPlayerGuid()))
					{
						ParentActor = Pawn;
					}
				}
			}
		}
	}

	if (!ParentActor)
	{
		return false;
	}

	// ParentLocation.None means not a held child — refuse rather than inventing RightHand.
	if (ParentLocation == 0)
	{
		return false;
	}
	Child->AttachToParentActor(ParentActor, ParentLocation);
	if (Child->IsAttachedToParent() && Child->ScriptComponent)
	{
		Child->ScriptComponent->NotifyAppearanceReady(false);
	}
	return Child->IsAttachedToParent();
}

void UACEWorldPresenterComponent::SpawnOrUpdateEntity(const FACEWorldObject& Object, bool bImmediateDatAppearance)
{
	if (ShouldSkipSelf(Object))
	{
		DestroySpawnedSelf(Object.Guid);
		return;
	}
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat || !Dat->IsDatReady())
	{
		for (auto& Pending : PendingSpawns)
			if (Pending.Guid == Object.Guid) { Pending = Object; return; }
		PendingSpawns.Add(Object);
		return;
	}
	const bool bViewerAdmin = Client && Client->GetSession() && Client->GetSession()->IsLocalPlayerAdmin();
	if (ShouldSkipAdminOnlyMesh(Object, bViewerAdmin))
	{
		if (TObjectPtr<AACEWorldEntityActor>* Existing = Spawned.Find(Object.Guid))
		{
			if (*Existing)
			{
				(*Existing)->Destroy();
			}
			Spawned.Remove(Object.Guid);
		}
		return;
	}
	if (IsNonVisualJewelry(Object) || (Object.ParentGuid != 0 && Object.ParentLocation == 0))
	{
		if (TObjectPtr<AACEWorldEntityActor>* Existing = Spawned.Find(Object.Guid))
		{
			if (*Existing)
			{
				(*Existing)->Destroy();
			}
			Spawned.Remove(Object.Guid);
		}
		return;
	}

	AACEWorldEntityActor* Actor = nullptr;
	const bool bExistingActor = Spawned.Contains(Object.Guid) && IsValid(Spawned[Object.Guid]);
	const int32 PrevSetupId = Spawned.Contains(Object.Guid) && Spawned[Object.Guid]
		? Spawned[Object.Guid]->SetupId
		: 0;
	if (Spawned.Contains(Object.Guid))
	{
		Actor = Spawned[Object.Guid];
		if (Actor)
		{
			Actor->PlacementTerrain = GetOwner()->FindComponentByClass<UACETerrainPresenterComponent>();
			Actor->InitializeFromObject(Object, WorldScale, /*bApplyDatAppearance*/ false);
		}
	}
	else
	{
		UWorld* World = GetWorld();
		if (!World || !EntityClass)
		{
			return;
		}

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Actor = World->SpawnActor<AACEWorldEntityActor>(EntityClass, FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return;
		}

		Actor->PlacementTerrain = GetOwner()->FindComponentByClass<UACETerrainPresenterComponent>();
		Actor->InitializeFromObject(Object, WorldScale, /*bApplyDatAppearance*/ false);
		Spawned.Add(Object.Guid, Actor);
	}

	if (!Actor)
	{
		return;
	}

	// Only a new actor needs its cached motion restored here. Replaying it on
	// every descriptor echo restarts completed attacks and queues followups twice.
	if (!bExistingActor)
	{
		if (const FACEObjectMotionState* Motion = LatestMotion.Find(Object.Guid))
		{
			Actor->ApplyMotionState(*Motion);
		}
		else if (Object.UsesOnOffMotion() && Actor->Appearance && Actor->Appearance->HasAppearance()
			&& Object.MotionTableId != 0)
		{
			const int32 Cmd = (Object.InitialMotionCommand == 0x000B || Object.InitialMotionCommand == 0x000C)
				? Object.InitialMotionCommand
				: 0x000C; // Off / closed
			Actor->Appearance->SetDoorHeldCommand(Cmd);
		}
		else if (ACEMotion::IsHeldRestCommand(Object.InitialMotionCommand) && Actor->Appearance
			&& Actor->Appearance->HasAppearance() && Object.MotionTableId != 0)
		{
			Actor->Appearance->SetHeldActionMotion(Object.InitialMotionCommand, Object.InitialMotionStyle);
		}
	}

	if (Object.ParentGuid != 0)
	{
		if (!TryAttachToParent(Actor, Object.ParentGuid, Object.ParentLocation))
		{
			PendingAttachments.AddUnique(Object.Guid);
		}
	}
	else if (Actor->IsAttachedToParent())
	{
		// PickupEvent / drop to world — detach and place freestanding.
		Actor->InitializeFromObject(Object, WorldScale, /*bApplyDatAppearance*/ false);
	}

	// Capture SetupId BEFORE InitializeFromObject — that call overwrites Actor->SetupId,
	// which previously skipped rebuilds on UpdateObject hook morphs (Font of Jojii) and
	// left ScriptComponent with StopAllEffects + bDefaultStarted=false forever.
	// PrevSetupId was sampled at the top of this function, before InitializeFromObject.
	const bool bSetupChanged = PrevSetupId != 0 && PrevSetupId != Object.SetupId;
	// A fresh server description starts a new attempt, including a replacement
	// object using the same GUID after an earlier setup failed.
	AppearanceRetryCounts.Remove(Object.Guid);

	if (bImmediateDatAppearance && bApplyDatAppearance && Dat && Dat->IsDatReady())
	{
		const uint64 Revision = Actor->Appearance ? Actor->Appearance->GetAppearanceRevision() : 0;
		if (Actor->ApplyDatAppearanceFromObject(Object))
		{
			PendingDatAppearance.Remove(Object.Guid);
		}
		else
		{
			PendingDatAppearance.AddUnique(Object.Guid);
		}
		if (bSetupChanged && Actor->ScriptComponent && !Actor->ScriptComponent->HasStartedDefaultScripts())
		{
			Actor->ScriptComponent->NotifyAppearanceReady(Object.UsesOnOffMotion());
		}
		if (Actor->Appearance && Actor->Appearance->GetAppearanceRevision() != Revision)
		{
			// Appearance rebuild resets DoorTransition — re-apply latest On/Off or held pose.
			if (const FACEObjectMotionState* Motion = LatestMotion.Find(Object.Guid))
			{
				if (!bExistingActor || bSetupChanged || Revision == 0) Actor->ApplyMotionState(*Motion);
			}
			else if (Object.UsesOnOffMotion() && Actor->Appearance && Actor->Appearance->HasAppearance()
				&& Object.MotionTableId != 0)
			{
				const int32 Cmd = (Object.InitialMotionCommand == 0x000B || Object.InitialMotionCommand == 0x000C)
					? Object.InitialMotionCommand
					: 0x000C;
				Actor->Appearance->SetDoorHeldCommand(Cmd);
			}
			else if (ACEMotion::IsHeldRestCommand(Object.InitialMotionCommand) && Actor->Appearance
				&& Actor->Appearance->HasAppearance() && Object.MotionTableId != 0)
			{
				Actor->Appearance->SetHeldActionMotion(Object.InitialMotionCommand, Object.InitialMotionStyle);
			}
		}
	}
	else if (bApplyDatAppearance)
	{
		// ApplyWorldObject owns the complete cache key (setup, ObjDesc, placement,
		// translucency and collision). Even an existing mesh needs that check:
		// UpdateObject/ObjDesc may change its parts without changing the setup.
		PendingDatAppearance.AddUnique(Object.Guid);
	}

	FlushPendingEffectsFor(Object.Guid);
}

void UACEWorldPresenterComponent::DrainPendingSpawns()
{
	// A body created before its Setup metadata is resident gets no movement
	// spheres and cannot settle later, even after its visual mesh appears.
	auto* Dat = GetWorld() && GetWorld()->GetGameInstance()
		? GetWorld()->GetGameInstance()->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat || !Dat->IsDatReady()) return;
	const int32 Budget = FMath::Max(1, SpawnsPerTick);
	int32 SpawnedCount = 0;
	const int32 EffectiveBudget = Dat->IsLightweightStreaming() ? 1 : Budget;
	while (PendingSpawns.Num() > 0 && SpawnedCount < EffectiveBudget)
	{
		if (SpawnedCount == 0 && PendingSpawns.Num() > 1 && Client)
		{
			const FACEPosition PlayerPos = Client->GetPlayerPosition();
			if (PlayerPos.IsValid())
			{
				const FVector PlayerUe = PlayerPos.ToUnrealLocation(WorldScale);
				PendingSpawns.Sort([&](const FACEWorldObject& A, const FACEWorldObject& B)
				{
					auto DistSq = [&](const FACEWorldObject& O) -> float
					{
						if (!O.bHasPosition)
						{
							return TNumericLimits<float>::Max();
						}
						return static_cast<float>(FVector::DistSquared(
							O.Position.ToUnrealLocation(WorldScale), PlayerUe));
					};
					return DistSq(A) < DistSq(B);
				});
			}
		}
		const FACEWorldObject Object = PendingSpawns[0];
		PendingSpawns.RemoveAt(0, 1, EAllowShrinking::No);
		if (ShouldSkipSelf(Object))
		{
			DestroySpawnedSelf(Object.Guid);
			continue;
		}
		if (!ShouldStreamObject(Object))
		{
			continue;
		}
		SpawnOrUpdateEntity(Object, /*bImmediateDatAppearance*/ false);
		++SpawnedCount;
	}
}

void UACEWorldPresenterComponent::DrainPendingAttachments()
{
	if (PendingAttachments.Num() == 0 || !Client)
	{
		return;
	}

	TArray<int32> StillPending;
	for (int32 Guid : PendingAttachments)
	{
		AACEWorldEntityActor* Child = Spawned.FindRef(Guid);
		if (!Child)
		{
			continue;
		}
		FACEWorldObject Obj;
		if (!Client->GetWorldObject(Guid, Obj) || Obj.ParentGuid == 0)
		{
			continue;
		}
		if (!TryAttachToParent(Child, Obj.ParentGuid, Obj.ParentLocation))
		{
			StillPending.Add(Guid);
		}
	}
	PendingAttachments = MoveTemp(StillPending);
}

void UACEWorldPresenterComponent::BeginPostPortalAppearanceBoost(int32 ExtraPerTick, float Seconds)
{
	AppearanceBoostExtra = FMath::Max(0, ExtraPerTick);
	AppearanceBoostUntil = FPlatformTime::Seconds() + FMath::Max(0.1f, Seconds);
}

void UACEWorldPresenterComponent::DrainPendingDatAppearance()
{
	if (!bApplyDatAppearance || DatAppearancesPerTick <= 0 || !Client || PendingDatAppearance.IsEmpty())
	{
		return;
	}

	const int32 Budget = DatAppearancesPerTick;
	int32 EffectiveAppearanceBudget = Budget;
	bool bPortalVisible = false;
	if (AppearanceBoostExtra > 0 && FPlatformTime::Seconds() < AppearanceBoostUntil)
	{
		EffectiveAppearanceBudget += AppearanceBoostExtra;
	}
	else
	{
		AppearanceBoostExtra = 0;
	}
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			{
				// EnsureLoaded starts an asynchronous index. Pending work must survive
				// that wait without spending its finite malformed-asset retry budget.
				if (!Dat->IsDatReady())
				{
					Dat->BeginBackgroundLoad();
					return;
				}
				bPortalVisible = Dat->IsInPortalSpace();
				if (Dat->IsLightweightStreaming())
				{
					// Skip expensive DAT mesh builds while the portal tunnel is on-camera.
					EffectiveAppearanceBudget = 0;
				}
			}
		}
	}
	if (EffectiveAppearanceBudget <= 0)
	{
		return;
	}
	if (bPortalVisible) EffectiveAppearanceBudget = FMath::Min(EffectiveAppearanceBudget, 2);
	const double Deadline = FPlatformTime::Seconds() + (bPortalVisible ? .002 : .004);

	int32 Done = 0;
	if (PendingDatAppearance.Num() > 1)
	{
		// Snapshot each key once: the comparator used to copy entire object
		// descriptions (clothing, properties, etc.) several times per comparison.
		TMap<int32, TPair<int32, float>> SortKeys;
		const FACEPosition PlayerPos = Client ? Client->GetPlayerPosition() : FACEPosition();
		const FVector PlayerLocation = PlayerPos.ToUnrealLocation(WorldScale);
		for (int32 Guid : PendingDatAppearance)
		{
			FACEWorldObject Obj;
			const bool bFound = Client && Client->GetWorldObject(Guid, Obj);
			const int32 Rank = !bFound ? 2 : (Obj.bIsPlayer || (Obj.ItemType & ACEItemType::Creature) || Obj.IsVendor()) ? 0 : 1;
			const float Distance = !bFound || !Obj.bHasPosition ? TNumericLimits<float>::Max()
				: !PlayerPos.IsValid() ? 0.f : static_cast<float>(FVector::DistSquared(Obj.Position.ToUnrealLocation(WorldScale), PlayerLocation));
			SortKeys.Add(Guid, TPair<int32, float>(Rank, Distance));
		}
		PendingDatAppearance.StableSort([&SortKeys](int32 A, int32 B)
		{
			const auto& Ka = SortKeys.FindChecked(A);
			const auto& Kb = SortKeys.FindChecked(B);
			return Ka.Key != Kb.Key ? Ka.Key < Kb.Key : Ka.Value < Kb.Value;
		});
	}
	// A failed GUID is requeued for another frame, not retried repeatedly in
	// this drain. Otherwise one missing setup can consume the entire budget.
	int32 Remaining = PendingDatAppearance.Num();
	while (PendingDatAppearance.Num() > 0 && Remaining-- > 0 && Done < EffectiveAppearanceBudget
		&& (Done == 0 || FPlatformTime::Seconds() < Deadline))
	{
		const int32 Guid = PendingDatAppearance[0];
		PendingDatAppearance.RemoveAt(0, 1, EAllowShrinking::No);

		AACEWorldEntityActor* Actor = Spawned.FindRef(Guid);
		FACEWorldObject Obj;
		const bool bHaveObj = Client && Client->GetWorldObject(Guid, Obj);
		if (!Actor)
		{
			continue;
		}
		if (!bHaveObj)
		{
			continue;
		}

		// Use the live descriptor, including changes received after this was queued.
		const bool RestoreMotion = !Actor->Appearance || !Actor->Appearance->HasAppearance()
			|| Actor->Appearance->GetSetupId() != Obj.SetupId;
		if (!Actor->ApplyDatAppearanceFromObject(Obj))
		{
			int32& Retries = AppearanceRetryCounts.FindOrAdd(Guid);
			++Retries;
			if (Retries <= 20)
			{
				if (UWorld* World = GetWorld())
				{
					if (UGameInstance* GI = World->GetGameInstance())
					{
						if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
						{
							if (!Dat->IsDatReady())
							{
								Dat->BeginBackgroundLoad();
							}
						}
					}
				}
				PendingDatAppearance.Add(Guid);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("ACE: gave up DAT appearance for guid=0x%08X '%s' setup=0x%08X after %d tries"),
					Guid, *Obj.Name, Obj.SetupId, Retries);
				if (Actor->ScriptComponent)
				{
					Actor->ScriptComponent->NotifyAppearanceReady(Obj.UsesOnOffMotion());
					FlushPendingEffectsFor(Guid);
				}
			}
			++Done;
			continue;
		}
		AppearanceRetryCounts.Remove(Guid);

		// ApplyWorldObject resets DoorTransition — restore server On/Off resting pose.
		if (const FACEObjectMotionState* Motion = LatestMotion.Find(Guid))
		{
			if (RestoreMotion) Actor->ApplyMotionState(*Motion);
		}
		else if (Obj.UsesOnOffMotion() && Actor->Appearance && Obj.MotionTableId != 0)
		{
			const int32 Cmd = (Obj.InitialMotionCommand == 0x000B || Obj.InitialMotionCommand == 0x000C)
				? Obj.InitialMotionCommand
				: 0x000C;
			Actor->Appearance->SetDoorHeldCommand(Cmd);
		}
		else if (ACEMotion::IsHeldRestCommand(Obj.InitialMotionCommand) && Actor->Appearance
			&& Obj.MotionTableId != 0)
		{
			Actor->Appearance->SetHeldActionMotion(Obj.InitialMotionCommand, Obj.InitialMotionStyle);
		}

		if (Obj.ParentGuid != 0 && !Actor->IsAttachedToParent())
		{
			PendingAttachments.AddUnique(Guid);
		}
		else if (Obj.ParentGuid != 0 && Actor->IsAttachedToParent())
		{
			// Re-snap after mesh build so bind-frame scale doesn't leave the weapon at world origin.
			TryAttachToParent(Actor, Obj.ParentGuid, Obj.ParentLocation);
		}
		FlushPendingEffectsFor(Guid);
		++Done;
	}
}

void UACEWorldPresenterComponent::HandleObjectDeleted(int32 ObjectGuid)
{
    ProjectileFlights.Remove(ObjectGuid);
	PendingSpawns.RemoveAll([ObjectGuid](const FACEWorldObject& O) { return O.Guid == ObjectGuid; });
	PendingDatAppearance.Remove(ObjectGuid);
	AppearanceRetryCounts.Remove(ObjectGuid);
	PendingAttachments.Remove(ObjectGuid);
	PendingEffects.RemoveAll([ObjectGuid](const FPendingPlayEffect& E) { return E.ObjectGuid == ObjectGuid; });
	PendingScriptIds.RemoveAll([ObjectGuid](const FPendingPlayScriptId& E) { return E.ObjectGuid == ObjectGuid; });
	PendingSounds.RemoveAll([ObjectGuid](const FPendingSound& E) { return E.ObjectGuid == ObjectGuid; });
	LatestMotion.Remove(ObjectGuid);

	if (TObjectPtr<AACEWorldEntityActor>* Found = Spawned.Find(ObjectGuid))
	{
		if (*Found)
		{
			(*Found)->Destroy();
		}
		Spawned.Remove(ObjectGuid);
	}
}

void UACEWorldPresenterComponent::HandlePositionUpdate(int32 ObjectGuid, const FACEPosition& Position)
{
	if (TObjectPtr<AACEWorldEntityActor>* Found = Spawned.Find(ObjectGuid))
	{
		if (*Found)
		{
			FACEWorldObject Live;
			if (Client && Client->GetWorldObject(ObjectGuid, Live))
			{
				Live.Position = Position;
				Live.bHasPosition = true;
				if (!ShouldStreamObject(Live))
				{
					DestroySpawnedGuid(ObjectGuid);
					return;
				}
				if (Live.ParentGuid == 0 && ((*Found)->IsAttachedToParent() || (*Found)->GetParentGuid() != 0))
					(*Found)->InitializeFromObject(Live, WorldScale, /*bApplyDatAppearance*/ false);
				(*Found)->ApplyPositionPlacement(Live.PlacementId);
			}
			(*Found)->ApplyACEPosition(Position);
		}
		return;
	}

	for (FACEWorldObject& Pending : PendingSpawns)
	{
		if (Pending.Guid == ObjectGuid)
		{
			// Queue entries may still contain an inventory/held placement from
			// before the drop. Carry the accepted descriptor through spawn as well.
			FACEWorldObject Live;
			if (Client && Client->GetWorldObject(ObjectGuid, Live)) Pending = MoveTemp(Live);
			Pending.Position = Position;
			Pending.bHasPosition = true;
			return;
		}
	}

	if (!Client || !bRequirePosition)
	{
		return;
	}

	FACEWorldObject Obj;
	if (Client->GetWorldObject(ObjectGuid, Obj))
	{
		if (ShouldSkipSelf(Obj))
		{
			return;
		}
		// Inventory / packed items must not reappear as freestanding world meshes.
		if (Obj.ContainerId != 0)
		{
			return;
		}
		Obj.Position = Position;
		Obj.bHasPosition = true;
		if (!ShouldStreamObject(Obj))
		{
			return;
		}
		HandleObjectCreated(Obj);
	}
}

void UACEWorldPresenterComponent::HandleMotionUpdate(int32 ObjectGuid, const FACEObjectMotionState& Motion)
{
	LatestMotion.Add(ObjectGuid, Motion);
	if (TObjectPtr<AACEWorldEntityActor>* Found = Spawned.Find(ObjectGuid))
	{
		if (*Found)
		{
			(*Found)->ApplyMotionState(Motion);
		}
	}
}

void UACEWorldPresenterComponent::HandleVectorUpdate(int32 ObjectGuid, FVector AceVelocity, FVector AceOmega)
{
	if (AACEWorldEntityActor* Actor = Spawned.FindRef(ObjectGuid))
	{
		Actor->ApplyPhysicsVelocity(AceVelocity, AceOmega);
		return;
	}
	for (FACEWorldObject& Pending : PendingSpawns)
	{
		if (Pending.Guid == ObjectGuid)
		{
			Pending.Velocity = AceVelocity;
			Pending.Omega = AceOmega;
			Pending.bHasVelocity = !AceVelocity.IsNearlyZero();
			return;
		}
	}
	if (Client)
	{
		FACEWorldObject Obj;
		if (Client->GetWorldObject(ObjectGuid, Obj))
		{
			Obj.Velocity = AceVelocity;
			Obj.Omega = AceOmega;
			Obj.bHasVelocity = !AceVelocity.IsNearlyZero();
			HandleObjectCreated(Obj);
		}
	}
}

void UACEWorldPresenterComponent::HandlePhysicsStateUpdate(int32 ObjectGuid, int32 PhysicsState)
{
    if (!(PhysicsState & ACEPhysicsState::Missile)) ProjectileFlights.Remove(ObjectGuid);
	const bool bViewerAdmin = Client && Client->GetSession() && Client->GetSession()->IsLocalPlayerAdmin();
	const bool bCloaked = (PhysicsState & ACEPhysicsState::Cloaked) != 0;
	const bool bProjectile = (PhysicsState & ACEPhysicsState::Missile) != 0
		|| (Spawned.FindRef(ObjectGuid) && (Spawned.FindRef(ObjectGuid)->PhysicsState & ACEPhysicsState::Missile));
	if (!bViewerAdmin && bCloaked && !bProjectile)
	{
		PendingSpawns.RemoveAll([ObjectGuid](const FACEWorldObject& Object) { return Object.Guid == ObjectGuid; });
		PendingDatAppearance.Remove(ObjectGuid);
		PendingAttachments.Remove(ObjectGuid);
		if (TObjectPtr<AACEWorldEntityActor>* Found = Spawned.Find(ObjectGuid))
		{
			if (AACEWorldEntityActor* Actor = Found->Get())
			{
				if (Actor->ScriptComponent)
				{
					Actor->ScriptComponent->StopAllEffects();
					Actor->ScriptComponent->StopAllSounds();
				}
				Actor->Destroy();
			}
			Spawned.Remove(ObjectGuid);
		}
		return;
	}
	if (TObjectPtr<AACEWorldEntityActor>* Found = Spawned.Find(ObjectGuid))
	{
		if (*Found)
		{
			(*Found)->ApplyPhysicsState(PhysicsState);
		}
	}
	else if (Client && (!bCloaked || bProjectile))
	{
		// A cloak update can remove the render actor while the network object
		// remains known. Uncloak does not require another ObjectCreate packet.
		FACEWorldObject Object;
		if (Client->GetWorldObject(ObjectGuid, Object))
		{
			Object.PhysicsState = PhysicsState;
			HandleObjectCreated(Object);
		}
	}
}

void UACEWorldPresenterComponent::HandlePlayScriptId(int32 ObjectGuid, int32 PhysicsScriptId, float Intensity)
{
	if (!TryPlayScriptIdOnActor(ObjectGuid, PhysicsScriptId, Intensity))
	{
		FPendingPlayScriptId Pending;
		Pending.ArrivalOrder = NextEffectArrivalOrder++;
		Pending.ObjectGuid = ObjectGuid;
		Pending.PhysicsScriptId = PhysicsScriptId;
		Pending.Intensity = Intensity;
		PendingScriptIds.Add(Pending);
	}
}

void UACEWorldPresenterComponent::HandlePlayEffect(int32 ObjectGuid, int32 ScriptType, float Intensity)
{
	if (!TryPlayEffectOnActor(ObjectGuid, ScriptType, Intensity))
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("ACE FX: PlayEffect queued guid=0x%08X type=0x%X intensity=%.3f (no ready ScriptComponent)"),
			ObjectGuid, ScriptType, Intensity);
		FPendingPlayEffect Pending;
		Pending.ArrivalOrder = NextEffectArrivalOrder++;
		Pending.ObjectGuid = ObjectGuid;
		Pending.ScriptType = ScriptType;
		Pending.Intensity = Intensity;
		PendingEffects.Add(Pending);
	}
}

void UACEWorldPresenterComponent::HandleSound(int32 ObjectGuid, int32 SoundType, float Volume)
{
	UE_LOG(LogTemp, Log, TEXT("ACE Sound: opcode guid=0x%08X type=%d vol=%.2f"),
		ObjectGuid, SoundType, Volume);
	if (!TryPlaySoundOnActor(ObjectGuid, SoundType, Volume))
	{
		FPendingSound Pending;
		Pending.ArrivalOrder = NextEffectArrivalOrder++;
		Pending.ObjectGuid = ObjectGuid;
		Pending.SoundType = SoundType;
		Pending.Volume = Volume;
		PendingSounds.Add(Pending);
	}
}

UACEScriptComponent* UACEWorldPresenterComponent::FindLocalPawnScriptComponent() const
{
	if (const UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				return Pawn->FindComponentByClass<UACEScriptComponent>();
			}
		}
	}
	return nullptr;
}

UACEScriptComponent* UACEWorldPresenterComponent::FindScriptComponentForGuid(int32 ObjectGuid) const
{
	if (ObjectGuid == 0)
	{
		return nullptr;
	}
	// Prefer the possessed pawn for the local player BEFORE any Spawned world actor.
	// A stale duplicate-self WorldEntity (ObjectCreate before PlayerCreate) would otherwise
	// steal TargetEffect buffs onto a hidden/misplaced actor — cast anim+sound still work
	// (motion on the pawn) while particles never appear.
	if (Client && Client->GetPlayerGuid() != 0 && ObjectGuid == Client->GetPlayerGuid())
	{
		if (UACEScriptComponent* Local = FindLocalPawnScriptComponent())
		{
			return Local;
		}
	}

	// Retail CPhysicsObj::play_script runs on the item PhysicsObj. CreateParticle partIdx
	// is relative to the weapon Setup; the wand is already parented to the hand. Routing
	// those hooks onto the wielder mapped part 0 onto a body joint (glow at torso/feet).
	if (AACEWorldEntityActor* Actor = Spawned.FindRef(ObjectGuid))
	{
		if (Actor->ScriptComponent)
		{
			return Actor->ScriptComponent;
		}
	}

	return nullptr;
}

bool UACEWorldPresenterComponent::TryPlayEffectOnActor(int32 ObjectGuid, int32 ScriptType, float Intensity)
{
	// Retail resolves the addressed PhysicsObj's script table. An item caster is
	// not permission to replay its effect on the wielder or the local player.
	UACEScriptComponent* Scripts = FindScriptComponentForGuid(ObjectGuid);
	// Blood needs the impact frame, not a completed mesh upload. Deferring it
	// can discard a killing blow when ObjectDelete clears the pending queue.
	const bool Blood = ScriptType >= 0x5B && ScriptType <= 0x66;
	if (Scripts && !Scripts->IsEffectReady() && !Blood) return false;
	if (Scripts) Scripts->PlayImpactEffect(ScriptType, Intensity);
	return Scripts != nullptr;
}

bool UACEWorldPresenterComponent::TryPlayScriptIdOnActor(int32 ObjectGuid, int32 PhysicsScriptId, float Intensity)
{
	if (UACEScriptComponent* Scripts = FindScriptComponentForGuid(ObjectGuid))
	{
		if (!Scripts->IsEffectReady())
		{
			return false;
		}
		Scripts->PlayScriptId(PhysicsScriptId, Intensity);
		return true;
	}
	return false;
}

bool UACEWorldPresenterComponent::TryPlaySoundOnActor(int32 ObjectGuid, int32 SoundType, float Volume)
{
	if (UACEScriptComponent* Scripts = FindScriptComponentForGuid(ObjectGuid))
	{
		if (!Scripts->IsEffectReady())
		{
			return false;
		}
		if (AACEWorldEntityActor* Ent = Spawned.FindRef(ObjectGuid))
		{
			Scripts->PlaySoundAtWorldLocation(SoundType, Volume, Ent->GetSoundEmitLocation());
		}
		else
		{
			Scripts->PlaySound(SoundType, Volume);
		}
		return true;
	}
	return false;
}

void UACEWorldPresenterComponent::FlushPendingEffectsForGuid(int32 ObjectGuid)
{
	FlushPendingEffectsFor(ObjectGuid);
}

void UACEWorldPresenterComponent::DropPendingOneShotEffectsForGuid(int32 ObjectGuid)
{
	if (ObjectGuid == 0)
	{
		return;
	}
	for (int32 i = PendingEffects.Num() - 1; i >= 0; --i)
	{
		if (PendingEffects[i].ObjectGuid != ObjectGuid)
		{
			continue;
		}
		const int32 ScriptType = PendingEffects[i].ScriptType;
		if (IsBodySpellPlayScript(ScriptType) || ScriptType == 0x04)
		{
			PendingEffects.RemoveAt(i, 1, EAllowShrinking::No);
		}
	}
}

void UACEWorldPresenterComponent::FlushPendingEffectsFor(int32 ObjectGuid)
{
	// SmartBox queues the original net messages until the PhysicsObj is ready.
	// Preserve their order across both script opcodes (and sounds); the final
	// numbered emitter must be the one the server requested last.
	for (;;)
	{
		const int32 E=PendingEffects.IndexOfByPredicate([ObjectGuid](const auto& P){return P.ObjectGuid==ObjectGuid;});
		const int32 S=PendingScriptIds.IndexOfByPredicate([ObjectGuid](const auto& P){return P.ObjectGuid==ObjectGuid;});
		const int32 A=PendingSounds.IndexOfByPredicate([ObjectGuid](const auto& P){return P.ObjectGuid==ObjectGuid;});
		const uint64 EO=E==INDEX_NONE?MAX_uint64:PendingEffects[E].ArrivalOrder;
		const uint64 SO=S==INDEX_NONE?MAX_uint64:PendingScriptIds[S].ArrivalOrder;
		const uint64 AO=A==INDEX_NONE?MAX_uint64:PendingSounds[A].ArrivalOrder;
		if (EO==MAX_uint64 && SO==MAX_uint64 && AO==MAX_uint64) break;
		if (EO<=SO && EO<=AO)
		{
			if (!TryPlayEffectOnActor(ObjectGuid,PendingEffects[E].ScriptType,PendingEffects[E].Intensity)) break;
			PendingEffects.RemoveAt(E,1,EAllowShrinking::No);
		}
		else if (SO<=AO)
		{
			if (!TryPlayScriptIdOnActor(ObjectGuid,PendingScriptIds[S].PhysicsScriptId,PendingScriptIds[S].Intensity)) break;
			PendingScriptIds.RemoveAt(S,1,EAllowShrinking::No);
		}
		else
		{
			if (!TryPlaySoundOnActor(ObjectGuid,PendingSounds[A].SoundType,PendingSounds[A].Volume)) break;
			PendingSounds.RemoveAt(A,1,EAllowShrinking::No);
		}
	}

	// Wand innate TargetEffects are keyed by item guid. When the local pawn becomes ready,
	// also flush any pending FX for items we currently wield/parent.
	if (Client && Client->GetPlayerGuid() != 0 && ObjectGuid == Client->GetPlayerGuid())
	{
		TArray<int32> ItemGuids;
		for (const FPendingPlayEffect& Pending : PendingEffects)
		{
			if (Pending.ObjectGuid == ObjectGuid)
			{
				continue;
			}
			FACEWorldObject Obj;
			if (!Client->GetWorldObject(Pending.ObjectGuid, Obj))
			{
				continue;
			}
			const int32 Host = Obj.WielderId != 0 ? Obj.WielderId : Obj.ParentGuid;
			if (Host == ObjectGuid)
			{
				ItemGuids.AddUnique(Pending.ObjectGuid);
			}
		}
		for (const int32 ItemGuid : ItemGuids)
		{
			FlushPendingEffectsFor(ItemGuid);
		}
	}
}
