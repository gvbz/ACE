#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEAutoCraft.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEInventoryRules.h"
#include "ACEEquipmentRules.h"
#include "ACEScrollLearning.h"
#include "ACEVendorPricing.h"
#include "ACEVTObjectClass.h"
#include "ACEVTLootPalette.h"
#include "ACEPetDevice.h"
#include "ACEPluginEquipment.h"
#include "ACESession.h"
#include "ACEOpcodes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Mods/ACEPluginSight.h"
#include "Mods/ACEPluginRouteGround.h"
#include "Mods/ACEPluginCombat.h"
#include "UI/ACERetailObjectNames.h"
#include "UI/ACEUIResourceResolver.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
    double N(const TSharedPtr<FJsonObject>& O,const TCHAR* K,double Default=0){double V;return O&&O->TryGetNumberField(K,V)&&FMath::IsFinite(V)?V:Default;}
    FString S(const TSharedPtr<FJsonObject>& O,const TCHAR* K){FString V;if(O)O->TryGetStringField(K,V);return V;}
    bool B(const TSharedPtr<FJsonObject>& O,const TCHAR* K,bool Default=false){bool V;return O&&O->TryGetBoolField(K,V)?V:Default;}
    FString CorpseOwnerName(FString Name)
    {
        // ACE strips administrator markers from Creature_Death's LongDesc to
        // match retail/VT corpse ownership. Character/object names retain them.
        Name.TrimStartAndEndInline();
        while(Name.RemoveFromStart(TEXT("+"))){}
        return Name;
    }
    float BodyHeight(UACEDatSubsystem* Dat,const FACEWorldObject& Obj)
    {
        float Step=0,Height=0,Radius=0;uint32 Anim=0;
        if(!Dat || !Dat->TryGetSetupPhysics(Obj.SetupId,Step,Height,Radius,Anim))Height=Obj.IsCorpse()?.4f:1.8f;
        return FMath::Max(1.f,Height*Obj.GetValidObjectScale()*100.f);
    }
    FVector SightPoint(UACEDatSubsystem* Dat,const FACEWorldObject& Obj,const FACEPluginSightQuery& Sight)
    {
        return Sight.Location(Obj)+FVector(0,0,FMath::Max(15.f,BodyHeight(Dat,Obj)*.6f));
    }
    bool CombatSight(UACEClientSubsystem* Client,UACEDatSubsystem* Dat,const FACEPosition& Player,
        const FACEWorldObject& Target,const FACEPluginSightQuery& Sight)
    {
        FACEWorldObject Caster;Client->GetWorldObject(Client->GetPlayerGuid(),Caster);
        const float SourceHeight=BodyHeight(Dat,Caster),TargetHeight=BodyHeight(Dat,Target);
        // ACE's targeted bolts aim at 2/3 of the scaled physics height. Do not
        // aim from the camera, a fixed eye height, or a rat's stale packet pose.
        const FVector Start=Player.ToUnrealLocation()+FVector(0,0,SourceHeight*2.f/3.f);
        const FVector End=Sight.Location(Target)+FVector(0,0,TargetHeight*2.f/3.f);
        // UCM safety margin, not a claim to know server-only projectile templates.
        // Avoid grazing corners; keep the corridor above the floor for tiny bodies.
        const float Clearance=FMath::Min3(15.f,SourceHeight/3.f,TargetHeight/3.f);
        return Sight.Clear(Start,End,Clearance);
    }
    TSharedPtr<FJsonObject> Pos(const FACEPosition& P)
    {
        auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("cell"),uint32(P.CellId));
        O->SetNumberField(TEXT("x"),P.Location.X);O->SetNumberField(TEXT("y"),P.Location.Y);O->SetNumberField(TEXT("z"),P.Location.Z);
        return O;
    }
    bool LootItemAvailable(UACEClientSubsystem* Client,FACEWorldObject Item,bool AllowRetained=false)
    {
        if(!Client->IsOwnedInventoryItem(Item)||(!AllowRetained&&(Item.ObjectDescriptionFlags&ACEObjectDescFlag::Retained)))return false;
        for(int Depth=0;Depth<32;++Depth)
        {
            if(Item.WielderId||Item.ParentGuid||Item.CurrentWieldedLocation||Client->GetSession()->GetTradeSelfItems().Contains(Item.Guid))return false;
            if(Item.ContainerId==Client->GetPlayerGuid())return true;
            if(!Client->GetWorldObject(Item.ContainerId,Item))return false;
        }
        return false;
    }
    bool SalvageToolAvailable(UACEClientSubsystem* Client,const FACEWorldObject& Item)
    {
        // Retained protects the tool from disposal, not from being used. WCID
        // identifies the standard Ust even when a server omits its item category.
        return (Item.WeenieClassId==ACEWeenieClass::TinkeringTool||ACEVTObjectClass::Classify(Item)==40)
            && LootItemAvailable(Client,Item,true);
    }
}
bool UACEPluginSubsystem::IsOwnedPluginItem(const FACEWorldObject& Item) const
{
    const auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const int32 Player=C->GetPlayerGuid();
    // IsOwnedInventoryItem intentionally means unequipped pack contents (for
    // dragging/merging). Automation must also retain the item after wielding it.
    return Player && (C->IsOwnedInventoryItem(Item) || Item.WielderId==Player
        || (Item.CurrentWieldedLocation && Item.ParentGuid==Player));
}
void UACEPluginSubsystem::ObserveAppraisal(const FACEAppraisalInfo& Info)
{
    // OnAppraisalObserved runs before the client consumes the background-request
    // marker. Inventory maintenance (including salvage scans) is not a manual
    // TestItem request. Keep caching the assessment, but do not announce it.
    auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    FACEWorldObject Item;
    const bool InventoryScan=Client&&Client->BackgroundAppraisals.Contains(Info.ObjectGuid)
        &&Client->GetWorldObject(Info.ObjectGuid,Item)&&IsOwnedPluginItem(Item);
    if(Info.bSuccess&&!InventoryScan)QueueLootMatch(Info.ObjectGuid);
    TargetAppraisalTimes.Add(Info.ObjectGuid,FPlatformTime::Seconds());
    Appraisals.Add(Info.ObjectGuid,Info);DirtyInventoryAppraisals.Add(Info.ObjectGuid);++DataRevision;
    if(Client&&Client->GetWorldObject(Info.ObjectGuid,Item))RememberEquipmentAppraisal(Item,Info);
    for(const auto& P:Plugins)if(P->WaitAction==TEXT("identify")&&P->WaitItem==Info.ObjectGuid)
    {
        P->NextAction=FMath::Min(P->NextAction,P->ActionSentAt+.25);
        P->NextDecision=0;P->WaitAction.Empty();
        if(P->IsPolicy()&&P->Running&&Info.ObjectGuid==CombatTarget)
        {P->NextAction=0;ScheduleDecisionWake(FPlatformTime::Seconds());}
    }
    // A slow reply can arrive after RefreshActionWait expired the ID wait.
    // It still makes the current opponent ready; do not wait for the idle poll.
    // Never release a newer cast, equipment operation or other action's gate.
    if(auto P=PolicyPlugin();P&&P->Running&&Info.ObjectGuid==CombatTarget
        &&P->WaitAction.IsEmpty()&&!PendingSpell&&P->NextAction<=FPlatformTime::Seconds())
    {P->NextDecision=0;ScheduleDecisionWake(FPlatformTime::Seconds());}
}
void UACEPluginSubsystem::UnbindCombatEvents()
{
    if(auto S=ObservedSession.Pin())
    {
        S->OnObjectHealth.Remove(TargetHealthHandle);S->OnMotionUpdate.Remove(TargetMotionHandle);
        S->OnObjectDeleted.Remove(TargetDeletedHandle);S->OnObjectCreated.Remove(TargetCreatedHandle);
        S->OnVitalsUpdated.Remove(CombatVitalsHandle);
        S->OnPositionUpdate.Remove(FollowPositionHandle);
    }
}
void UACEPluginSubsystem::WakeCombatDecision()
{
    auto P=PolicyPlugin();if(!P||!P->Running)return;
    // Physical attack's retry throttle is not a server action lock. Release
    // only that delay; casts, inventory transfers and equipment still wait.
    if(PhysicalAttackOwner==P->Id&&P->WaitAction.IsEmpty()&&!PendingSpell
        &&(P->NextAction==PhysicalAttackNextSendAt||P->NextAction<=FPlatformTime::Seconds()))P->NextAction=0;
    P->NextDecision=0;ScheduleDecisionWake(FPlatformTime::Seconds());
}
void UACEPluginSubsystem::ReleaseCombatTarget(int32 Guid)
{
    if(!Guid)return;
    auto P=PolicyPlugin();
    const bool DeferredTarget=P&&P->CombatTransitionIntent
        &&(S(P->CombatTransitionIntent,TEXT("action"))==TEXT("attack")||S(P->CombatTransitionIntent,TEXT("action"))==TEXT("cast"))
        &&int32(uint32(N(P->CombatTransitionIntent,TEXT("target"))))==Guid;
    if(CombatTarget!=Guid&&PhysicalAttackTarget!=Guid&&!DeferredTarget)return;
    // A server charge is not a pending Use. Otherwise Tick stays behind the
    // approach gate even after the attack throttle was released on death.
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))PC->EndCombatApproachTo(Guid);
    if(DeferredTarget)
    {
        // This request has not been sent yet. Let policy choose recovery or a
        // living target; PrepareCombatAction still honors real attack/stance ACKs.
        P->CombatTransitionIntent.Reset();P->NextAction=0;
    }
    WakeCombatDecision();if(CombatTarget==Guid)CombatTarget=0;
    if(P&&P->WaitAction==TEXT("identify")&&P->WaitItem==Guid)
    {P->WaitAction.Empty();P->NextAction=0;}
    if(PhysicalAttackTarget==Guid){PhysicalAttackTarget=0;PhysicalAttackOwner.Empty();PhysicalAttackNextSendAt=0;}
    // Do not clear PendingSpell: its untagged UseDone still belongs to that
    // cast. A death event can arrive while a projectile or recoil is active.
}
void UACEPluginSubsystem::RecordCombatDeath(int32 Guid)
{
    auto P=PolicyPlugin();
    if(!P||!P->Running)return;
    const bool DeferredTarget=P->CombatTransitionIntent
        &&(S(P->CombatTransitionIntent,TEXT("action"))==TEXT("attack")||S(P->CombatTransitionIntent,TEXT("action"))==TEXT("cast"))
        &&int32(uint32(N(P->CombatTransitionIntent,TEXT("target"))))==Guid;
    if(CombatTarget!=Guid&&PhysicalAttackTarget!=Guid&&!DeferredTarget)return;
    if(LastCombatDeath&&int32(uint32(N(LastCombatDeath,TEXT("id"))))==Guid)return;
    auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Object;
    if(!Client->GetWorldObject(Guid,Object)||!Object.bHasPosition||Object.bIsPlayer||Object.PetOwnerId)return;
    // A zero-health/death-motion notification precedes the corpse ObjectCreate.
    // Retain this across deletion, but never infer death from lost visibility.
    LastCombatDeath=Pos(Object.Position);
    LastCombatDeath->SetNumberField(TEXT("id"),uint32(Guid));
    LastCombatDeath->SetNumberField(TEXT("time"),FPlatformTime::Seconds());
    LastCombatDeath->SetNumberField(TEXT("teleport_sequence"),Client->GetTeleportSeq());
}
void UACEPluginSubsystem::ObserveTargetHealth(int32 Guid,float Fraction)
{
    if(!FMath::IsFinite(Fraction)||Fraction<0||Fraction>1)return;
    TargetHealthSamples.Add(Guid,{Fraction,FPlatformTime::Seconds()});
    FACEWorldObject Object;auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(Fraction>0)
    {
        DefeatedTargets.Remove(Guid);
        if(LastCombatDeath&&int32(uint32(N(LastCombatDeath,TEXT("id"))))==Guid)LastCombatDeath.Reset();
        return;
    }
    if(C->GetWorldObject(Guid,Object)&&Object.IsAttackable()&&!Object.bIsPlayer&&!Object.PetOwnerId)
    {RecordCombatDeath(Guid);DefeatedTargets.Add(Guid);ReleaseCombatTarget(Guid);}
}
void UACEPluginSubsystem::ObserveTargetMotion(int32 Guid,const FACEObjectMotionState& Motion)
{
    FACEWorldObject Object;
    if(Motion.IsDeathMotion()&&GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->GetWorldObject(Guid,Object)&&Object.bDying)
        {RecordCombatDeath(Guid);ReleaseCombatTarget(Guid);}
}
void UACEPluginSubsystem::ObserveTargetDeleted(int32 Guid)
{
    // PickupEvent also removes a held visual on every missile shot and on
    // unequip. The retained owned stack/item has not been deleted or changed.
    FACEWorldObject Item;
    if(GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->GetWorldObject(Guid,Item)&&IsOwnedPluginItem(Item))return;
    if(EquipmentAssessments.Remove(Guid))EquipmentAssessmentsDirty=true;
    for(const auto& P:Plugins)if(P->Running&&P->WaitConsumedItems.Contains(Guid)
        &&(P->WaitAction==TEXT("salvage")||P->WaitAction==TEXT("combine_salvage")))
        RefreshActionWait(*P);
    ClearPluginTarget(Guid);CorpseFirstSeen.Remove(Guid);DefeatedTargets.Remove(Guid);ReleaseCombatTarget(Guid);ForgetTargetEffects(Guid);
}
void UACEPluginSubsystem::ObserveTargetCreated(const FACEWorldObject& Object)
{
    // Wield/parent and appearance notifications reuse OnObjectCreated to refresh
    // meshes. They do not replace an owned item's assessed qualities. Dropping
    // those here made every weapon swap/reload require another appraisal.
    if((Object.bAppearanceOnlyUpdate||Object.bAttachmentOnlyUpdate)&&IsOwnedPluginItem(Object))return;
    ClearPluginTarget(Object.Guid);
    AppraisalRequests.Remove(Object.Guid);
    DirtyInventoryAppraisals.Add(Object.Guid);++DataRevision;
    ForgetTargetEffects(Object.Guid);
    DefeatedTargets.Remove(Object.Guid);
    if(LastCombatDeath&&int32(uint32(N(LastCombatDeath,TEXT("id"))))==Object.Guid)LastCombatDeath.Reset();
    if(Object.IsCorpse())
    {
        CorpseFirstSeen.Add(Object.Guid,FPlatformTime::Seconds());
        if(LastCombatDeath&&FPlatformTime::Seconds()-N(LastCombatDeath,TEXT("time"))<8)WakeCombatDecision();
    }
    // VT fo retains opened corpse IDs across Stop and visibility releases,
    // but a new corpse at a different position can reuse an old ID.
    if(const auto* Record=LootedCorpses.Find(Object.Guid);Record&&Object.bHasPosition)
        if(!Object.IsCorpse()||Object.Position.CellId!=Record->Position.CellId
            ||!Object.Position.Location.Equals(Record->Position.Location,.01))LootedCorpses.Remove(Object.Guid);
}
void UACEPluginSubsystem::ObserveCombatVitals(const FACEPlayerVitals& Vitals)
{
    const FIntVector Current(Vitals.Health,Vitals.Stamina,Vitals.Mana);
    if(Current==LastCombatVitals)return;
    LastCombatVitals=Current;
    if(auto P=PolicyPlugin();P&&P->Running&&B(P->Profile,TEXT("recovery"),true))
    {
        const auto Below=[](int32 Value,int32 Maximum,double Threshold){return Maximum>0&&100.0*Value/Maximum<Threshold;};
        const double HealthThreshold=FMath::Max(N(P->Profile,TEXT("health_threshold"),65),N(P->Profile,TEXT("emergency_health_threshold"),35));
        if(Below(Vitals.Health,Vitals.MaxHealth,HealthThreshold)||Below(Vitals.Stamina,Vitals.MaxStamina,N(P->Profile,TEXT("stamina_threshold"),50))
            ||Below(Vitals.Mana,Vitals.MaxMana,N(P->Profile,TEXT("mana_threshold"),45)))
        {
            // An unsent attack selected before a stance change must not bypass
            // recovery after damage arrives during that wait. Do not interrupt
            // a cast or equipment transaction already accepted by the server.
            if(P->CombatTransitionIntent&&S(P->CombatTransitionIntent,TEXT("activity"))==TEXT("combat"))
            {P->CombatTransitionIntent.Reset();P->NextAction=0;}
            WakeCombatDecision();
        }
    }
    // VT's ChangeVital wakes the scheduler. Run recovery through the ordinary
    // policy before choosing another attack; never attack from this callback.
    if(CombatTarget||!PhysicalAttackOwner.IsEmpty())WakeCombatDecision();
}
void UACEPluginSubsystem::PrepareSpellConfirmation(const FString& SpellName)
{
    PendingSpellConfirmation.Empty();PendingSpellItemConfirmations.Empty();PendingSpellRecoveryConfirmations.Empty();
    auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const int32 Player=Client->GetPlayerGuid();
    const bool Self=PendingSpellTarget==Player;
    FACEWorldObject Recipient;
    const FString Prefix=TEXT("You cast ")+SpellName+TEXT(" on ");
    if(Self||Client->GetWorldObject(PendingSpellTarget,Recipient))
        PendingSpellConfirmation=Prefix+(Self?TEXT("yourself"):Recipient.Name);

    auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    uint32 School=0,Power=0,Category=0,Flags=0,TargetType=0;double Duration=0;bool Projectile=false;
    if(!Dat->TryGetPluginSpellInfo(PendingSpell,School,Power,Category,Flags,Duration))return;
    // VT d3 also recognizes boost results, which omit "on yourself".
    // Match the exact spell/recipient and a numeric restored amount, not merely
    // a general recovery phrase from unrelated server chat.
    if(School==2&&(Flags&4)&&Duration==0&&!PendingSpellConfirmation.IsEmpty())
        for(const FString Vital:{TEXT("health"),TEXT("stamina"),TEXT("mana")})
            PendingSpellRecoveryConfirmations.Emplace(
                Self?TEXT("You cast ")+SpellName+TEXT(" and restore "):TEXT("With ")+SpellName+TEXT(" you restore "),
                Self?TEXT(" points of your ")+Vital+TEXT("."):TEXT(" points of ")+Vital+TEXT(" to ")+Recipient.Name+TEXT("."));

    // ACE redirects a self-targeted bane/impenetrability to worn vestments.
    // Its result names each item, not "yourself". VT's d3/gj/dm likewise
    // accepts the item result and retains a timer for the requested buff.
    // Snapshot only compatible worn items: a matching spell on carried armor
    // or another player's equipment must not confirm this attempt.
    if(!Self||!Player||School!=3||!(Flags&4)||Duration<=0
        ||!Dat->TryGetRetailSpellTargeting(PendingSpell,Flags,TargetType,Projectile)
        ||!(TargetType&(ACEItemType::Armor|ACEItemType::Clothing)))return;
    if(auto Session=Client->GetSession())for(const auto& Pair:Session->GetWorldObjects())
    {
        const auto& Item=Pair.Value;
        const bool Worn=Item.WielderId==Player||(Item.CurrentWieldedLocation&&Item.ParentGuid==Player);
        if(Worn&&(uint32(Item.ItemType)&TargetType)&&!Item.Name.IsEmpty())
            PendingSpellItemConfirmations.AddUnique(Prefix+Item.Name);
    }
}
void UACEPluginSubsystem::ObserveUseDone(uint32 Error)
{
    if(Error)PendingDebuffCasts.RemoveAll([this](const auto& E){const auto J=E->AsObject();return N(J,TEXT("spell"))==LastTargetEffectSpell
        &&N(J,TEXT("target"))==uint32(LastTargetEffectTarget);});
    // A confirmed buff may have already advanced the policy. Its late success
    // acknowledgment must never finish the next cast or an inventory action.
    if(ConfirmedBuffAckAt && !Error)
    {
        ConfirmedBuffAckAt=0;ConfirmedBuffOwner.Empty();
        if(BuffRetryAt)BuffRetryAt=FPlatformTime::Seconds();
        ScheduleDecisionWake(FPlatformTime::Seconds());return;
    }
    // A manually attempted use can fail while waiting for recoil. Its error
    // is not the successful buff's acknowledgment and must not release it.
    if(ConfirmedBuffAckAt&&!PendingSpell)return;
    // VT retries CastSpell while waiting for the spell words. Servers without
    // recoil queuing reject an early request as busy; retry that same request,
    // without consuming the buff family's failure limit or advancing Lua.
    if(Error==0x001D && PendingBuffResultDriven && !PendingSpellWords
        && FPlatformTime::Seconds()-PendingSpellAt<5)
    {
        BuffRetryAt=FPlatformTime::Seconds()+.2;
        ScheduleDecisionWake(BuffRetryAt);return;
    }
    CompletePendingAction(Error);
}
void UACEPluginSubsystem::CompleteConfirmedBuff()
{
    if(!PendingSpell||!PendingBuffResultDriven||!PendingSpellConfirmed||PendingSpellFizzled||ConfirmedBuffAckAt)return;
    // Words and result may arrive in one network batch. Preserve the single
    // movement edge that cancels recoil before letting the next buff start.
    if(!FastCastOwner.IsEmpty()&&FastCastStarted&&!FastCastMovementApplied)return;
    const FString Owner=PendingSpellOwner;
    CompletePendingAction(0);
    ConfirmedBuffAckAt=FPlatformTime::Seconds();ConfirmedBuffOwner=Owner;
}
void UACEPluginSubsystem::CompletePendingAction(uint32 Error)
{
    BuffRetryAt=0;PendingBuffResultDriven=false;PendingSpellWords=false;
    // UseDone releases the caster even while a launched projectile is still
    // travelling. Damage/enchantment confirmation is tracked separately.
    // Wake on the next frame like VT's SchedulePoke, rather than adding up to
    // half a second from the idle ticker. Fizzles/rejections retain backoff.
    if(PendingSpell)if(auto Owner=Find(PendingSpellOwner))
    {
        const bool Completed=!PendingSpellFizzled&&!Error;
        Owner->NextAction=FMath::Min(Owner->NextAction,Completed?FPlatformTime::Seconds():PendingSpellAt+.5);
        Owner->NextDecision=0;
        if(Owner->Running)ScheduleDecisionWake(Owner->NextAction);
    }
    PendingSpellOwner.Empty();
    FastCastOwner.Empty();
    OptionalCastOwner.Empty();
    FastCastStarted=false;
    if(Error&&PendingSpell)OffensiveCasts.RemoveAll([this](const auto& E){return E.Spell==PendingSpell&&E.Target==PendingSpellTarget;});
    if(Error&&PendingSpell)PendingDebuffCasts.RemoveAll([this](const auto& E){return N(E->AsObject(),TEXT("spell"))==PendingSpell&&N(E->AsObject(),TEXT("target"))==uint32(PendingSpellTarget);});
    LastActionError=Error;LastCompletedSpell=PendingSpell;LastSpellConfirmed=PendingSpellConfirmed&&!PendingSpellFizzled&&!Error;++ActionSerial;
    if(ObservedAction){auto Event=MakeShared<FJsonObject>(*ObservedAction);Event->SetStringField(TEXT("stage"),TEXT("completed"));Event->SetNumberField(TEXT("action_serial"),ActionSerial);Event->SetNumberField(TEXT("error"),Error);if(PendingSpell)Event->SetBoolField(TEXT("confirmed"),LastSpellConfirmed);EmitPluginAction(Event);ObservedAction.Reset();}
    if(PendingManaRefresh&&!Error)
    {
        auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
        if(auto Session=Client->GetSession())for(const auto& Pair:Session->GetWorldObjects())
            if(Pair.Value.WielderId==Client->GetPlayerGuid()
                ||(Pair.Value.CurrentWieldedLocation&&Pair.Value.ParentGuid==Client->GetPlayerGuid()))
            {
                // Keep the previous reading until the reply arrives: Lua must
                // confirm an increase, and equipment eligibility must not vanish.
                // Missing timestamps prioritize a fresh reading of the lowest
                // mana piece ahead of the general inventory scan.
                AppraisalRequests.Remove(Pair.Key);TargetAppraisalTimes.Remove(Pair.Key);
            }
    }
    PendingManaRefresh=false;
    // Stone contents and essence charges change after use as well as equipped mana.
    // Reassess them before another maintenance decision can reuse stale contents.
    for (int32 Id : PendingResourceRefresh)
    {
        auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Item;
        const bool Owned=Client->GetWorldObject(Id,Item)&&IsOwnedPluginItem(Item);
        // An innate cast spends mana, not the implement's identity/requirements.
        // Refresh its resources without making an already assessed wand unknown.
        if(!Owned||!(Item.ItemType&ACEItemType::Caster))Appraisals.Remove(Id);
        AppraisalRequests.Remove(Id);DirtyInventoryAppraisals.Add(Id);
        if(Owned)
        {
            // At most the source and target: do not put confirmation behind a
            // full inventory's periodic appraisal scan.
            LastAppraisalRequest=FPlatformTime::Seconds();AppraisalRequests.Add(Id,LastAppraisalRequest);
            Client->RequestBackgroundAppraisal(Id);
        }
    }
    if (!PendingResourceRefresh.IsEmpty()) ++DataRevision;
    PendingResourceRefresh.Empty();
    if(PendingSpell && FPlatformTime::Seconds()-PendingSpellAt<30 && LastSpellConfirmed)
    {
        uint32 School,Power,Category,Flags;double Duration;
        auto* D=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
        if(D->TryGetPluginSpellInfo(PendingSpell,School,Power,Category,Flags,Duration) && School==3 && (Flags&4) && Duration>0)
        {
            // VT MySpell.EffectiveDurationS and ACE EnchantmentManager apply
            // +20% per IncreasedSpellDuration augmentation to cast buffs.
            // Otherwise item-only banes are refreshed before their real expiry.
            auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            Duration*=1.+.2*FMath::Max(0,Client->GetPlayerVitalsView().StatQualityInts.FindRef(238));
            // Item enchantments are not in the player's enchantment list. Only
            // record confirmed casts, never assume a request was successful.
            auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("target"),uint32(PendingSpellTarget));
            E->SetNumberField(TEXT("spell"),PendingSpell);
            E->SetNumberField(TEXT("category"),Category);E->SetNumberField(TEXT("power"),Power);
            E->SetNumberField(TEXT("expires"),FPlatformTime::Seconds()+Duration);
            E->SetNumberField(TEXT("duration"),Duration);
            FACEWorldObject BuffItem;
            E->SetBoolField(TEXT("owned"),PendingSpellTarget==Client->GetPlayerGuid()
                ||(Client->GetWorldObject(PendingSpellTarget,BuffItem)&&IsOwnedPluginItem(BuffItem)));
            uint32 TargetFlags=0,TargetType=0;bool Projectile=false;
            if(PendingSpellTarget==Client->GetPlayerGuid()&&D->TryGetRetailSpellTargeting(PendingSpell,TargetFlags,TargetType,Projectile)
                &&(TargetType&(ACEItemType::Armor|ACEItemType::Clothing)))
            {
                TArray<TSharedPtr<FJsonValue>> Equipment;
                for(const auto& Pair:Client->GetSession()->GetWorldObjects())if(Pair.Value.WielderId==PendingSpellTarget&&(Pair.Value.ItemType&TargetType))
                    Equipment.Add(MakeShared<FJsonValueNumber>(uint32(Pair.Key)));
                E->SetArrayField(TEXT("equipment"),Equipment);
            }
            ItemBuffs.RemoveAll([&](const auto& Old){return N(Old->AsObject(),TEXT("target"))==uint32(PendingSpellTarget)&&N(Old->AsObject(),TEXT("category"))==Category;});
            ItemBuffs.Add(MakeShared<FJsonValueObject>(E));
            SaveItemBuffTimers();
        }
    }
    PendingSpell=PendingSpellTarget=0;
    PendingSpellItemConfirmations.Empty();
    PendingSpellRecoveryConfirmations.Empty();
}
void UACEPluginSubsystem::RecordCombatOutcome(bool Hit)
{
    auto P=Find(PhysicalAttackOwner);auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const double Now=FPlatformTime::Seconds();FACEWorldObject Target;
    if(!P||!P->Running||!PhysicalAttackTarget||Now>PhysicalAttackUntil
        ||C->GetSelectedObject().Guid!=PhysicalAttackTarget||!C->GetWorldObject(PhysicalAttackTarget,Target))return;
    AddCombatOutcome(PhysicalAttackTarget,Hit);
}
void UACEPluginSubsystem::AddCombatOutcome(int32 Target,bool Hit,bool Unhittable,int32 Spell,int32 Damage)
{
    auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("serial"),++CombatOutcomeSerial);
    E->SetNumberField(TEXT("target"),uint32(Target));E->SetBoolField(TEXT("hit"),Hit);E->SetBoolField(TEXT("unhittable"),Unhittable);
    E->SetNumberField(TEXT("time"),FPlatformTime::Seconds());
    if(Spell)E->SetNumberField(TEXT("spell"),Spell);
    if(Damage>0)E->SetNumberField(TEXT("damage"),Damage);
    CombatOutcomes.Add(MakeShared<FJsonValueObject>(E));if(CombatOutcomes.Num()>128)CombatOutcomes.RemoveAt(0);
}
void UACEPluginSubsystem::ExpireOffensiveCasts(double Now)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    for(int32 Index=OffensiveCasts.Num()-1;Index>=0;--Index)
    {
        const auto& E=OffensiveCasts[Index];auto P=Find(E.Owner);FACEWorldObject Target;
        if(!P||!P->Running||!C->GetWorldObject(E.Target,Target)||Target.Name!=E.TargetName)
        {OffensiveCasts.RemoveAt(Index);continue;}
        // VT gj uses four 907ms result ticks after spell words, not after the
        // cast request. Turning/equipping and a resisted spell are not misses.
        if(E.Started>0&&Now-E.Started>=3.628)
        {AddCombatOutcome(E.Target,false);OffensiveCasts.RemoveAt(Index);}
        else if(!E.Started&&Now-E.Sent>=5)OffensiveCasts.RemoveAt(Index);
    }
}
void UACEPluginSubsystem::ObserveOffensiveSpellChat(const FString& Text,const FString& Sender,int32 Type)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();const double Now=FPlatformTime::Seconds();
    ExpireOffensiveCasts(Now);
    if(Type==ACEChatMessageType::Spellcasting)
    {
        FACEWorldObject Player;
        if(!C->GetWorldObject(C->GetPlayerGuid(),Player)||Player.Name.IsEmpty()||Sender!=Player.Name)return;
        for(int32 Index=OffensiveCasts.Num()-1;Index>=0;--Index)
            if(OffensiveCasts[Index].Spell==PendingSpell&&!OffensiveCasts[Index].Started)
            {OffensiveCasts[Index].Started=Now;break;}
        return;
    }
    if(!Sender.IsEmpty()||Type!=ACEChatMessageType::Magic)return;
    for(int32 Index=OffensiveCasts.Num()-1;Index>=0;--Index)
    {
        const auto E=OffensiveCasts[Index];if(!E.Started)continue;
        const FString Prefix=TEXT("You cast ")+E.SpellName+TEXT(" on ")+E.TargetName;
        const bool Damage=Text.Contains(TEXT("You "))&&Text.Contains(TEXT(" ")+E.TargetName+TEXT(" for "))
            &&Text.Contains(TEXT(" points with ")+E.SpellName+TEXT("."));
        const bool Hit=Damage||Text==Prefix||Text==Prefix+TEXT(".")||Text.StartsWith(Prefix+TEXT(", refreshing "))
            ||Text.StartsWith(Prefix+TEXT(", surpassing "))||Text.StartsWith(Prefix+TEXT(", but it is surpassed by "));
        const bool Resist=Text==E.TargetName+TEXT(" resists your spell")||Text==E.TargetName+TEXT(" resists your spell.");
        const bool Fizzle=Text==TEXT("Your spell fizzled.");
        const bool Invalid=Text==TEXT("Target is out of range")||Text==TEXT("Target is out of range.")
            ||Text==E.TargetName+TEXT(" is an invalid target.")
            ||(Text.StartsWith(TEXT("You fail to affect ")+E.TargetName+TEXT(" because "))
                &&(Text.EndsWith(TEXT(" is not a player killer!"))||Text.EndsWith(TEXT("you are not a player killer!"))
                    ||Text.Contains(TEXT(" because beneficial spells do not affect "))));
        if(Hit||Resist||Fizzle||Invalid)
        {
            if(Hit)
            {
                int32 Amount=0;
                if(Damage)
                {
                    // Parse only this already-correlated target/spell result.
                    const FString Before=TEXT(" ")+E.TargetName+TEXT(" for ");
                    const int32 Match=Text.Find(Before,ESearchCase::IgnoreCase);
                    if(Match!=INDEX_NONE)
                    {
                        const int32 Start=Match+Before.Len();
                        const int32 End=Text.Find(TEXT(" points with "),ESearchCase::IgnoreCase,ESearchDir::FromStart,Start);
                        if(End>Start){const FString Digits=Text.Mid(Start,End-Start);if(Digits.Len()<=9&&Digits.IsNumeric())Amount=FMath::Max(0,FCString::Atoi(*Digits));}
                    }
                }
                AddCombatOutcome(E.Target,true,false,E.Spell,Amount);
            }
            else if(Invalid&&C->GetSelectedObject().Guid==E.Target)AddCombatOutcome(E.Target,false,true);
            OffensiveCasts.RemoveAt(Index);break;
        }
    }
}
void UACEPluginSubsystem::ObserveCombatFeedback(const FString& Name,int32 Damage,bool Incoming,bool Critical)
{
    // The retail notification contains a name rather than a GUID. Associate it
    // only with our still-selected attack target; evades are not obstruction hits.
    FACEWorldObject Target;auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(!Incoming&&Damage>0&&C->GetWorldObject(PhysicalAttackTarget,Target)&&Name==Target.Name)RecordCombatOutcome(true);
    if(!Incoming&&Damage>0)for(int32 Index=OffensiveCasts.Num()-1;Index>=0;--Index)
    {
        const auto E=OffensiveCasts[Index];auto P=Find(E.Owner);
        if(P&&P->Running&&E.Started>0&&FPlatformTime::Seconds()-E.Started<3.628
            &&Name==E.TargetName&&C->GetSelectedObject().Guid==E.Target)
        {AddCombatOutcome(E.Target,true,false,E.Spell,Damage);OffensiveCasts.RemoveAt(Index);break;}
    }
}
void UACEPluginSubsystem::RefreshAutomationData()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginAppraisalMaintenance);
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();
    if(!Session || C->GetSessionState()!=EACESessionState::InWorld)
    {
        SaveItemBuffTimers();SaveEquipmentAppraisals();EquipmentAssessments.Empty();ActionOutcomes.Empty();FellowshipTells.Reset();UCMCraft.Reset();UCMServicesProfile.Reset();FellowshipManagerStatus.Empty();
        CombatTarget=0;DefeatedTargets.Empty();LastCombatDeath.Reset();LastCombatVitals=FIntVector(-1,-1,-1);
        ConfirmedBuffAckAt=BuffRetryAt=0;ConfirmedBuffOwner.Empty();DeferredBuffIntent.Reset();DeferredBuffOwner.Empty();PendingBuffResultDriven=false;
        DataPlayer=0;Appraisals.Empty();TargetAppraisalTimes.Empty();TargetHealthSamples.Empty();CorpseFirstSeen.Empty();LootedCorpses.Empty();AppraisalRequests.Empty();ItemBuffs.Empty();CachedInventory.Empty();CachedInventoryIndices.Empty();DirtyInventoryAppraisals.Empty();ClearBuffRequests();
        PendingSpell=0;PendingSpellOwner.Empty();FastCastOwner.Empty();OptionalCastOwner.Empty();PendingManaRefresh=false;PendingResourceRefresh.Empty();Debuffs.Empty();PendingDebuffCasts.Empty();OffensiveCasts.Empty();InventoryRevision=MAX_uint64;C->BackgroundAppraisals.Empty();return;
    }
    if(DataPlayer!=C->GetPlayerGuid()||DataServer!=C->GetServerName()||ObservedSession.Pin()!=Session)
    {
        FellowshipTells.Reset();UCMCraft.Reset();UCMServicesProfile.Reset();FellowshipManagerStatus.Empty();
        SaveItemBuffTimers();SaveEquipmentAppraisals();
        UnbindCombatEvents();CombatTarget=0;DefeatedTargets.Empty();LastCombatDeath.Reset();LastCombatVitals=FIntVector(-1,-1,-1);
        ConfirmedBuffAckAt=BuffRetryAt=0;ConfirmedBuffOwner.Empty();DeferredBuffIntent.Reset();DeferredBuffOwner.Empty();PendingBuffResultDriven=false;
        if(auto Old=ObservedSession.Pin()){Old->OnItemUseRequested.Remove(ItemUseHandle);Old->OnUseDone.Remove(UseDoneHandle);Old->OnSpellCastRequested.Remove(SpellCastHandle);Old->OnPlayerTell.Remove(TellHandle);Old->OnChatMessage.Remove(MetaChatHandle);Old->OnCombatFeedback.Remove(CombatFeedbackHandle);}
        ObservedAction.Reset();ItemUseHandle=Session->OnItemUseRequested.AddUObject(this,&UACEPluginSubsystem::ObserveItemUse);
        ObservedSession=Session;UseDoneHandle=Session->OnUseDone.AddUObject(this,&UACEPluginSubsystem::ObserveUseDone);
        SpellCastHandle=Session->OnSpellCastRequested.AddUObject(this,&UACEPluginSubsystem::ObserveTargetSpellCast);
        MetaChatHandle=Session->OnChatMessage.AddUObject(this,&UACEPluginSubsystem::ObserveMetaChat);MetaChatEvents.Empty();MetaPortalEvents.Empty();MetaPortalSpace=false;
        CombatFeedbackHandle=Session->OnCombatFeedback.AddUObject(this,&UACEPluginSubsystem::ObserveCombatFeedback);
        TargetHealthHandle=Session->OnObjectHealth.AddUObject(this,&UACEPluginSubsystem::ObserveTargetHealth);
        TargetMotionHandle=Session->OnMotionUpdate.AddUObject(this,&UACEPluginSubsystem::ObserveTargetMotion);
        FollowPositionHandle=Session->OnPositionUpdate.AddUObject(this,&UACEPluginSubsystem::ObserveFollowPosition);
        TargetDeletedHandle=Session->OnObjectDeleted.AddUObject(this,&UACEPluginSubsystem::ObserveTargetDeleted);
        TargetCreatedHandle=Session->OnObjectCreated.AddUObject(this,&UACEPluginSubsystem::ObserveTargetCreated);
        CombatVitalsHandle=Session->OnVitalsUpdated.AddUObject(this,&UACEPluginSubsystem::ObserveCombatVitals);
        PhysicalAttackTarget=0;PhysicalAttackOwner.Empty();CombatOutcomes.Empty();OffensiveCasts.Empty();ActionOutcomes.Empty();
        TellHandle=Session->OnPlayerTell.AddUObject(this,&UACEPluginSubsystem::ObservePlayerTell);ClearBuffRequests();
        DataPlayer=C->GetPlayerGuid();DataServer=C->GetServerName();NextAppraisalScan=0;Appraisals.Empty();TargetAppraisalTimes.Empty();TargetHealthSamples.Empty();CachedSpeciesNames.Reset();CorpseFirstSeen.Empty();LootedCorpses.Empty();AppraisalRequests.Empty();ItemBuffs.Empty();
        LoadItemBuffTimers();LoadEquipmentAppraisals();
        CachedInventory.Empty();CachedInventoryIndices.Empty();DirtyInventoryAppraisals.Empty();
        PendingSpell=0;PendingSpellOwner.Empty();FastCastOwner.Empty();OptionalCastOwner.Empty();PendingResourceRefresh.Empty();PendingManaRefresh=false;Debuffs.Empty();PendingDebuffCasts.Empty();InventoryRevision=MAX_uint64;++DataRevision;C->BackgroundAppraisals.Empty();
    }
    ExpireOffensiveCasts(FPlatformTime::Seconds());
    if(FPlatformTime::Seconds()>=NextEquipmentAssessmentSave)SaveEquipmentAppraisals();
    const double EffectNow=FPlatformTime::Seconds();
    Debuffs.RemoveAll([&](const auto& E){return N(E->AsObject(),TEXT("expires"))<=EffectNow
        ||!Session->GetWorldObjects().Contains(int32(uint32(N(E->AsObject(),TEXT("target")))));});
    PendingDebuffCasts.RemoveAll([EffectNow](const auto& E){return EffectNow-N(E->AsObject(),TEXT("sent"))>30;});
    RefreshPlateAppraisals();
    auto UCM=PolicyPlugin();if(!UCM||!UCM->Enabled)return;
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))
    {
        const bool Portal=PC->IsWorldTransitionActive();
        if(Portal!=MetaPortalSpace){MetaPortalSpace=Portal;auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("serial"),++MetaPortalSerial);E->SetBoolField(TEXT("entered"),Portal);MetaPortalEvents.Add(MakeShared<FJsonValueObject>(E));if(MetaPortalEvents.Num()>32)MetaPortalEvents.RemoveAt(0);}
    }
    const double Now=FPlatformTime::Seconds();if(Now<NextAppraisalScan||Now-LastAppraisalRequest<1)return;
    if(UCM->Api>=2&&!UCM->Permissions.Contains(TEXT("inventory")))return;
    // The empty scan is work too. Previously a fully appraised inventory fell
    // through here every frame because only a sent request advanced the clock.
    NextAppraisalScan=Now+1;
    // Object IDs are session-local. Do not retain assessments for objects that
    // left awareness or inventory; refreshed mana/requirements must not stay stale.
    for(auto It=Appraisals.CreateIterator();It;++It)if(!Session->GetWorldObjects().Contains(It.Key()))It.RemoveCurrent();
    for(auto It=DirtyInventoryAppraisals.CreateIterator();It;++It)if(!Session->GetWorldObjects().Contains(*It))It.RemoveCurrent();
    for(auto It=CorpseFirstSeen.CreateIterator();It;++It)if(!Session->GetWorldObjects().Contains(It.Key()))It.RemoveCurrent();
    for(auto It=LootedCorpses.CreateIterator();It;++It)
    {
        if(Session->GetWorldObjects().Contains(It.Key()))It.Value().LastSeen=Now;
        else if(Now-It.Value().LastSeen>=3600)It.RemoveCurrent();
    }
    for(auto It=AppraisalRequests.CreateIterator();It;++It)if(!Session->GetWorldObjects().Contains(It.Key()))
    {C->BackgroundAppraisals.Remove(It.Key());It.RemoveCurrent();}
    int32 Request=0;
    // Identify the current opponent first, then look ahead while fighting.
    // Keep the existing one-request-per-second budget; a new opponent should
    // usually be assessed before the kill rather than stalling the handoff.
    auto Need=[&](int32 Id)
    {
        // An object with no prior request is immediately eligible. A missing entry is not a request
        // at time zero (which delayed first assessments on Linux's startup clock).
        const double* Last=AppraisalRequests.Find(Id);const auto* A=Appraisals.Find(Id);
        return !Last||Now-*Last>(!A||!A->bSuccess?30:120);
    };
    // Mana drains without ordinary property updates. A first-match scan of a
    // large inventory can perpetually revisit its first 120 entries and never
    // reach the worn jewel. Give worn mana and supplies a bounded refresh lane,
    // shared by running and stopped UCM, within the existing one-ID/sec budget.
    const bool Recharge=B(UCM->Profile,TEXT("item_mana"),true)
        &&(UCM->Running||B(UCM->Profile,TEXT("mana_charges_when_off")));
    if(Recharge)
    {
        double BestFraction=MAX_dbl,Oldest=MAX_dbl;
        for(const auto& Pair:Session->GetWorldObjects())
        {
            const auto& O=Pair.Value;if(!IsOwnedPluginItem(O))continue;
            const bool Worn=O.WielderId==C->GetPlayerGuid()||(O.CurrentWieldedLocation&&O.ParentGuid==C->GetPlayerGuid());
            if(!Worn&&!(O.ItemType&ACEItemType::ManaStone))continue;
            const auto* A=Appraisals.Find(O.Guid);
            const int32 Maximum=O.ItemMaximumMana!=-1?FMath::Max(0,O.ItemMaximumMana):(A?A->IntProperties.FindRef(108):0);
            if(Worn&&A&&A->bSuccess&&Maximum<=0)continue;
            // Carried supplies do not drain. Refresh unknown/used supplies here;
            // known ones keep the general cadence so a bag of stones cannot
            // occupy every request slot ahead of opponent appraisals.
            if(!Worn&&A&&A->bSuccess)continue;
            const double* Last=AppraisalRequests.Find(O.Guid);
            const double* Received=TargetAppraisalTimes.Find(O.Guid);
            // Also bounds retries when a server ignores or rejects appraisal.
            if((Last&&Now-*Last<30)||(Received&&Now-*Received<30))continue;
            const double Fraction=Worn&&A&&A->bSuccess&&Maximum>0
                ?double(O.ItemCurrentMana!=-1?FMath::Max(0,O.ItemCurrentMana):A->IntProperties.FindRef(107))/Maximum:2.;
            const double Age=Last?*Last:-MAX_dbl;
            if(Fraction<BestFraction||(Fraction==BestFraction&&(Age<Oldest
                ||(Age==Oldest&&(!Request||uint32(O.Guid)<uint32(Request))))))
            {Request=O.Guid;BestFraction=Fraction;Oldest=Age;}
        }
    }
    const auto Sel=C->GetSelectedObject();
    // Selection alone is not a request to assess another player. In particular,
    // fellowship recovery/buffing can select a deceptive player repeatedly.
    FACEWorldObject Selected;
    if(!Request && UCM->Running && Sel.Guid && C->GetWorldObject(Sel.Guid,Selected)
        && Selected.IsAttackable() && (Selected.ItemType&ACEItemType::Creature)
        && !Selected.bIsPlayer && !Selected.PetOwnerId && !Selected.bDying
        && !DefeatedTargets.Contains(Sel.Guid) && Need(Sel.Guid))Request=Sel.Guid;
    if(!Request&&UCM->Running&&CombatTarget)
    {
        FACEPosition Player=C->GetPlayerPosition();
        if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))PC->TryGetLocallyPredictedPosition(Player);
        double Nearest=FMath::Square(FMath::Clamp(N(UCM->Profile,TEXT("approach_range"),60),0.,240.)*100.);
        const uint32 Cell=uint32(Player.CellId);
        for(const auto& Pair:Session->GetWorldObjects())
        {
            const auto& O=Pair.Value;
            if(O.Guid==CombatTarget||!O.IsSelectableWorldObject()||!O.IsAttackable()||O.bIsPlayer||O.PetOwnerId
                ||!(O.ItemType&ACEItemType::Creature)||DefeatedTargets.Contains(O.Guid)||!Need(O.Guid))continue;
            const uint32 TargetCell=uint32(O.Position.CellId);
            if((Cell>>16)!=(TargetCell>>16)&&((Cell&0xffff)>=0x100||(TargetCell&0xffff)>=0x100))continue;
            const double Distance=FVector::DistSquared(Player.ToUnrealLocation(),O.Position.ToUnrealLocation());
            if(Distance<Nearest||(Distance==Nearest&&(!Request||uint32(O.Guid)<uint32(Request)))){Nearest=Distance;Request=O.Guid;}
        }
    }
    if(!Request)
    {
        double Oldest=MAX_dbl;
        for(const auto& Pair:Session->GetWorldObjects())
        {
            const auto& O=Pair.Value;if(!IsOwnedPluginItem(O)||!Need(O.Guid)
                ||(O.ItemType&(ACEItemType::SpellComponents|ACEItemType::Money|ACEItemType::Container)))continue;
            const double* Last=AppraisalRequests.Find(O.Guid);const double Age=Last?*Last:-MAX_dbl;
            if(Age<Oldest||(Age==Oldest&&(!Request||uint32(O.Guid)<uint32(Request)))){Request=O.Guid;Oldest=Age;}
        }
    }
    if(Request){LastAppraisalRequest=Now;AppraisalRequests.Add(Request,Now);C->RequestBackgroundAppraisal(Request);}
}
void UACEPluginSubsystem::UpdateRouteVisibility(const FACEPosition& Position,
    const TArray<TSharedPtr<FJsonValue>>& Route, const FACEPluginSightQuery& Sight,
    const TSharedPtr<FJsonObject>& Out)
{
    auto Visible=MakeShared<FJsonObject>();
    struct FCandidate { double Distance; int32 Index; FVector Location; };
    TArray<FCandidate> Nearest;
    const FVector Origin=Position.ToUnrealLocation();
    const uint32 Cell=uint32(Position.CellId);
    uint32 Signature=GetTypeHash(Route.Num());
    for(int32 Index=0;Index<Route.Num();++Index)
    {
        const auto Point=Route[Index]->AsObject();FACEPosition Dest;
        bool Legacy=false,WalkFirst=false;Point->TryGetBoolField(TEXT("legacy"),Legacy);Point->TryGetBoolField(TEXT("walk_first"),WalkFirst);
        Dest.CellId=int32(uint32(N(Point,TEXT("cell"))));Dest.Location=FVector(N(Point,TEXT("x")),N(Point,TEXT("y")),N(Point,TEXT("z")));
        Signature=HashCombineFast(Signature,HashCombineFast(GetTypeHash(Dest.CellId),GetTypeHash(Dest.Location)));
        Signature=HashCombineFast(Signature,uint32(Legacy)|(uint32(WalkFirst)<<1));
        if(Legacy&&!WalkFirst)continue;
        Visible->SetBoolField(FString::FromInt(Index+1),false);
        const uint32 Target=uint32(Dest.CellId);
        const bool Connected=Legacy||(Cell>>16)==(Target>>16)||((Cell&0xffff)<0x100&&(Target&0xffff)<0x100);
        const double Distance=FVector::DistSquared(Origin,Dest.ToUnrealLocation());
        // Same movement validation as Execute: no cross-dungeon walk or
        // unbounded geometry scan. Combat acquisition is well inside 400m.
        if(Dest.IsValid()&&Connected&&Distance<=FMath::Square(40000.))
            Nearest.Add({Distance,Index,Dest.ToUnrealLocation()});
    }
    Nearest.Sort([](const FCandidate& A,const FCandidate& B){return A.Distance==B.Distance?A.Index<B.Index:A.Distance<B.Distance;});
    if(RouteVisibilityOffset>=Nearest.Num()||RouteVisibilitySignature!=Signature||RouteVisibilityCell!=Cell
        ||FVector::DistSquared(RouteVisibilityOrigin,Origin)>2500)
        RouteVisibilityOffset=0;
    if(RouteVisibilityOffset==0)RouteVisibilityOrigin=Origin;
    RouteVisibilitySignature=Signature;RouteVisibilityCell=Cell;
    FACEPluginRouteGround Ground(*GetWorld(),2048);
    const int32 ScanEnd=FMath::Min(RouteVisibilityOffset+16,Nearest.Num());
    bool Found=false;
    while(RouteVisibilityOffset<ScanEnd)
    {
        const auto& Candidate=Nearest[RouteVisibilityOffset];
        if(!Ground.CanQueryPath(Origin,Candidate.Location))break;
        ++RouteVisibilityOffset;
        if(Ground.Reachable(Origin,Candidate.Location,Sight))
        {
            Visible->SetBoolField(FString::FromInt(Candidate.Index+1),true);
            Found=true;break; // sorted: the closest reachable entry is sufficient
        }
    }
    const bool Pending=!Found&&RouteVisibilityOffset<Nearest.Num();
    Out->SetBoolField(TEXT("route_visible_pending"),Pending);
    Out->SetObjectField(TEXT("route_visible"),Visible);
    if(!Pending)RouteVisibilityOffset=0;
}

TSharedPtr<FJsonValue> UACEPluginSubsystem::PluginItemSnapshot(const FACEWorldObject& Item)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    const auto& V=C->GetPlayerVitalsView();
    auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("id"),uint32(Item.Guid));J->SetNumberField(TEXT("wcid"),Item.WeenieClassId);
    J->SetStringField(TEXT("name"),ACERetailObjectNames::Name(Item));J->SetNumberField(TEXT("type"),uint32(Item.ItemType));J->SetNumberField(TEXT("icon"),uint32(Item.IconId));
    J->SetNumberField(TEXT("icon_overlay"),uint32(Item.IconOverlayId));J->SetNumberField(TEXT("icon_underlay"),uint32(Item.IconUnderlayId));J->SetNumberField(TEXT("ui_effects"),uint32(Item.UiEffects));
    J->SetNumberField(TEXT("object_class"),ACEVTObjectClass::Classify(Item));
    J->SetNumberField(TEXT("material"),Item.MaterialType);J->SetNumberField(TEXT("value"),Item.Value);J->SetNumberField(TEXT("count"),FMath::Max(1,Item.StackSize));
    J->SetNumberField(TEXT("max_stack"),Item.MaxStackSize);J->SetNumberField(TEXT("container"),uint32(Item.ContainerId));
    J->SetNumberField(TEXT("spell"),Item.SpellDID);
    if(ACEVTObjectClass::Classify(Item)==42&&Item.SpellDID)
    {
        uint32 School=0,Power=0,Category=0,Flags=0;double Duration=0;
        if(Dat->TryGetPluginSpellInfo(Item.SpellDID,School,Power,Category,Flags,Duration))
        {
            J->SetBoolField(TEXT("scroll_can_learn"),ACEScrollLearning::CanLearn(Power,School,V));
        }
    }
    J->SetNumberField(TEXT("slots"),Item.ValidLocations);
    J->SetBoolField(TEXT("auto_wield_left"),(Item.ObjectDescriptionFlags&ACEObjectDescFlag::WieldLeft)!=0);
    TArray<TSharedPtr<FJsonValue>> Palettes,PaletteColors;
    for(const auto& Palette:Item.Appearance.bHasWireSubPalettes?Item.Appearance.WireSubPalettes:Item.Appearance.SubPalettes)
    {Palettes.Add(MakeShared<FJsonValueNumber>(uint32(Palette.SubPaletteId)));PaletteColors.Add(MakeShared<FJsonValueNumber>(ACEVTLootPalette::ExampleColor(Dat,Palette)));}
    J->SetArrayField(TEXT("palettes"),Palettes);J->SetArrayField(TEXT("palette_colors"),PaletteColors);
    J->SetNumberField(TEXT("burden"),Item.Burden);
    J->SetNumberField(TEXT("structure"),Item.Structure);J->SetNumberField(TEXT("max_structure"),Item.MaxStructure);J->SetBoolField(TEXT("retained"),(Item.ObjectDescriptionFlags&ACEObjectDescFlag::Retained)!=0);
    J->SetNumberField(TEXT("icon_effects"),Item.UiEffects);
    if(Item.SalvageWorkmanship>=0)J->SetNumberField(TEXT("workmanship"),Item.SalvageWorkmanship);
    const bool Equipped=C->GetPlayerGuid()!=0 && (Item.WielderId==C->GetPlayerGuid() || (Item.CurrentWieldedLocation && Item.ParentGuid==C->GetPlayerGuid()));
    J->SetBoolField(TEXT("equipped"),Equipped);J->SetNumberField(TEXT("ammo_type"),Item.AmmoType);
    J->SetBoolField(TEXT("resource_available"),LootItemAvailable(C,Item));
    if(Item.ObjectDescriptionFlags&0x20000)J->SetBoolField(TEXT("lockpick_available"),LootItemAvailable(C,Item,true)&&Item.Structure>0);
    // Retention protects against destructive donor use, not releasing stored
    // mana. Both the supply and containing packs must remain outside trade.
    J->SetBoolField(TEXT("mana_supply_available"),(Item.ItemType&ACEItemType::ManaStone)&&LootItemAvailable(C,Item,true));
    J->SetBoolField(TEXT("salvage_tool_available"),SalvageToolAvailable(C,Item));
    J->SetNumberField(TEXT("equipped_slot"),uint32(Item.CurrentWieldedLocation));
    J->SetBoolField(TEXT("targeted"),ACEItemUseable::IsTargeted(Item.ItemUseable));J->SetBoolField(TEXT("usable"),ACEInventoryRules::IsUsable(Item.ItemUseable));
    J->SetBoolField(TEXT("healing_kit"),(Item.ObjectDescriptionFlags&0x10000)!=0); // Retail Healer descriptor.
    const auto* A=Appraisals.Find(Item.Guid);J->SetBoolField(TEXT("identified"),A&&A->bSuccess);
    J->SetBoolField(TEXT("pet_device"),ACEPetDevice::IsSummoningDevice(A));
    auto Ints=MakeShared<FJsonObject>(),Floats=MakeShared<FJsonObject>(),Strings=MakeShared<FJsonObject>();
    Ints->SetNumberField(TEXT("19"),Item.Value);Ints->SetNumberField(TEXT("5"),Item.Burden);Ints->SetNumberField(TEXT("131"),Item.MaterialType);
    // VT stores the singular wire name independently from the stack label.
    Strings->SetStringField(TEXT("1"),Item.Name);
    Strings->SetStringField(TEXT("184549376"),Item.PluralName);
    Ints->SetNumberField(TEXT("10"),Item.CurrentWieldedLocation);
    Ints->SetNumberField(TEXT("91"),Item.MaxStructure);Ints->SetNumberField(TEXT("92"),Item.Structure);
    Ints->SetNumberField(TEXT("95"),Item.RadarBlipColor);Ints->SetNumberField(TEXT("133"),Item.RadarBehavior);
    // Public description aliases used by Decal; these are not server property IDs.
    // VT g6.e stores the packed icon/overlay/underlay indices from PublicWeenieDesc.
    // Our parser adds the texture DID prefix for rendering. Remove it only for
    // these aliases (e.g. a level-4 overlay is 27703, not 0x06006C37). Keep all
    // 24 index bits for custom DATs, and leave the native/rendering IDs intact.
    auto IconIndex=[](int32 Id){return uint32(Id)&0x00ffffffu;};
    const TPair<int64,int64> PublicInts[]={{218103808,Item.WeenieClassId},{218103809,IconIndex(Item.IconId)},{218103810,uint32(Item.ContainerId)},{218103811,uint32(Item.Position.CellId)>>16},{218103812,Item.ItemsCapacity},{218103813,Item.ContainersCapacity},{218103814,FMath::Max(1,Item.StackSize)},{218103815,Item.MaxStackSize},{218103816,Item.SpellDID},{218103818,uint32(Item.WielderId)},{218103819,Item.CurrentWieldedLocation},{218103821,Item.ClothingPriority},{218103822,Item.ValidLocations},{218103823,Item.CombatUse},{218103824,Item.UiEffects},{218103825,Item.AmmoType},{218103826,Item.ItemUseable},{218103834,uint32(Item.ItemType)},{218103835,uint32(Item.ObjectDescriptionFlags)},{218103849,IconIndex(Item.IconOverlayId)},{218103850,IconIndex(Item.IconUnderlayId)}};
    for(const auto& Pair:PublicInts)Ints->SetNumberField(FString::Printf(TEXT("%lld"),Pair.Key),Pair.Value);
    const TPair<int32,int64> ExtraInts[]={{218103817,Item.PlacementPosition},{218103820,uint32(Item.MonarchGuid)},
        {218103827,uint32(Item.HouseOwnerId)},{218103828,Item.HookType},
        {218103832,int32(Item.CreateFlags1)},{218103833,int32(Item.CreateFlags2)},
        {218103843,uint32(Item.TargetType)},{218103846,Item.PublicPhysicsScript},{218103847,int32(Item.PhysicsDataFlags)},
        {218103851,uint32(Item.WielderId)},{218103854,uint32(Item.HouseOwnerId)}};
    for(const auto& Pair:ExtraInts)Ints->SetNumberField(FString::FromInt(Pair.Key),Pair.Value);
    Floats->SetNumberField(TEXT("167772168"),Item.UseRadius);
    Floats->SetNumberField(TEXT("280"),Item.CooldownDuration);
    if(Item.SalvageWorkmanship>=0)Floats->SetNumberField(TEXT("167772169"),Item.SalvageWorkmanship);
    if(A&&A->bSuccess)
    {
        for(const auto& Prop:A->IntProperties)Ints->SetNumberField(FString::FromInt(Prop.Key),Prop.Value);
        for(const auto& Prop:A->FloatProperties)Floats->SetNumberField(FString::FromInt(Prop.Key),Prop.Value);
        for(const auto& Prop:A->StringProperties)Strings->SetStringField(FString::FromInt(Prop.Key),Prop.Value.Left(1024));
        if(A->bHasWeaponProfile)
        {
            Ints->SetNumberField(TEXT("218103839"),A->WeaponTime);Ints->SetNumberField(TEXT("218103840"),A->WeaponSkill);
            Ints->SetNumberField(TEXT("218103841"),A->DamageType);Ints->SetNumberField(TEXT("218103842"),A->Damage);
            Floats->SetNumberField(TEXT("167772171"),A->DamageVariance);Floats->SetNumberField(TEXT("167772172"),A->WeaponOffense);
            Floats->SetNumberField(TEXT("167772173"),A->WeaponMaxVelocity);Floats->SetNumberField(TEXT("167772174"),A->WeaponDamageMod);
        }
        Ints->SetNumberField(TEXT("218103838"),A->SpellIds.Num());
        // Decal order differs from the wire: Slash/Pierce/Bludgeon/Acid/Electric/Fire/Cold.
        static const int32 ArmorOrder[]={0,1,2,5,7,4,3};
        for(int Index=0;Index<UE_ARRAY_COUNT(ArmorOrder);++Index)if(A->ArmorResistances.IsValidIndex(ArmorOrder[Index]))
            Floats->SetNumberField(FString::FromInt(167772160+Index),A->ArmorResistances[ArmorOrder[Index]]);
        if(A->bHasCreatureHealth)
        {
            Ints->SetNumberField(TEXT("226492416"),A->CreatureProfileFlags);
            Ints->SetNumberField(TEXT("226492417"),A->Health);Ints->SetNumberField(TEXT("226492418"),A->MaxHealth);
            if(A->bHasCreatureAttributes)
            {
                const int32 Stats[]={A->Strength,A->Endurance,A->Quickness,A->Coordination,A->Focus,A->Self,A->Stamina,A->Mana,A->MaxStamina,A->MaxMana};
                for(int I=0;I<UE_ARRAY_COUNT(Stats);++I)Ints->SetNumberField(FString::FromInt(226492419+I),Stats[I]);
            }
            if(A->CreatureProfileFlags&1){Ints->SetNumberField(TEXT("226492429"),A->AttributeHighlights);Ints->SetNumberField(TEXT("226492430"),A->AttributeColors);}
        }
        TArray<TSharedPtr<FJsonValue>> SpellNames,SpellIds;
        for(int32 Id:A->SpellIds){SpellIds.Add(MakeShared<FJsonValueNumber>(Id));FString Name;uint32 Icon=0;if(Dat->TryGetSpellInfo(uint32(Id),Name,Icon))SpellNames.Add(MakeShared<FJsonValueString>(Name));}
        J->SetArrayField(TEXT("spell_names"),SpellNames);
        J->SetArrayField(TEXT("spell_ids"),SpellIds);
    }
    J->SetObjectField(TEXT("int_properties"),Ints);J->SetObjectField(TEXT("float_properties"),Floats);J->SetObjectField(TEXT("string_properties"),Strings);

    if(A&&A->bSuccess)
    {
        bool RequiresServerCheck=false;
        const FString WieldFailure=Equipped?FString():ACEPluginEquipment::WieldFailure(*A,V,&RequiresServerCheck);
        J->SetBoolField(TEXT("can_wield"),WieldFailure.IsEmpty());
        J->SetBoolField(TEXT("wield_requires_server_check"),RequiresServerCheck);
        if(!WieldFailure.IsEmpty())J->SetStringField(TEXT("wield_reason"),WieldFailure);
        if(Item.SalvageWorkmanship<0)if(const auto* W=A->IntProperties.Find(105))
            J->SetNumberField(TEXT("workmanship"),double(*W)/FMath::Max(1,A->IntProperties.FindRef(170)));
        // Casters do not have a weapon appraisal profile. VT reads their
        // WandElemDmgType (integer quality 45), not the absent profile's zero.
        J->SetNumberField(TEXT("damage_type"),(Item.ItemType&ACEItemType::Caster)?A->IntProperties.FindRef(45):A->DamageType);
        J->SetNumberField(TEXT("damage"),A->Damage);
        J->SetNumberField(TEXT("damage_mod"),A->WeaponDamageMod);J->SetNumberField(TEXT("weapon_time"),A->WeaponTime);
        J->SetNumberField(TEXT("weapon_skill"),A->WeaponSkill);J->SetNumberField(TEXT("variance"),A->DamageVariance);
        J->SetNumberField(TEXT("weapon_type"),A->IntProperties.FindRef(353));J->SetNumberField(TEXT("attack_type"),A->IntProperties.FindRef(47));
        J->SetNumberField(TEXT("slayer"),A->IntProperties.FindRef(166));J->SetNumberField(TEXT("slayer_mod"),A->FloatProperties.FindRef(138));
        J->SetNumberField(TEXT("element_mod"),A->FloatProperties.FindRef(152));
        J->SetNumberField(TEXT("boost_vital"),A->IntProperties.FindRef(89));J->SetNumberField(TEXT("boost"),A->IntProperties.FindRef(90));
        if(const double* HealMod=A->FloatProperties.Find(100))J->SetNumberField(TEXT("heal_mod"),*HealMod);
        J->SetNumberField(TEXT("mana"),A->IntProperties.FindRef(107));J->SetNumberField(TEXT("max_mana"),A->IntProperties.FindRef(108));
        J->SetBoolField(TEXT("mana_empty"),A->IntProperties.FindRef(107)<=0);
        if(const auto* Rating=A->IntProperties.Find(307))J->SetNumberField(TEXT("damage_rating"),*Rating);
        J->SetNumberField(TEXT("armor"),A->IntProperties.FindRef(28));
        J->SetNumberField(TEXT("spell"),Item.SpellDID);
    }
    // GDLE sends public current-mana updates while recharging. They supersede
    // the older appraisal without invalidating weapon/buff eligibility. ACE
    // also works through the appraisal path when no live quality is sent.
    if(Item.ItemCurrentMana!=-1)
    {
        J->SetNumberField(TEXT("mana"),FMath::Max(0,Item.ItemCurrentMana));J->SetBoolField(TEXT("mana_empty"),Item.ItemCurrentMana<=0);
        if(Item.ItemCurrentMana>=0)Ints->SetNumberField(TEXT("107"),Item.ItemCurrentMana);else Ints->RemoveField(TEXT("107"));
    }
    if(Item.ItemMaximumMana!=-1)
    {
        J->SetNumberField(TEXT("max_mana"),FMath::Max(0,Item.ItemMaximumMana));
        if(Item.ItemMaximumMana>=0)Ints->SetNumberField(TEXT("108"),Item.ItemMaximumMana);else Ints->RemoveField(TEXT("108"));
    }
    // Item spells use activation and item mana, not learned-school components.
    if((Item.ItemType&ACEItemType::Caster)&&Item.SpellDID&&A&&A->bSuccess)
    {
        uint32 School=0,Power=0,Category=0,Flags=0;double Duration=0;
        if(Dat->TryGetPluginSpellInfo(Item.SpellDID,School,Power,Category,Flags,Duration))
        {
            auto Spell=MakeShared<FJsonObject>();FString Name;uint32 Icon=0,Level=0,LevelSchool=0;
            Dat->TryGetSpellInfo(Item.SpellDID,Name,Icon);Dat->TryGetSpellSchoolAndLevel(Item.SpellDID,LevelSchool,Level);
            Spell->SetNumberField(TEXT("id"),Item.SpellDID);Spell->SetStringField(TEXT("name"),Name);
            Spell->SetNumberField(TEXT("power"),Power);Spell->SetNumberField(TEXT("category"),Category);Spell->SetNumberField(TEXT("level"),Level);
            Spell->SetBoolField(TEXT("caster_target"),(Flags&8)!=0);Spell->SetBoolField(TEXT("beneficial"),(Flags&4)!=0);
            Spell->SetNumberField(TEXT("duration"),Duration);Spell->SetNumberField(TEXT("flags"),Flags);Spell->SetNumberField(TEXT("school"),School);
            Spell->SetNumberField(TEXT("skill"),A->IntProperties.FindRef(106));
            const bool Available=ACEPluginEquipment::CanActivate(*A,V)
                &&(Equipped||LootItemAvailable(C,Item,true))
                &&(N(J,TEXT("max_mana"))<=0||N(J,TEXT("mana"))>0);
            J->SetObjectField(TEXT("innate_spell"),Spell);J->SetBoolField(TEXT("innate_spell_available"),Available);
            if((Flags&4)&&(Flags&8)&&Duration==0)
            {J->SetObjectField(TEXT("innate_recovery"),Spell);J->SetBoolField(TEXT("innate_recovery_available"),Available);}
        }
    }
    // VT ga/dz uses the first appraised spell of a physical weapon for proc
    // debuffing. It is triggered by attacks, never by UseWithTarget. Do not scan
    // past a buff/damage spell and invent a different proc for this weapon.
    if((Item.ItemType&(ACEItemType::MeleeWeapon|ACEItemType::MissileWeapon))
        &&!(Item.ValidLocations&ACEEquipMask::MissileAmmo)&&A&&A->bSuccess&&!A->SpellIds.IsEmpty())
    {
        const uint32 SpellId=A->SpellIds[0];uint32 School=0,Power=0,Category=0,Flags=0;double Duration=0;
        if(Dat->TryGetPluginSpellInfo(SpellId,School,Power,Category,Flags,Duration)
            &&School!=1&&School!=3&&!(Flags&(4|8))&&Duration>0)
        {
            auto Spell=MakeShared<FJsonObject>();FString Name;uint32 Icon=0;
            Dat->TryGetSpellInfo(SpellId,Name,Icon);
            Spell->SetNumberField(TEXT("id"),SpellId);Spell->SetStringField(TEXT("name"),Name);
            Spell->SetNumberField(TEXT("power"),Power);Spell->SetNumberField(TEXT("category"),Category);
            Spell->SetNumberField(TEXT("school"),School);Spell->SetNumberField(TEXT("flags"),Flags);
            Spell->SetNumberField(TEXT("duration"),Duration);Spell->SetNumberField(TEXT("skill"),A->IntProperties.FindRef(106));
            Spell->SetBoolField(TEXT("beneficial"),false);Spell->SetBoolField(TEXT("caster_target"),false);
            J->SetObjectField(TEXT("proc_debuff"),Spell);
            J->SetBoolField(TEXT("proc_debuff_available"),ACEPluginEquipment::CanActivate(*A,V)
                &&(Equipped||LootItemAvailable(C,Item,true))&&(N(J,TEXT("max_mana"))<=0||N(J,TEXT("mana"))>0));
        }
    }
    AddEquipmentSnapshot(Item,J);
    return MakeShared<FJsonValueObject>(J);
}
TSharedPtr<FJsonObject> UACEPluginSubsystem::UCMGrenadeSpell(const FACEWorldObject& Item) const
{
    const auto P=PolicyPlugin();if(!P||!P->Running)return nullptr;
    const auto Config=UCMServicesProfile?UCMServicesProfile:P->Profile;
    const TSharedPtr<FJsonObject> *Catalog=nullptr,*Entry=nullptr;const TArray<TSharedPtr<FJsonValue>>* Assist=nullptr;
    if(!Config->TryGetObjectField(TEXT("grenade_catalog"),Catalog)||!(*Catalog)->TryGetObjectField(Item.Name,Entry)
        ||!Config->TryGetArrayField(TEXT("assist_items"),Assist)||!Assist->ContainsByPredicate([&](const auto& V){return V->Type==EJson::Object&&N(V->AsObject(),TEXT("type"),-1)==7&&S(V->AsObject(),TEXT("name"))==Item.Name;}))return nullptr;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    const bool Equipped=Item.WielderId==C->GetPlayerGuid()||(Item.CurrentWieldedLocation&&Item.ParentGuid==C->GetPlayerGuid());
    if(!(Item.ItemType&ACEItemType::MissileWeapon)||(!Equipped&&!LootItemAvailable(C,Item,true))||Item.StackSize==0)return nullptr;
    FACEAppraisalInfo Requirements;Requirements.bSuccess=true;
    const int32 Kind=int32(N(*Entry,TEXT("WieldReqType"),-1));if(Kind<0||Kind>12)return nullptr;
    Requirements.IntProperties.Add(158,Kind);Requirements.IntProperties.Add(159,int32(N(*Entry,TEXT("WieldReqAttribute"))));Requirements.IntProperties.Add(160,int32(N(*Entry,TEXT("WieldReqValue"))));
    if(!ACEPluginEquipment::WieldFailure(Requirements,C->GetPlayerVitalsView()).IsEmpty())return nullptr;
    if(const auto* Actual=Appraisals.Find(Item.Guid);Actual&&Actual->bSuccess&&!ACEPluginEquipment::WieldFailure(*Actual,C->GetPlayerVitalsView()).IsEmpty())return nullptr;
    const uint32 SpellId=uint32(N(*Entry,TEXT("Spell")));uint32 School=0,Power=0,Category=0,Flags=0,Icon=0;double Duration=0;FString Name;
    if(!Dat->TryGetPluginSpellInfo(SpellId,School,Power,Category,Flags,Duration)||(Flags&(4|8))||Duration<=0||!Dat->TryGetSpellInfo(SpellId,Name,Icon))return nullptr;
    auto Spell=MakeShared<FJsonObject>();Spell->SetNumberField(TEXT("id"),SpellId);Spell->SetStringField(TEXT("name"),Name);
    Spell->SetNumberField(TEXT("school"),School);Spell->SetNumberField(TEXT("power"),Power);Spell->SetNumberField(TEXT("category"),Category);
    Spell->SetNumberField(TEXT("flags"),Flags);Spell->SetNumberField(TEXT("duration"),Duration);Spell->SetNumberField(TEXT("skill"),N(*Entry,TEXT("Spellcraft")));
    return Spell;
}

void UACEPluginSubsystem::ExtendUCMCombatSnapshot(const TSharedPtr<FJsonObject>& Out)
{
    TArray<TSharedPtr<FJsonValue>> Choices;const TArray<TSharedPtr<FJsonValue>>* Inventory=nullptr;
    if(Out->TryGetArrayField(TEXT("inventory"),Inventory))
    {
        auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
        for(const auto& Value:*Inventory)
        {
            FACEWorldObject Item;if(!C->GetWorldObject(int32(uint32(N(Value->AsObject(),TEXT("id")))),Item))continue;
            if(auto Spell=UCMGrenadeSpell(Item))
            {auto Choice=MakeShared<FJsonObject>();Choice->SetNumberField(TEXT("item"),uint32(Item.Guid));Choice->SetObjectField(TEXT("spell"),Spell);Choices.Add(MakeShared<FJsonValueObject>(Choice));if(Choices.Num()>=64)break;}
        }
    }
    Out->SetArrayField(TEXT("grenade_debuffs"),Choices);
}

void UACEPluginSubsystem::ExtendSnapshot(const TSharedPtr<FJsonObject>& Out)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginSnapshot);
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();if(!Session)return;
    const auto& V=C->GetPlayerVitalsView();const double Now=FPlatformTime::Seconds();
    const bool* ComponentsRequired=V.QualityBools.Find(68); // SpellComponentsRequired, absent means true.
    Out->SetBoolField(TEXT("components_required"),!ComponentsRequired||*ComponentsRequired);
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))Out->SetBoolField(TEXT("jumping"),PC->IsPluginJumpPending());
    Out->SetNumberField(TEXT("player"),uint32(C->GetPlayerGuid()));Out->SetNumberField(TEXT("action_serial"),ActionSerial);
    Out->SetNumberField(TEXT("action_error"),LastActionError);Out->SetNumberField(TEXT("container"),uint32(C->GetOpenExternalContainerGuid()));
    Out->SetNumberField(TEXT("teleport_sequence"),C->GetTeleportSeq());
    Out->SetNumberField(TEXT("last_spell"),LastCompletedSpell);
    Out->SetBoolField(TEXT("last_spell_confirmed"),LastSpellConfirmed);
    TArray<TSharedPtr<FJsonValue>> Confirmations;
    for(const auto& Dialog:Session->GetConfirmations()){auto D=MakeShared<FJsonObject>();D->SetNumberField(TEXT("type"),Dialog.Type);D->SetNumberField(TEXT("context"),Dialog.Context);Confirmations.Add(MakeShared<FJsonValueObject>(D));}
    Out->SetArrayField(TEXT("confirmations"),Confirmations);
    Out->SetNumberField(TEXT("chat_serial"),MetaChatSerial);Out->SetArrayField(TEXT("chat_events"),MetaChatEvents);
    Out->SetArrayField(TEXT("combat_events"),CombatOutcomes);
    Out->SetArrayField(TEXT("action_events"),ActionOutcomes);
    Out->SetNumberField(TEXT("inventory_failure_serial"),Session->GetInventoryFailureSerial());
    Out->SetNumberField(TEXT("inventory_failure_item"),uint32(Session->GetInventoryFailureItem()));
    Out->SetNumberField(TEXT("inventory_failure_error"),Session->GetInventoryFailureError());
    if(LastCombatDeath)Out->SetObjectField(TEXT("combat_death"),LastCombatDeath);
    else Out->RemoveField(TEXT("combat_death"));
    Out->SetNumberField(TEXT("portal_serial"),MetaPortalSerial);Out->SetArrayField(TEXT("portal_events"),MetaPortalEvents);Out->SetBoolField(TEXT("portal_space"),MetaPortalSpace);
    Out->SetNumberField(TEXT("selected"),uint32(C->GetSelectedObject().Guid));Out->SetNumberField(TEXT("vendor"),uint32(C->GetOpenVendorGuid()));
    FACEWorldObject Vendor;
    TArray<TSharedPtr<FJsonValue>> Stock;
    TArray<TSharedPtr<FJsonValue>> Notes;
    int64 Pyreals=0;
    if(C->GetOpenVendorGuid()&&C->GetWorldObject(C->GetOpenVendorGuid(),Vendor))
    {
        Out->SetStringField(TEXT("vendor_name"),Vendor.Name);Out->SetNumberField(TEXT("vendor_wcid"),Vendor.WeenieClassId);
        for(const auto& Item:C->GetVendorMerchandise())
        {
            auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("id"),uint32(Item.Guid));J->SetNumberField(TEXT("wcid"),Item.WeenieClassId);
            J->SetStringField(TEXT("name"),ACERetailObjectNames::Name(Item));J->SetNumberField(TEXT("limit"),ACEInventoryRules::VendorPurchaseLimit(Item));
            J->SetNumberField(TEXT("unit_value"),FMath::Max(0,Item.Value)/FMath::Max(1,Item.StackSize));
            J->SetNumberField(TEXT("sell_rate"),Item.ItemType==ACEItemType::PromissoryNote?1.15:double(C->GetVendorSellRate()));
            Stock.Add(MakeShared<FJsonValueObject>(J));
        }
        // Use live ownership/retained/trade flags here, not the appraisal cache.
        for(const auto& Pair:Session->GetWorldObjects())
        {
            const auto& Item=Pair.Value;
            if(!C->IsOwnedInventoryItem(Item))continue;
            if(Item.WeenieClassId==273)Pyreals+=FMath::Max(1,Item.StackSize);
            if(Item.ItemType!=ACEItemType::PromissoryNote || Item.Value<=0
                || !LootItemAvailable(C,Item) || !Session->CanVendorBuyItem(Item))continue;
            const int32 UnitValue=Item.Value/FMath::Max(1,Item.StackSize);
            if(UnitValue<=0)continue;
            auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("id"),uint32(Item.Guid));
            J->SetNumberField(TEXT("wcid"),Item.WeenieClassId);J->SetStringField(TEXT("name"),ACERetailObjectNames::Name(Item));
            J->SetNumberField(TEXT("count"),FMath::Max(1,Item.StackSize));J->SetNumberField(TEXT("unit_value"),ACEVendorPricing::VendorBuyPayout(Item,1,C->GetVendorBuyRate()));
            Notes.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    Out->SetArrayField(TEXT("vendor_stock"),Stock);
    Out->SetArrayField(TEXT("vendor_trade_notes"),Notes);
    Out->SetBoolField(TEXT("vendor_uses_pyreals"),Session->VendorUsesPyreals());
    Out->SetNumberField(TEXT("pyreals"),double(Pyreals));
    int32 Encumbrance=0;Session->TryGetPlayerEncumbrance(Encumbrance);
    Out->SetNumberField(TEXT("burden_percent"),100.*FMath::Max(0,Encumbrance)/(FMath::Max(1,V.GetBuffedStrength())*(150.+30.*FMath::Clamp(V.CarryingCapacityAugs,0,5))));Out->SetNumberField(TEXT("level"),V.Level);
    auto CharStrings=MakeShared<FJsonObject>();for(const auto& Pair:V.QualityStrings)CharStrings->SetStringField(FString::FromInt(Pair.Key),Pair.Value);FACEWorldObject PlayerObject;
    if(C->GetWorldObject(C->GetPlayerGuid(),PlayerObject) && !CharStrings->HasField(TEXT("1")))CharStrings->SetStringField(TEXT("1"),PlayerObject.Name);
    Out->SetObjectField(TEXT("char_strings"),CharStrings);
    auto CharInts=MakeShared<FJsonObject>();for(const auto& Pair:V.StatQualityInts)CharInts->SetNumberField(FString::FromInt(Pair.Key),Pair.Value);Out->SetObjectField(TEXT("char_ints"),CharInts);
    auto CharQuads=MakeShared<FJsonObject>();for(const auto& Pair:V.QualityInt64s)CharQuads->SetNumberField(FString::FromInt(Pair.Key),double(Pair.Value));Out->SetObjectField(TEXT("char_quads"),CharQuads);
    auto CharDoubles=MakeShared<FJsonObject>();for(const auto& Pair:V.QualityDoubles)CharDoubles->SetNumberField(FString::FromInt(Pair.Key),Pair.Value);Out->SetObjectField(TEXT("char_doubles"),CharDoubles);
    auto CharBools=MakeShared<FJsonObject>();for(const auto& Pair:V.QualityBools)CharBools->SetNumberField(FString::FromInt(Pair.Key),Pair.Value?1:0);Out->SetObjectField(TEXT("char_bools"),CharBools);
    Out->SetNumberField(TEXT("base_health"),V.HealthStart+V.HealthRanks+FMath::RoundToInt(V.Endurance/2.f));
    Out->SetNumberField(TEXT("base_stamina"),V.StaminaStart+V.StaminaRanks+V.Endurance);
    Out->SetNumberField(TEXT("base_mana"),V.ManaStart+V.ManaRanks+V.Self);
    const auto Packs=C->GetPlayerPacks();
    TMap<int32,int32> PackCounts;PackCounts.Add(C->GetPlayerGuid(),0);
    for(const auto& Pack:Packs)PackCounts.Add(Pack.Guid,0);
    Session->GetPackItemCounts(PackCounts);
    auto Free=[&](int32 Id,int32 Capacity){return Capacity==-1?MAX_int32:FMath::Max(0,Capacity-PackCounts.FindRef(Id));};
    int FreeSlots=Free(C->GetPlayerGuid(),PlayerObject.ItemsCapacity!=0?PlayerObject.ItemsCapacity:102);
    Out->SetNumberField(TEXT("main_pack_slots"),FreeSlots);
    const auto OfferedPacks=C->GetTradeSelfItems();
    for(const auto& Pack:Packs)if(!OfferedPacks.Contains(Pack.Guid))
        FreeSlots=int32(FMath::Min(int64(MAX_int32),int64(FreeSlots)+Free(Pack.Guid,Pack.ItemsCapacity)));
    Out->SetNumberField(TEXT("pack_slots"),FreeSlots);Out->SetNumberField(TEXT("combat_mode"),V.CombatMode);
    Out->SetBoolField(TEXT("in_fellowship"),C->GetFellowship().bValid);
    Out->SetStringField(TEXT("world_name"),C->GetServerName());
    const auto Fellowship=C->GetFellowship();auto Fellow=MakeShared<FJsonObject>();
    Fellow->SetBoolField(TEXT("valid"),Fellowship.bValid);Fellow->SetStringField(TEXT("name"),Fellowship.Name);
    Fellow->SetNumberField(TEXT("leader"),uint32(Fellowship.LeaderGuid));Fellow->SetBoolField(TEXT("open"),Fellowship.bOpen);Fellow->SetBoolField(TEXT("locked"),Fellowship.bLocked);
    TArray<TSharedPtr<FJsonValue>> Members;
    for(const auto& M:Fellowship.Members)
    {
        auto Member=MakeShared<FJsonObject>();Member->SetNumberField(TEXT("id"),uint32(M.Guid));Member->SetStringField(TEXT("name"),M.Name);Member->SetBoolField(TEXT("share_loot"),M.bShareLoot);
        Member->SetNumberField(TEXT("health"),M.HealthCur);Member->SetNumberField(TEXT("max_health"),M.HealthMax);
        Member->SetNumberField(TEXT("stamina"),M.StaminaCur);Member->SetNumberField(TEXT("max_stamina"),M.StaminaMax);
        Member->SetNumberField(TEXT("mana"),M.ManaCur);Member->SetNumberField(TEXT("max_mana"),M.ManaMax);
        Member->SetNumberField(TEXT("vitals_age"),M.VitalsReceivedAt>0?FMath::Max(0.,Now-M.VitalsReceivedAt):1.e9);
        Members.Add(MakeShared<FJsonValueObject>(Member));
    }
    Fellow->SetArrayField(TEXT("members"),Members);Out->SetObjectField(TEXT("fellowship"),Fellow);
    TArray<TSharedPtr<FJsonValue>> PackJson;
    for(const auto& Pack:Packs)
    {
        if(OfferedPacks.Contains(Pack.Guid))continue;
        auto Bag=MakeShared<FJsonObject>();Bag->SetNumberField(TEXT("id"),uint32(Pack.Guid));
        Bag->SetNumberField(TEXT("free"),Free(Pack.Guid,Pack.ItemsCapacity));PackJson.Add(MakeShared<FJsonValueObject>(Bag));
    }
    Out->SetArrayField(TEXT("packs"),PackJson);
    TArray<TSharedPtr<FJsonValue>> Skills,UsableSkills;
    auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    for(const auto& Skill:V.Skills)
    {
        if(Skill.AdvancementClass>=2)Skills.Add(MakeShared<FJsonValueNumber>(Skill.SkillId));
        uint32 MinLevel=2;Dat->TryGetSkillMinLevel(Skill.SkillId,MinLevel);
        if(Skill.AdvancementClass>=2 || (Skill.AdvancementClass>=1 && MinLevel==1))UsableSkills.Add(MakeShared<FJsonValueNumber>(Skill.SkillId));
    }
    auto SkillStats=MakeShared<FJsonObject>();
    for(const auto& Skill:V.Skills){auto Record=MakeShared<FJsonObject>();Record->SetNumberField(TEXT("base"),Skill.Base);Record->SetNumberField(TEXT("current"),Skill.Current);Record->SetNumberField(TEXT("training"),Skill.AdvancementClass);SkillStats->SetObjectField(FString::FromInt(Skill.SkillId),Record);}
    Out->SetObjectField(TEXT("skills"),SkillStats);
    Out->SetArrayField(TEXT("trained_skills"),Skills);
    Out->SetArrayField(TEXT("usable_skills"),UsableSkills);
    ExtendInventorySnapshot(Out);
    auto ItemJson=[this](const FACEWorldObject& Item){return PluginItemSnapshot(Item);};
    bool AcceptOthers=false;if(auto Plugin=PolicyPlugin())Plugin->Profile->TryGetBoolField(TEXT("buff_others"),AcceptOthers);
    if(!AcceptOthers)ClearBuffRequests();
    if(BuffRequests.Num())
    {
        auto Request=MakeShared<FJsonObject>(*BuffRequests[0]->AsObject());
        bool Started=false;Request->TryGetBoolField(TEXT("started"),Started);Request->SetBoolField(TEXT("started"),Started);
        const int32 Recipient=int32(uint32(N(Request,TEXT("player"))));FACEWorldObject Person;
        const bool Present=C->GetWorldObject(Recipient,Person)&&Person.bIsPlayer;
        Request->SetBoolField(TEXT("present"),Present);
        Request->SetNumberField(TEXT("distance"),Present?FVector::Distance(Person.Position.ToUnrealLocation(),C->GetPlayerPosition().ToUnrealLocation())/100:1.e9);
        TArray<TSharedPtr<FJsonValue>> Gear;
        if(Present)for(const auto& Pair:Session->GetWorldObjects())
            if(Pair.Value.WielderId==Recipient || (Pair.Value.CurrentWieldedLocation && Pair.Value.ParentGuid==Recipient))
                Gear.Add(ItemJson(Pair.Value));
        Request->SetArrayField(TEXT("equipment"),Gear);Out->SetObjectField(TEXT("buff_request"),Request);
    }
    else Out->RemoveField(TEXT("buff_request"));
    FACEPosition P=C->GetPlayerPosition();if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))PC->TryGetLocallyPredictedPosition(P);
    auto Position=Out->GetObjectField(TEXT("position"));const FVector Global=P.ToUnrealLocation()/100;
    Position->SetNumberField(TEXT("gx"),Global.X);Position->SetNumberField(TEXT("gy"),Global.Y);Position->SetNumberField(TEXT("gz"),Global.Z);
    FACEWorldObject Container;
    if(!CachedSpeciesNames||CachedSpeciesNames->Values.IsEmpty())
    {
        CachedSpeciesNames=MakeShared<FJsonObject>();
        auto* Resolver=NewObject<UACEUIResourceResolver>(this);Resolver->Initialize(Dat);
        for(const auto& Pair:Resolver->ResolveEnumStrings(0x10000005))CachedSpeciesNames->SetStringField(FString::FromInt(Pair.Key),Pair.Value);
    }
    Out->SetObjectField(TEXT("species_names"),CachedSpeciesNames);
    Out->SetBoolField(TEXT("container_is_corpse"),C->GetWorldObject(C->GetOpenExternalContainerGuid(),Container)&&Container.IsCorpse());
    TArray<TSharedPtr<FJsonValue>> Targets,Corpses,Contents,RouteObjects;bool OwnedPet=false;TSet<int32> ArmoredParents;
    for(const auto& Pair:Session->GetWorldObjects())
    {
        const auto& Obj=Pair.Value;
        // VT's hasshield predicate checks any attached Armor-class object,
        // rather than requiring appraisal or a known shield WCID.
        if(ACEVTObjectClass::Classify(Obj)==2)
        {if(Obj.WielderId)ArmoredParents.Add(Obj.WielderId);if(Obj.ParentGuid)ArmoredParents.Add(Obj.ParentGuid);}
        // Hidden/despawned pets can linger in awareness until the delete message.
        // Only a live, present creature prevents another summon.
        if(Obj.PetOwnerId==C->GetPlayerGuid() && Obj.IsSelectableWorldObject()
            && (Obj.ItemType&ACEItemType::Creature) && !Obj.IsCorpse() && !Obj.bPhysicsDescriptionPending
            && !DefeatedTargets.Contains(Obj.Guid)
            && FVector::DistSquared(Obj.Position.ToUnrealLocation(),P.ToUnrealLocation())<=FMath::Square(24000.))OwnedPet=true;
        if(Obj.IsCorpse()&&!CorpseFirstSeen.Contains(Obj.Guid))CorpseFirstSeen.Add(Obj.Guid,Now);
        if(C->GetOpenExternalContainerGuid() && Obj.ContainerId==C->GetOpenExternalContainerGuid()){Contents.Add(ItemJson(Obj));continue;}
        if(!Obj.IsSelectableWorldObject() || FVector::DistSquared(Obj.Position.ToUnrealLocation(),P.ToUnrealLocation())>FMath::Square(24000.))continue;
        const bool Monster=Obj.IsAttackable()&&!Obj.bIsPlayer&&!Obj.PetOwnerId&&(Obj.ItemType&ACEItemType::Creature);
        if(!Monster&&!Obj.IsCorpse())
        {
            if(Obj.bIsPlayer || ACEInventoryRules::IsUsable(Obj.ItemUseable))
            {auto R=Pos(Obj.Position);R->SetNumberField(TEXT("id"),uint32(Obj.Guid));R->SetNumberField(TEXT("wcid"),Obj.WeenieClassId);R->SetStringField(TEXT("name"),Obj.Name);R->SetNumberField(TEXT("object_class"),ACEVTObjectClass::Classify(Obj));
                // The physics update is authoritative for passage. Motion can
                // still say On after an auto-close or Off while a door opens.
                R->SetBoolField(TEXT("door_open"),Obj.IsDoor()&&Obj.IsEthereal());
                if(Obj.IsDoor())
                {
                    const auto* Door=Appraisals.Find(Obj.Guid);
                    R->SetBoolField(TEXT("door_identified"),Door&&Door->bSuccess);
                    if(Door&&Door->bSuccess)
                    {R->SetBoolField(TEXT("door_locked"),Door->BoolProperties.FindRef(3));R->SetNumberField(TEXT("lock_difficulty"),Door->IntProperties.FindRef(38));}
                }
                R->SetNumberField(TEXT("distance"),FVector::Distance(Obj.Position.ToUnrealLocation(),P.ToUnrealLocation())/100);RouteObjects.Add(MakeShared<FJsonValueObject>(R));}
            continue;
        }
        auto J=Pos(Obj.Position);J->SetNumberField(TEXT("id"),uint32(Obj.Guid));J->SetStringField(TEXT("name"),Obj.Name);
        J->SetNumberField(TEXT("distance"),FVector::Distance(Obj.Position.ToUnrealLocation(),P.ToUnrealLocation())/100);
        J->SetNumberField(TEXT("object_class"),ACEVTObjectClass::Classify(Obj));J->SetNumberField(TEXT("wcid"),Obj.WeenieClassId);
        if(Monster)
        {
            if(DefeatedTargets.Contains(Obj.Guid))continue;
            const FVector Delta=Obj.Position.ToUnrealLocation()-P.ToUnrealLocation();
            J->SetNumberField(TEXT("attack_height"),ACEPluginCombat::AttackHeight(*Dat,Obj,P));
            J->SetArrayField(TEXT("preferred_elements"),ACEPluginCombat::PreferredElements(Obj.Name));
            J->SetNumberField(TEXT("angle"),FMath::Abs(FMath::FindDeltaAngleDegrees(N(Out,TEXT("heading")),double(Delta.Rotation().Yaw))));
            if(C->GetSelectedObject().Guid==Obj.Guid&&C->GetSelectedObject().bShowHealth&&C->GetSelectedObject().HealthFraction<=0)continue;
            if(C->GetSelectedObject().Guid==Obj.Guid&&C->GetSelectedObject().bShowHealth)J->SetNumberField(TEXT("health_fraction"),C->GetSelectedObject().HealthFraction);
            auto Resist=MakeShared<FJsonObject>();const auto* A=Appraisals.Find(Obj.Guid);
            J->SetBoolField(TEXT("identified"),A&&A->bSuccess);
            // A failed background assessment has already answered. Use the
            // ordinary neutral-resistance fallback, without asking twice at
            // handoff; an unsuccessful assessment may be retried after 30s.
            const double* Requested=AppraisalRequests.Find(Obj.Guid);
            J->SetBoolField(TEXT("appraisal_recent"),A&&Requested&&Now-*Requested<30);
            if(A&&A->bSuccess)
            {
                J->SetNumberField(TEXT("creature_type"),A->CreatureType);
                J->SetNumberField(TEXT("max_health"),A->MaxHealth);
                if(A->bHasCreatureHealth&&A->MaxHealth>0)
                {J->SetNumberField(TEXT("health"),A->Health);J->SetNumberField(TEXT("health_fraction"),double(A->Health)/A->MaxHealth);J->SetNumberField(TEXT("health_observed_at"),TargetAppraisalTimes.FindRef(Obj.Guid));}
                const int Props[]={64,65,66,67,68,69,70,166};
                for(int I=0;I<8;++I)if(const double* R=A->FloatProperties.Find(Props[I]))Resist->SetNumberField(FString::FromInt(I==7?1024:1<<I),*R);
            }
            if(const auto* Health=TargetHealthSamples.Find(Obj.Guid);Health&&(!A||!A->bSuccess||Health->ReceivedAt>TargetAppraisalTimes.FindRef(Obj.Guid)))
            {J->RemoveField(TEXT("health"));J->SetNumberField(TEXT("health_fraction"),Health->Fraction);J->SetNumberField(TEXT("health_observed_at"),Health->ReceivedAt);}
            J->SetObjectField(TEXT("resists"),Resist);Targets.Add(MakeShared<FJsonValueObject>(J));
        }
        else if(Obj.IsCorpse())
        {
            const auto* Info=Appraisals.Find(Obj.Guid);J->SetBoolField(TEXT("identified"),Info&&Info->bSuccess);
            J->SetBoolField(TEXT("looted"),LootedCorpses.Contains(Obj.Guid));
            J->SetBoolField(TEXT("ownership_available"),true);J->SetNumberField(TEXT("observed_age"),FMath::Max(0.,Now-CorpseFirstSeen.FindRef(Obj.Guid)));
            if(Info&&Info->bSuccess)
            {
                const FString Description=Info->StringProperties.FindRef(16);int32 End=INDEX_NONE;
                if(Description.StartsWith(TEXT("Killed by "))&&Description.FindChar('.',End)&&End>10)
                {
                    FString Killer=Description.Mid(10,End-10);
                    const FString NormalizedKiller=CorpseOwnerName(Killer);
                    auto Matches=[&](const FString& Name)
                    {
                        const FString Owner=CorpseOwnerName(Name);
                        return !Owner.IsEmpty()&&(NormalizedKiller.Equals(Owner,ESearchCase::IgnoreCase)||NormalizedKiller.StartsWith(Owner+TEXT("'s "),ESearchCase::IgnoreCase));
                    };
                    const FString PlayerName=S(CharStrings,TEXT("1"));
                    if(Matches(PlayerName)||Matches(PlayerObject.Name))Killer=PlayerName.IsEmpty()?PlayerObject.Name:PlayerName;
                    else for(const auto& M:Fellowship.Members)if(Matches(M.Name)){Killer=M.Name;break;}
                    J->SetStringField(TEXT("killer"),Killer);J->SetBoolField(TEXT("rare"),Description.Contains(TEXT("generated"),ESearchCase::IgnoreCase));
                }
                else {J->SetStringField(TEXT("killer"),TEXT(""));J->SetBoolField(TEXT("rare"),true);}
            }
            Corpses.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    Out->SetBoolField(TEXT("owned_pet"),OwnedPet);
    auto Sort=[](const auto& A,const auto& B){return N(A->AsObject(),TEXT("distance"))<N(B->AsObject(),TEXT("distance"));};
    Targets.Sort(Sort);Corpses.Sort(Sort);RouteObjects.Sort(Sort);
    Contents.Sort([](const auto& A,const auto& B){return N(A->AsObject(),TEXT("id"))<N(B->AsObject(),TEXT("id"));});
    bool Ready=true;
    if(C->GetOpenExternalContainerGuid())
    {
        const auto* Expected=Session->GetContainerContents(C->GetOpenExternalContainerGuid());
        Ready=Expected!=nullptr;
        if(Expected)for(const auto& Ref:*Expected)
        {
            FACEWorldObject Item;
            if(!C->GetWorldObject(Ref.ItemGuid,Item)||Item.Name.IsEmpty()||!Item.WeenieClassId){Ready=false;break;}
        }
    }
    Out->SetBoolField(TEXT("contents_ready"),Ready);
    if(Targets.Num()>128)Targets.SetNum(128);if(Corpses.Num()>64)Corpses.SetNum(64);if(RouteObjects.Num()>256)RouteObjects.SetNum(256);
    for(const auto& Value:Targets)Value->AsObject()->SetBoolField(TEXT("has_shield"),ArmoredParents.Contains(int32(uint32(N(Value->AsObject(),TEXT("id"))))));
    if(UWorld* World=GetWorld())
    {
        auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
        FACEPluginSightQuery Sight(*World,PC?PC->GetPawn():nullptr);
        const FVector Eye=P.ToUnrealLocation()+FVector(0,0,120);
        // Only the optional idle helper needs nearby player rays. Bound this
        // independently of crowd size; RouteObjects is already distance sorted.
        if(auto UCM=PolicyPlugin();UCM&&UCM->Running&&B(UCMServicesProfile?UCMServicesProfile:UCM->Profile,TEXT("random_helper_buffs")))
        {
            int32 Checked=0;
            for(const auto& Value:RouteObjects)
            {
                auto J=Value->AsObject();if(N(J,TEXT("distance"))>18)break;
                if(N(J,TEXT("object_class"))!=24||N(J,TEXT("id"))==uint32(C->GetPlayerGuid()))continue;
                FACEWorldObject Obj;J->SetBoolField(TEXT("line_of_sight"),C->GetWorldObject(int32(uint32(N(J,TEXT("id")))),Obj)&&Sight.Clear(Eye,SightPoint(Dat,Obj,Sight)));
                if(++Checked>=32)break;
            }
        }
        Out->SetNumberField(TEXT("movement_blocked_serial"),MovementBlockedSerial);
        if(auto UCM=PolicyPlugin();UCM&&UCM->Running&&bRouteJoinRequested)
        {
            const TArray<TSharedPtr<FJsonValue>>* Route=nullptr;
            if((RuntimeRoute?RuntimeRoute:UCM->Profile)->TryGetArrayField(TEXT("route"),Route))
                UpdateRouteVisibility(P,*Route,Sight,Out);
        }
        for(const auto& Value:Members)
        {
            const auto J=Value->AsObject();FACEWorldObject Obj;
            if(C->bPluginFellowshipUpdates && N(J,TEXT("vitals_age"))<10 && N(J,TEXT("id"))!=uint32(C->GetPlayerGuid()) && C->GetWorldObject(int32(uint32(N(J,TEXT("id")))),Obj) && Obj.IsSelectableWorldObject())
            {
                J->SetNumberField(TEXT("distance"),FVector::Distance(P.ToUnrealLocation(),Obj.Position.ToUnrealLocation())/100.);
                J->SetBoolField(TEXT("line_of_sight"),Sight.Clear(Eye,SightPoint(Dat,Obj,Sight)));
            }
        }
        for(auto* List:{&Targets,&Corpses})for(const auto& Value:*List)
        {
            auto J=Value->AsObject();FACEWorldObject Obj;
            const bool Clear=C->GetWorldObject(int32(uint32(N(J,TEXT("id")))),Obj)
                &&(Obj.IsCorpse()?Sight.Clear(Eye,SightPoint(Dat,Obj,Sight)):CombatSight(C,Dat,P,Obj,Sight));
            J->SetBoolField(TEXT("line_of_sight"),Clear);
        }
    }
    Out->SetArrayField(TEXT("targets"),Targets);Out->SetArrayField(TEXT("corpses"),Corpses);Out->SetArrayField(TEXT("contents"),Contents);Out->SetArrayField(TEXT("route_objects"),RouteObjects);
    ValidateItemBuffTimers();Out->SetArrayField(TEXT("item_buffs"),ItemBuffs);
    Debuffs.RemoveAll([&](const auto& E){return N(E->AsObject(),TEXT("expires"))<=Now||!Session->GetWorldObjects().Contains(int32(uint32(N(E->AsObject(),TEXT("target")))));});Out->SetArrayField(TEXT("debuffs"),Debuffs);
    PendingDebuffCasts.RemoveAll([Now](const auto& E){return Now-N(E->AsObject(),TEXT("sent"))>30;});
    Out->SetNumberField(TEXT("debuff_revision"),DebuffRevision);Out->SetNumberField(TEXT("debuff_target"),uint32(LastDebuffTarget));Out->SetNumberField(TEXT("debuff_spell"),LastDebuffSpell);
}
bool UACEPluginSubsystem::HasClearCombatSight(const FACEWorldObject& Target) const
{
    auto* World=GetWorld();if(!World)return false;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    FACEPosition P=C->GetPlayerPosition();if(PC)PC->TryGetLocallyPredictedPosition(P);
    FACEPluginSightQuery Sight(*World,PC?PC->GetPawn():nullptr);
    return CombatSight(C,GetGameInstance()->GetSubsystem<UACEDatSubsystem>(),P,Target,Sight);
}
bool UACEPluginSubsystem::PrepareCombatAction(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& I,int32 Mode)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();
    if(!Session)return false;
    // ACE cancellation ends repeats, not a swing whose damage hooks are already
    // queued. Keep both weapons equipped until the terminal AttackDone arrives.
    const bool SameAttack=S(I,TEXT("action"))==TEXT("attack")&&Session->IsCombatModeConfirmed(Mode);
    const bool Cancel=!SameAttack&&(Session->IsServerAttackInProgress()||(!PhysicalAttackOwner.IsEmpty()&&!Session->GetLastAttackError()));
    if(!Cancel&&Session->IsCombatModeConfirmed(Mode))return true;
    P.CombatTransitionIntent=MakeShared<FJsonObject>(*I);P.CombatTransitionMode=Mode;
    P.CombatTransitionStarted=FPlatformTime::Seconds();P.CombatTransitionRequested=false;
    P.WaitingAttackCancel=Cancel;P.AttackCancelRevision=Session->GetCombatEventRevision();
    if(Cancel)
    {
        Session->SendCancelAttack();PhysicalAttackOwner.Empty();PhysicalAttackTarget=0;PhysicalAttackNextSendAt=0;
        P.Status=TEXT("Waiting for the current attack to finish");
    }
    UpdateCombatTransition(P);
    return false;
}
void UACEPluginSubsystem::UpdateCombatTransition(FACEClientPlugin& P)
{
    if(!P.CombatTransitionIntent)return;
    if(CompanionReadBatch)CompanionReadBatch->Reset();
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();
    auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    const double Now=FPlatformTime::Seconds();
    bool IdleEnabled=false;P.Profile->TryGetBoolField(TEXT("mana_charges_when_off"),IdleEnabled);
    const FString Action=S(P.CombatTransitionIntent,TEXT("action"));
    const bool IdleMana=!P.Running&&P.Enabled&&P.IsPolicy()&&P.Id==IdleManaOwner&&IdleEnabled
        &&IdleManaSession.Pin()==Session&&IdleManaPlayer==C->GetPlayerGuid()
        &&(Action==TEXT("use_item")||Action==TEXT("apply_item"));
    if((!P.Running&&!IdleMana)||!Session||C->GetSessionState()!=EACESessionState::InWorld
        ||(!IdleMana&&(C->GetPlayerGuid()!=P.Player||C->GetServerName()!=P.Server))||(PC&&PC->IsWorldTransitionActive()))
    {P.CombatTransitionIntent.Reset();return;}
    if(Now-P.CombatTransitionStarted>=15)
    {
        auto I=P.CombatTransitionIntent;P.CombatTransitionIntent.Reset();P.NextAction=0;P.NextDecision=0;
        ReportActivityFailure(P,I,P.WaitingAttackCancel?TEXT("Attack cancellation was not confirmed; equipment left unchanged")
            :TEXT("Combat stance was not confirmed; action deferred"));return;
    }
    if(P.WaitingAttackCancel)
    {
        // AttackDone(0) only starts the next repeat's power bar. A terminal
        // result after our cancel is required, including between dual-wield hits.
        if(Session->GetCombatEventRevision()==P.AttackCancelRevision||Session->IsServerAttackInProgress()||!Session->GetLastAttackError())
        {ScheduleDecisionWake(Now+.05);return;}
        P.WaitingAttackCancel=false;
    }
    if(C->IsUseBusy()||PendingSpell||InventoryInteractionPending()){ScheduleDecisionWake(Now+.05);return;}
    if(!P.CombatTransitionRequested&&!Session->IsCombatModeConfirmed(P.CombatTransitionMode))
    {
        P.CombatTransitionRevision=Session->GetCombatModeRevision();P.CombatTransitionRequested=true;
        C->SendChangeCombatMode(P.CombatTransitionMode);
        P.Status=TEXT("Waiting for the server combat stance");
    }
    if((P.CombatTransitionRequested&&Session->GetCombatModeRevision()==P.CombatTransitionRevision)||!Session->IsCombatModeConfirmed(P.CombatTransitionMode))
    {ScheduleDecisionWake(Now+.05);return;}
    auto I=P.CombatTransitionIntent;P.CombatTransitionIntent.Reset();P.NextAction=0;P.NextDecision=0;
    Execute(P,I); // Revalidate the target, ownership and permissions after the wait.
    if(Action==TEXT("combat_mode")&&!P.CombatTransitionIntent)
    {P.NextDecision=0;ScheduleDecisionWake(Now);}
}
void UACEPluginSubsystem::TrackActionWait(FACEClientPlugin& P,const FString& Action,int32 Item,int32 Value)
{
    P.WaitAction=Action;P.WaitItem=Item;P.WaitValue=Value;
    P.WaitConsumedItems.Reset();
    P.WaitSerial=ActionSerial;P.ActionSentAt=FPlatformTime::Seconds();
}
void UACEPluginSubsystem::RefreshActionWait(FACEClientPlugin& P)
{
    if(P.WaitAction.IsEmpty())return;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();
    const auto* Item=Session?Session->GetWorldObjects().Find(P.WaitItem):nullptr;
    const bool Consuming=P.WaitAction==TEXT("salvage")||P.WaitAction==TEXT("combine_salvage");
    bool Complete=false;
    if(Consuming)
    {
        // ACE may suppress the salvage-result chat packet. Actual inventory
        // removals confirm completion; a visual detach or UseDone does not.
        Complete=Session.IsValid()&&!P.WaitConsumedItems.IsEmpty();
        if(Complete)for(int32 Id:P.WaitConsumedItems)if(Session->GetWorldObjects().Contains(Id)){Complete=false;break;}
    }
    else if(P.WaitAction==TEXT("loot"))
        Complete=Item&&(C->IsOwnedInventoryItem(*Item)||Item->StackSize<P.WaitValue);
    else if(P.WaitAction==TEXT("equip"))
        Complete=Item&&(P.WaitValue?Item->WielderId==C->GetPlayerGuid()&&(Item->CurrentWieldedLocation&P.WaitValue)!=0:
            C->IsOwnedInventoryItem(*Item)&&!Item->CurrentWieldedLocation);
    else if(P.WaitAction==TEXT("open_corpse"))Complete=C->GetOpenExternalContainerGuid()==P.WaitItem;
    else if(P.WaitAction==TEXT("use"))Complete=ActionSerial!=P.WaitSerial;
    if(Complete)
    {
        // Resume on the relevant server update, not the worst-case timeout.
        // Lua still confirms quantities/ownership, and the host retains busy gates.
        P.NextAction=FMath::Min(P.NextAction,Consuming?FPlatformTime::Seconds():P.ActionSentAt+.25);
        P.NextDecision=0;P.WaitAction.Empty();P.WaitConsumedItems.Reset();
        // Defer policy execution until after network dispatch; never issue a
        // new destructive action from inside an inventory-removal callback.
        if(Consuming&&P.Running)ScheduleDecisionWake(P.NextAction);
    }
    else if(FPlatformTime::Seconds()>=P.NextAction)
    {
        // Keep ownership of an ongoing corpse approach beyond the initial
        // throttle so combat can interrupt it even on a long walk.
        auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
        if(P.WaitAction!=TEXT("open_corpse")||UseApproachOwner!=P.Id||!PC||!PC->IsUseApproachActive())P.WaitAction.Empty();
    }
}
bool UACEPluginSubsystem::ExecuteInventory(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& I,const FString& Action)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();const double Now=FPlatformTime::Seconds();
    const double Raw=N(I,TEXT("item"));const int32 Id=Raw>0&&Raw<=MAX_uint32&&FMath::FloorToDouble(Raw)==Raw?int32(uint32(Raw)):0;
    FACEWorldObject Item;const bool Found=C->GetWorldObject(Id,Item),Owned=Found&&C->IsOwnedInventoryItem(Item);
    if(Action==TEXT("split_note")||Action==TEXT("sell_note"))
    {
        auto Session=C->GetSession();const int32 Vendor=C->GetOpenVendorGuid();const double Amount=N(I,TEXT("count"));
        if(!Session||!Vendor||N(I,TEXT("vendor"))!=uint32(Vendor)||!Session->VendorUsesPyreals()
            || !Owned||!LootItemAvailable(C,Item)||Item.ItemType!=ACEItemType::PromissoryNote||Item.Value<=0
            || !Session->CanVendorBuyItem(Item)||Amount<1||Amount>FMath::Max(1,Item.StackSize)||FMath::FloorToDouble(Amount)!=Amount)
        {ReportActivityFailure(P,I,TEXT("Trade note or vendor changed; restocking stopped"));return true;}
        if(Action==TEXT("sell_note"))
        {
            if(Amount!=FMath::Max(1,Item.StackSize)){ReportActivityFailure(P,I,TEXT("Trade note split is not confirmed; nothing sold"));return true;}
            C->SendSellItems(Vendor,{{int32(Amount),Id}});return true;
        }
        if(Amount>=FMath::Max(1,Item.StackSize)){ReportActivityFailure(P,I,TEXT("Trade note stack changed; nothing split"));return true;}
        int32 Destination=0;
        auto TryPack=[&](int32 PackId)
        {
            FACEWorldObject Pack;if(Destination||!C->GetWorldObject(PackId,Pack))return;
            const int32 Capacity=Pack.ItemsCapacity>0?Pack.ItemsCapacity:(PackId==C->GetPlayerGuid()?102:0);
            if(Capacity>C->GetPackItems(PackId).Num())Destination=PackId;
        };
        TryPack(Item.ContainerId);TryPack(C->GetPlayerGuid());for(const auto& Pack:C->GetPlayerPacks())TryPack(Pack.Guid);
        if(!Destination){ReportActivityFailure(P,I,TEXT("Free an inventory slot to split trade notes for restocking"));return true;}
        C->SendStackableSplitToContainer(Id,Destination,0,int32(Amount));return true;
    }
    if(Action==TEXT("buy"))
    {
        const int32 Vendor=C->GetOpenVendorGuid();const double Amount=N(I,TEXT("count"));
        if(!Vendor||N(I,TEXT("vendor"))!=uint32(Vendor)||Amount<1||Amount>100000||FMath::FloorToDouble(Amount)!=Amount)
        {ReportActivityFailure(P,I,TEXT("Vendor changed or purchase quantity is invalid"));return true;}
        for(const auto& Stock:C->GetVendorMerchandise())if(Stock.Guid==Id)
        {
            if(Amount>ACEInventoryRules::VendorPurchaseLimit(Stock)){ReportActivityFailure(P,I,TEXT("Requested vendor stock is no longer available"));return true;}
            C->SendBuyItems(Vendor,{{int32(Amount),Id}});return true;
        }
        ReportActivityFailure(P,I,TEXT("Purchase item is not in the open vendor's stock"));return true;
    }
    if(Action==TEXT("combine_salvage"))
    {
        const double ToolId=N(I,TEXT("tool"));FACEWorldObject Tool;
        if(ToolId<=0||ToolId>MAX_uint32||FMath::FloorToDouble(ToolId)!=ToolId||!C->GetWorldObject(int32(uint32(ToolId)),Tool)||!SalvageToolAvailable(C,Tool)){ReportActivityFailure(P,I,TEXT("Salvage combining requires an available Ust"));return true;}
        TArray<FString> Parts;S(I,TEXT("items")).ParseIntoArray(Parts,TEXT(","));TArray<int32> Items;int32 Material=0;
        if(Parts.Num()<2||Parts.Num()>64){ReportActivityFailure(P,I,TEXT("Invalid salvage combine list"));return true;}
        for(const FString& Part:Parts)
        {
            uint32 BagId=0;FACEWorldObject Bag;
            if(!LexTryParseString(BagId,*Part)||!BagId||Items.Contains(int32(BagId))||!C->GetWorldObject(int32(BagId),Bag)||!LootItemAvailable(C,Bag)||ACEVTObjectClass::Classify(Bag)!=39||Bag.Structure<=0||Bag.Structure>=100||Bag.MaterialType<=0||(Material&&Material!=Bag.MaterialType)){ReportActivityFailure(P,I,TEXT("Salvage bags changed or are unavailable; no bags combined"));return true;}
            Material=Bag.MaterialType;Items.Add(int32(BagId));
        }
        if(!PrepareCombatAction(P,I,1))return true;
        TrackActionWait(P,Action,0);P.WaitConsumedItems=Items;
        C->SendCreateTinkeringTool(Tool.Guid,Items);P.NextAction=Now+3;return true;
    }
    if(Action==TEXT("identify"))
    {
        if(Found)
        {
            // Resistance appraisal is optional; combat has a neutral fallback.
            // Inventory/corpse appraisal still gates rules and destructive uses.
            const bool CombatAssessment=P.IsPolicy()&&S(I,TEXT("activity"))==TEXT("combat")
                &&Item.IsAttackable()&&!Item.bIsPlayer&&!Item.PetOwnerId;
            if(CombatAssessment)P.NextAction=Now;
            else TrackActionWait(P,Action,Id);
            // Share the request ledger with the background inventory scanner so
            // it cannot immediately duplicate a policy-requested appraisal.
            LastAppraisalRequest=Now;AppraisalRequests.Add(Id,Now);C->RequestBackgroundAppraisal(Id);
        }
        return true;
    }
    if(Action==TEXT("equip"))
    {
        bool Unequip=false,Offhand=false;I->TryGetBoolField(TEXT("unequip"),Unequip);I->TryGetBoolField(TEXT("offhand"),Offhand);
        if(Unequip)
        {
            if(Found&&IsOwnedPluginItem(Item)&&Item.CurrentWieldedLocation)
            {
                const int32 Destination=C->FindPickupContainer(Id,0);
                if(!Destination){ReportActivityFailure(P,I,TEXT("No pack has room to put away this weapon"));return true;}
                if(!PrepareCombatAction(P,I,1))return true;
                TrackActionWait(P,Action,Id,0);C->SendPutItemInContainer(Id,Destination,0);P.NextAction=Now+2;
            }
            return true;
        }
        const auto* A=Found?GetEquipmentAppraisal(Item):nullptr;
        if(!Found||!IsOwnedPluginItem(Item)||!A||!A->bSuccess){ReportActivityFailure(P,I,TEXT("Equipment is no longer owned or its appraisal is unavailable"));return true;}
        const FString WieldFailure=ACEPluginEquipment::WieldFailure(*A,C->GetPlayerVitalsView());
        if(!WieldFailure.IsEmpty()){ReportActivityFailure(P,I,ACERetailObjectNames::Name(Item)+TEXT(": ")+WieldFailure);return true;}
        // Ordinary equip honors the same automatic hand preference as retail
        // ItemHolder::DetermineUse. It is live object data, not an appraisal hint.
        if((Item.ObjectDescriptionFlags&ACEObjectDescFlag::WieldLeft)!=0)Offhand=true;
        int64 Mask=Item.ItemType&ACEItemType::Caster?0x1000000:Item.ItemType&ACEItemType::MissileWeapon?0x400000:Item.ItemType&ACEItemType::MeleeWeapon?0x100000:Item.ValidLocations;
        if(Item.ValidLocations&0x2000000)Mask=0x2000000;
        if(Item.ValidLocations&0x800000)Mask=0x800000;
        if(Offhand)
        {
            // A melee weapon's normal location is the primary hand; dual wield
            // explicitly uses ShieldLoc. Never put a two-handed weapon there.
            if(!ACEEquipmentRules::CanWieldInSlot(Item,ACEEquipMask::Shield))return true;
            Mask=0x200000;
        }
        Mask &= Item.ValidLocations;
        if(Offhand&&(Item.ItemType&ACEItemType::MeleeWeapon)&&!(Item.ValidLocations&0x2000000))Mask=0x200000;
        if(Mask){if(!PrepareCombatAction(P,I,1))return true;TrackActionWait(P,Action,Id,int32(Mask));C->SendGetAndWieldItem(Id,Mask);P.NextAction=Now+2;}
        return true;
    }
    if(Action==TEXT("salvage")||Action==TEXT("sell")||Action==TEXT("read"))
    {
        if(!Owned||!LootItemAvailable(C,Item))
        {ReportActivityFailure(P,I,TEXT("Loot action requires an unequipped, unretained owned item outside trade"));return true;}
        if(Action==TEXT("read"))
        {
            if(ACEVTObjectClass::Classify(Item)!=42){ReportActivityFailure(P,I,TEXT("Read rule requires a spell scroll"));return true;}
            if(!PrepareCombatAction(P,I,1))return true;
            TrackActionWait(P,TEXT("use"),Id);C->SendUseItem(Id);P.NextAction=Now+3;return true;
        }
        if(Action==TEXT("sell"))
        {
            const int32 Vendor=C->GetOpenVendorGuid();if(!Vendor){ReportActivityFailure(P,I,TEXT("A vendor must be open to sell loot"));return true;}
            C->SendSellItems(Vendor,{TPair<int32,int32>(FMath::Max(1,Item.StackSize),Id)});P.NextAction=Now+2;return true;
        }
        const double ToolId=N(I,TEXT("tool"));FACEWorldObject Tool;
        const auto* SalvageAppraisal=Appraisals.Find(Id);
        if(!SalvageAppraisal||!SalvageAppraisal->bSuccess||SalvageAppraisal->IntProperties.FindRef(171)>0||!SalvageAppraisal->StringProperties.FindRef(8).IsEmpty())
        {ReportActivityFailure(P,I,TEXT("Automatic salvage requires appraisal and skips tinkered or inscribed items"));return true;}
        if(!SalvageAppraisal->IntProperties.Contains(105) && Item.SalvageWorkmanship<0)
        {ReportActivityFailure(P,I,TEXT("Item has no workmanship and cannot be salvaged"));return true;}
        if(ToolId<=0||ToolId>MAX_uint32||FMath::FloorToDouble(ToolId)!=ToolId||!C->GetWorldObject(int32(uint32(ToolId)),Tool)||!SalvageToolAvailable(C,Tool)||Item.Guid==Tool.Guid||Item.ItemsCapacity>0||(ACEVTObjectClass::Classify(Item)==39&&Item.Structure>=100)||Item.MaterialType<1||Item.MaterialType>77||TArray<int>{3,9,56,65,72}.Contains(Item.MaterialType))
        {ReportActivityFailure(P,I,TEXT("Item is not salvageable or no owned Ust is available"));return true;}
        if(!PrepareCombatAction(P,I,1))return true;
        TrackActionWait(P,Action,Id);P.WaitConsumedItems={Id};
        C->SendCreateTinkeringTool(Tool.Guid,{Id});P.NextAction=Now+3;return true;
    }
    if(Action==TEXT("store_item"))
    {
        const double TargetId=N(I,TEXT("target"));FACEWorldObject Bag;
        if(Owned&&TargetId>0&&TargetId<=MAX_uint32&&FMath::FloorToDouble(TargetId)==TargetId&&C->GetWorldObject(int32(uint32(TargetId)),Bag)&&C->IsOwnedInventoryItem(Bag)&&Bag.ItemsCapacity>C->GetPackItems(Bag.Guid).Num()&&!(Item.ItemType&ACEItemType::Container)&&!(Item.ObjectDescriptionFlags&ACEObjectDescFlag::RequiresPackSlot))
        {C->SendPutItemInContainer(Id,Bag.Guid,0);P.NextAction=Now+1;}else ReportActivityFailure(P,I,TEXT("AutoCram destination is unavailable or full"));return true;
    }
    if(Action==TEXT("give"))
    {
        const double TargetId=N(I,TEXT("target"));FACEWorldObject Target;
        if(!Owned||TargetId<=0||TargetId>MAX_uint32||FMath::FloorToDouble(TargetId)!=TargetId||!C->GetWorldObject(int32(uint32(TargetId)),Target)||!Target.IsSelectableWorldObject()||!Target.IsGiveOrCreatureTarget())
        {ReportActivityFailure(P,I,TEXT("Meta give requires an owned item and a valid recipient"));return true;}
        const int Amount=int(FMath::Clamp(N(I,TEXT("amount"),1),1.,double(FMath::Max(1,Item.StackSize))));
        if(!PrepareCombatAction(P,I,1))return true;
        C->SendGiveObjectRequest(Target.Guid,Id,Amount);P.NextAction=Now+3;return true;
    }
    if(Action==TEXT("apply_item"))
    {
        const double TargetValue=N(I,TEXT("target"));FACEWorldObject Target;
        if(Owned&&TargetValue>0&&TargetValue<=MAX_uint32&&FMath::FloorToDouble(TargetValue)==TargetValue&&C->GetWorldObject(int32(uint32(TargetValue)),Target)
            &&(IsOwnedPluginItem(Target)||Target.IsSelectableWorldObject())&&ACEInventoryRules::IsUsable(Item.ItemUseable))
        {
            if((Item.ObjectDescriptionFlags&0x20000)&&(!LootItemAvailable(C,Item,true)||Item.Structure<=0))
            {ReportActivityFailure(P,I,TEXT("Lockpick is unavailable or exhausted"));return true;}
            if (Item.ItemType & ACEItemType::ManaStone)
            {
                if(!LootItemAvailable(C,Item,true)) { ReportActivityFailure(P,I,TEXT("Mana supply is unavailable or offered in trade")); return true; }
                const auto* Stone=Appraisals.Find(Id); const auto* Source=Appraisals.Find(Target.Guid);
                if (!Stone || !Stone->bSuccess) { ReportActivityFailure(P,I,TEXT("Inspect the mana stone before using it")); return true; }
                const bool Empty=(Item.ItemCurrentMana!=-1?Item.ItemCurrentMana:Stone->IntProperties.FindRef(107))<=0;
                if (Empty && (!LootItemAvailable(C,Target) || !Source || !Source->bSuccess
                    || Source->IntProperties.FindRef(107)<=0 || Source->IntProperties.FindRef(108)<=0
                    || Target.Guid==Id || (Target.ItemType&(ACEItemType::ManaStone|ACEItemType::Container))))
                { ReportActivityFailure(P,I,TEXT("Mana source must be an unequipped, unretained owned item with mana, outside trade")); return true; }
                PendingManaRefresh=true;
            }
            PendingResourceRefresh.Add(Id);PendingResourceRefresh.Add(Target.Guid);
            if(!PrepareCombatAction(P,I,1))return true;TrackActionWait(P,TEXT("use"));C->SendUseWithTarget(Id,Target.Guid);P.NextAction=Now+3;
        }
        else ReportActivityFailure(P,I,TEXT("Item or target is unavailable for use"));
        return true;
    }
    if(Action==TEXT("cast_item"))
    {
        const auto Snapshot=Found?PluginItemSnapshot(Item)->AsObject():nullptr;
        const bool Equipped=Found&&(Item.WielderId==C->GetPlayerGuid()||(Item.CurrentWieldedLocation&&Item.ParentGuid==C->GetPlayerGuid()));
        bool Available=false;const TSharedPtr<FJsonObject>* Spell=nullptr;
        if(!P.Permissions.Contains(TEXT("inventory"))||!Equipped||!Snapshot
            ||!Snapshot->TryGetBoolField(TEXT("innate_spell_available"),Available)||!Available
            ||!Snapshot->TryGetObjectField(TEXT("innate_spell"),Spell)||N(I,TEXT("spell"))!=Item.SpellDID)
        {ReportActivityFailure(P,I,TEXT("Casting implement or its activation requirements changed"));return true;}
        const double RawTarget=N(I,TEXT("target"));const int32 Target=RawTarget>0&&RawTarget<=MAX_uint32&&FMath::FloorToDouble(RawTarget)==RawTarget?int32(uint32(RawTarget)):0;FACEWorldObject Monster;
        const bool Self=Target==C->GetPlayerGuid()&&B(*Spell,TEXT("beneficial"))&&B(*Spell,TEXT("caster_target"))&&N(*Spell,TEXT("duration"))==0;
        const bool Debuff=!B(*Spell,TEXT("beneficial"))&&!B(*Spell,TEXT("caster_target"))&&N(*Spell,TEXT("duration"))>0;
        if(!Self&&(!Debuff||!C->GetWorldObject(Target,Monster)||!Monster.IsAttackable()||Monster.bIsPlayer||Monster.PetOwnerId
            ||Monster.bDying||DefeatedTargets.Contains(Target)||!HasClearCombatSight(Monster)))
        {ReportActivityFailure(P,I,TEXT("Item spell target is unavailable or obstructed"));return true;}
        if(!PrepareCombatAction(P,I,8))return true;
        if(Debuff)ObserveTargetSpellCast(Item.SpellDID,Target);
        ConfigureCastMotion(P,I,Item.SpellDID,true);
        PendingResourceRefresh.Add(Id);TrackActionWait(P,TEXT("use"));
        C->SendUseWithTarget(Id,Target);P.NextAction=Now+3;return true;
    }
    if(Action==TEXT("use_item"))
    {
        if(Owned&&ACEInventoryRules::IsUsable(Item.ItemUseable))
        {
            if((Item.ItemType&ACEItemType::ManaStone)&&!LootItemAvailable(C,Item,true))
            {ReportActivityFailure(P,I,TEXT("Mana supply is unavailable or offered in trade"));return true;}
            PendingManaRefresh=(Item.ItemType&ACEItemType::ManaStone)!=0;
            if(PendingManaRefresh)PendingResourceRefresh.Add(Id);
            // The server's summoning cooldown quality identifies the same
            // devices accepted by UCM's pet policy, including custom essences.
            // PetDevice.ActOnUse has no peace-mode requirement.
            const auto* Appraisal=Appraisals.Find(Id);
            const bool SummoningDevice=ACEPetDevice::IsSummoningDevice(Appraisal);
            if(SummoningDevice)PendingResourceRefresh.Add(Id);
            if(!SummoningDevice&&!PrepareCombatAction(P,I,1))return true;
            TrackActionWait(P,TEXT("use"));
            if(ACEItemUseable::IsTargeted(Item.ItemUseable))C->SendUseWithTarget(Id,C->GetPlayerGuid());else C->SendUseItem(Id);
            P.NextAction=Now+3;
        }
        return true;
    }
    if(Action==TEXT("use_world")||Action==TEXT("open_corpse"))
    {
        if(Found&&Item.IsSelectableWorldObject() && (Action!=TEXT("open_corpse")||Item.IsCorpse()))
        {
            if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))
            {
                // Corpse Use/loot is valid while armed. Preserve the player's
                // stance for both the loot action and a recorded corpse Use.
                if(!Item.IsCorpse()&&!PrepareCombatAction(P,I,1))return true;
                PC->BeginUseApproach(Id,Item.UseRadius);UseApproachOwner=P.Id;
                TrackActionWait(P,Item.IsCorpse()?TEXT("open_corpse"):TEXT("use"),Id);
                P.NextAction=Now+2;
            }
        }
        return true;
    }
    if(Action==TEXT("close_corpse"))
    {
        const int32 Guid=C->GetOpenExternalContainerGuid();FACEWorldObject Corpse;
        bool Completed=false;I->TryGetBoolField(TEXT("completed"),Completed);
        if(Guid)
        {
            if(Completed&&C->GetWorldObject(Guid,Corpse)&&Corpse.IsCorpse())LootedCorpses.Add(Guid,{Corpse.Position,FPlatformTime::Seconds()});
            C->SendNoLongerViewingContents(Guid);
        }
        // Closing clears the local container immediately and uses the ordered
        // action stream. There is no use animation or completion to wait for.
        P.NextAction=Now;
        return true;
    }
    if(Action==TEXT("close_vendor"))
    {
        const int32 Vendor=C->GetOpenVendorGuid();
        if(Vendor&&N(I,TEXT("vendor"))==uint32(Vendor))C->SendNoLongerViewingContents(Vendor);
        else if(Vendor)ReportActivityFailure(P,I,TEXT("Vendor changed before closing"));
        return true;
    }
    if(Action==TEXT("loot"))
    {
        FACEWorldObject Container;
        if(Found && Item.ContainerId && Item.ContainerId==C->GetOpenExternalContainerGuid() && C->GetWorldObject(Item.ContainerId,Container) && Container.IsCorpse())
        {
            const int32 Amount=int32(FMath::Clamp(N(I,TEXT("amount"),FMath::Max(1,Item.StackSize)),1.,double(FMath::Max(1,Item.StackSize))));
            const int32 Destination=C->FindPickupContainer(Id,0,Amount);
            if(!Destination)
            {
                ReportActivityFailure(P,I,TEXT("No pack has space for this loot; waiting for inventory space"));
                if(P.ActivityFailure)P.ActivityFailure->SetBoolField(TEXT("inventory_full"),true);
                P.WaitAction.Empty();
                return true;
            }
            TrackActionWait(P,Action,Id,Item.StackSize);
            if(Amount<Item.StackSize)
            {
                // A partial pickup into a full pack must merge directly. Split
                // creates a new stack and requires a free slot on the server.
                int32 MergeTo=0;const auto Offered=C->GetTradeSelfItems();
                for(const auto& Target:C->GetPackItems(Destination))
                    if(C->IsOwnedInventoryItem(Target)&&!Offered.Contains(Target.Guid)
                        &&ACEInventoryRules::MergeAmount(Item,Target)>=Amount){MergeTo=Target.Guid;break;}
                if(MergeTo)C->SendStackableMerge(Id,MergeTo,Amount);
                else C->SendStackableSplitToContainer(Id,Destination,0,Amount);
            }
            else C->SendPutItemInContainer(Id,Destination,0);
        }
        P.NextAction=Now+1;return true;
    }
    if(Action==TEXT("merge"))
    {
        FACEWorldObject Target;const double RawTarget=N(I,TEXT("target"));
        if(RawTarget<=0||RawTarget>MAX_uint32||FMath::FloorToDouble(RawTarget)!=RawTarget)return true;
        const int32 To=int32(uint32(RawTarget));
        if(Owned&&C->GetWorldObject(To,Target)&&LootItemAvailable(C,Item)&&LootItemAvailable(C,Target))
            if(int32 Amount=ACEInventoryRules::MergeAmount(Item,Target))C->SendStackableMerge(Id,To,Amount);
        return true;
    }
    if(Action==TEXT("jump"))
    {
        if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))
            PC->QueuePluginJump(FMath::Clamp(float(N(I,TEXT("charge"),.5)),0.f,1.f),(I->HasTypedField<EJson::Boolean>(TEXT("current_heading"))&&I->GetBoolField(TEXT("current_heading"))&&PC->GetPawn())?PC->GetPawn()->GetActorRotation().Yaw:float(N(I,TEXT("heading"))),N(I,TEXT("forward"),1),N(I,TEXT("strafe")),I->HasTypedField<EJson::Boolean>(TEXT("walk_jump"))&&I->GetBoolField(TEXT("walk_jump")));
        P.NextAction=Now+2;return true;
    }
    return false;
}
void UACEPluginSubsystem::RecordRouteAction(const FString& Type,float Charge)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Obj;
    if((Type==TEXT("portal")||Type==TEXT("use")) && (!C->GetWorldObject(C->GetSelectedObject().Guid,Obj)||!Obj.IsSelectableWorldObject()))
    {Notice=TEXT("Select a portal, door or device before recording Use");return;}
    auto Point=MakeShared<FJsonObject>();Point->SetStringField(TEXT("kind"),Type);
    if(Type==TEXT("portal")||Type==TEXT("use"))
    {
        Point->SetNumberField(TEXT("wcid"),Obj.WeenieClassId);Point->SetStringField(TEXT("object_name"),Obj.Name);
    }
    Point->SetNumberField(TEXT("charge"),Charge);
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))Point->SetNumberField(TEXT("heading"),PC->GetPawn()?PC->GetPawn()->GetActorRotation().Yaw:0);
    RecordWaypoint(TEXT("ucm"),Point);
}
void UACEPluginSubsystem::DrawRoute()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginRoute);
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto P=PolicyPlugin();bool Show=true;
    if(P)P->Profile->TryGetBoolField(TEXT("route_preview"),Show);
    if(!P||!P->Enabled||!Show||C->GetSessionState()!=EACESessionState::InWorld)
    {if(RouteMesh&&RouteMesh->GetNumSections())RouteMesh->ClearAllMeshSections();RouteSegments.Reset();RouteRebuiltAt=0;return;}
    const TArray<TSharedPtr<FJsonValue>>* Points=nullptr;
    if(!(RuntimeRoute?RuntimeRoute:P->Profile)->TryGetArrayField(TEXT("route"),Points)||Points->IsEmpty())
    {if(RouteMesh&&RouteMesh->GetNumSections())RouteMesh->ClearAllMeshSections();RouteSegments.Reset();RouteRebuiltAt=0;return;}
    // Edits explicitly invalidate RouteRebuiltAt. Player movement and waypoint
    // changes need at most a 10 Hz overlay check, not a full JSON hash at 144 Hz.
    const double Now=FPlatformTime::Seconds();
    if(RouteRebuiltAt>0&&Now<NextRouteCheck)return;
    NextRouteCheck=Now+.1;
    UWorld* W=GetWorld();if(!W)return;
    if(!RouteMesh||RouteMesh->GetWorld()!=W)
    {
        if(RouteMesh)RouteMesh->DestroyComponent();if(RouteActor)RouteActor->Destroy();
        RouteActor=W->SpawnActor<AActor>();if(!RouteActor)return;
        RouteMesh=NewObject<UProceduralMeshComponent>(RouteActor);RouteActor->SetRootComponent(RouteMesh);
        RouteMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);RouteMesh->SetCastShadow(false);
        RouteMesh->SetMaterial(0,GetGameInstance()->GetSubsystem<UACEDatSubsystem>()->GetVertexColorMaterial());
        RouteMesh->SetCanEverAffectNavigation(false);
        RouteMesh->RegisterComponentWithWorld(W);RouteRebuiltAt=0;
    }
    const auto Player=C->GetPlayerPosition();const FVector PlayerWorld=Player.ToUnrealLocation();
    bool Loop=false,Reverse=false;const auto Route=RuntimeRoute?RuntimeRoute:P->Profile;
    Route->TryGetBoolField(TEXT("loop_route"),Loop);Route->TryGetBoolField(TEXT("reverse_route"),Reverse);
    // Outdoor cell indices and same-landblock room changes do not move route
    // geometry. Only the landblock/rebasing mode affects the coordinate frame.
    const uint32 Area=(uint32(Player.CellId)&0xffff0000u)|((uint32(Player.CellId)&0xffffu)>=0x100?0x100u:0u);
    uint32 Signature=HashCombine(GetTypeHash(RoutePoint),GetTypeHash(Area));
    Signature=HashCombine(Signature,GetTypeHash(Loop&&!Reverse));
    Signature=HashCombine(Signature,GetTypeHash(FIntVector(FMath::FloorToInt(PlayerWorld.X/5000),FMath::FloorToInt(PlayerWorld.Y/5000),0)));
    for(const auto& Value:*Points)if(auto O=Value->AsObject())
    {for(const TCHAR* Key:{TEXT("cell"),TEXT("x"),TEXT("y"),TEXT("z")})Signature=HashCombine(Signature,GetTypeHash(N(O,Key)));Signature=HashCombine(Signature,GetTypeHash(S(O,TEXT("kind"))));}
    // Persistent geometry; edits/waypoint changes redraw immediately. Periodic
    // reprojection accommodates freshly streamed terrain without per-frame traces.
    if(RouteRebuiltAt>0&&Signature==RouteSignature&&Now-RouteRebuiltAt<3)return;
    RouteRebuiltAt=Now;RouteSignature=Signature;RouteSegments.Reset();
    FACEPluginRouteGround Floor(*W);
    auto Ground=[&](const FVector& V)
    {
        FVector Hit;
        if(Floor.Sample(V,Hit))return Hit+FVector(0,0,10);
        return V+FVector(0,0,10);
    };
    auto& Lines=RouteSegments;
    auto Line=[&](const FVector& A,const FVector& B,FLinearColor Color,float Width=10)
    {Lines.Add({A,B,Color,Width});};
    auto Segment=[&](const FACEPosition& From,const FACEPosition& To,FLinearColor Color)
    {
        const bool Connected=(uint32(From.CellId)>>16)==(uint32(To.CellId)>>16)||((uint32(From.CellId)&0xffff)<0x100&&(uint32(To.CellId)&0xffff)<0x100);
        const FVector A=From.ToUnrealLocation(),B=To.ToUnrealLocation();const double Length=FVector::Distance(A,B);
        if(!Connected||Length>=40000)return;
        TArray<FVector> Path;
        if(!Floor.Path(A,B,Path))return; // no false line through voids or another floor
        for(int32 I=1;I<Path.Num()&&Lines.Num()<8192;++I)
            Line(Path[I-1]+FVector(0,0,10),Path[I]+FVector(0,0,10),Color);
    };
    FACEPosition Prev,First;bool Have=false,FirstVisible=false;int32 Index=0;
    for(const auto& Value:*Points)
    {
        ++Index;const auto O=Value->AsObject();if(!O){Have=false;continue;}
        FACEPosition Next;Next.CellId=int32(uint32(N(O,TEXT("cell"))));Next.Location=FVector(N(O,TEXT("x")),N(O,TEXT("y")),N(O,TEXT("z")));
        // NAV has global coordinates, not env-cell IDs. Apply the same dungeon
        // rebasing as Execute(move), including points beyond a nominal LB edge.
        bool Legacy=false;O->TryGetBoolField(TEXT("legacy"),Legacy);
        if(Legacy && (uint32(Player.CellId)&0xffff)>=0x100)
        {
            const uint32 From=uint32(Next.CellId)>>16,To=uint32(Player.CellId)>>16;
            Next.Location.X+=(int32(From>>8)-int32(To>>8))*192;
            Next.Location.Y+=(int32(From&255)-int32(To&255))*192;
            Next.CellId=Player.CellId;
        }
        const bool SameArea=(uint32(Next.CellId)>>16)==(uint32(Player.CellId)>>16)||((uint32(Next.CellId)&0xffff)<0x100&&(uint32(Player.CellId)&0xffff)<0x100);
        const FVector Position=Next.ToUnrealLocation();
        const bool Visible=SameArea&&FVector::DistSquared(Position,PlayerWorld)<FMath::Square(40000.);
        if(Index==1){First=Next;FirstVisible=Visible;}
        if(Visible)
        {
            const FLinearColor Color=S(O,TEXT("kind"))==TEXT("jump")?FLinearColor(1,.7f,.15f):FLinearColor(.1f,.75f,1);
            const FVector Center=Ground(Position);
            if(Index==RoutePoint)
            {
                constexpr int32 Sides=32;FVector Last;
                for(int32 I=0;I<=Sides;++I)
                {
                    const float Angle=2*PI*I/Sides;
                    const FVector V=Ground(Position+FVector(FMath::Cos(Angle)*80,FMath::Sin(Angle)*80,0));
                    if(I)Line(Last,V,FLinearColor(.2f,1,.35f),8);Last=V;
                }
            }
            else if(Lines.Num()+24<8192)
            {
                // Three great circles read as a small sphere without the full
                // latitude/longitude debug-sphere cost at every waypoint.
                for(int32 Axis=0;Axis<3;++Axis)for(int32 I=0;I<8;++I)
                {
                    auto Vertex=[&](int32 Side){const float Angle=2*PI*Side/8;FVector V=FVector::ZeroVector;V[(Axis+1)%3]=12*FMath::Cos(Angle);V[(Axis+2)%3]=12*FMath::Sin(Angle);return Center+FVector(0,0,12)+V;};
                    Line(Vertex(I),Vertex(I+1),Color,2);
                }
            }
            if(Have)Segment(Prev,Next,Color);
        }
        Prev=Next;Have=Visible&&S(O,TEXT("kind"))!=TEXT("portal")&&S(O,TEXT("kind"))!=TEXT("recall");
    }
    if(Loop&&!Reverse&&Points->Num()>1&&Have&&FirstVisible)Segment(Prev,First,FLinearColor(.1f,.75f,1));
    // Game geometry, not the engine's editor/debug PDI. One unlit mesh is
    // visible in packaged builds, stereo eyes and scene captures alike.
    // Local vertices avoid precision loss at distant world coordinates.
    TArray<FVector> Vertices,Normals;TArray<int32> Triangles;TArray<FLinearColor> Colors;
    Vertices.Reserve(Lines.Num()*8);Colors.Reserve(Lines.Num()*8);Triangles.Reserve(Lines.Num()*24);
    for(const auto& L:Lines)
    {
        const FVector Direction=(L.End-L.Start).GetSafeNormal();if(Direction.IsNearlyZero())continue;
        const FVector Side=FVector::CrossProduct(Direction,FMath::Abs(Direction.Z)>.9?FVector::RightVector:FVector::UpVector).GetSafeNormal();
        const FVector Up=FVector::CrossProduct(Direction,Side);const int32 Base=Vertices.Num();
        for(const FVector End:{L.Start,L.End})for(int32 Corner=0;Corner<4;++Corner)
        {
            const FVector Normal=Side*FMath::Cos(PI*.5f*Corner)+Up*FMath::Sin(PI*.5f*Corner);
            Vertices.Add(End-PlayerWorld+Normal*L.Thickness*.5f);Normals.Add(Normal);Colors.Add(L.Color);
        }
        for(int32 Corner=0;Corner<4;++Corner)
        {const int32 Next=(Corner+1)%4;Triangles.Append({Base+Corner,Base+Next,Base+4+Corner,Base+Next,Base+4+Next,Base+4+Corner});}
    }
    RouteMesh->SetWorldLocation(PlayerWorld);
    RouteMesh->CreateMeshSection_LinearColor(0,Vertices,Triangles,Normals,{},Colors,{},false);
}

void UACEPluginSubsystem::ForgetTargetEffects(int32 Target)
{
    const auto Matches=[Target](const auto& E){return N(E->AsObject(),TEXT("target"))==uint32(Target);};
    Debuffs.RemoveAll(Matches);PendingDebuffCasts.RemoveAll(Matches);
}

void UACEPluginSubsystem::ObserveTargetSpellCast(int32 Spell,int32 Target)
{
    ObservedAction=MakeShared<FJsonObject>();ObservedAction->SetStringField(TEXT("type"),TEXT("cast"));ObservedAction->SetStringField(TEXT("stage"),TEXT("requested"));ObservedAction->SetNumberField(TEXT("spell"),Spell);ObservedAction->SetNumberField(TEXT("target"),uint32(Target));EmitPluginAction(ObservedAction);
    LastTargetEffectSpell=Spell;LastTargetEffectTarget=Target;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    FACEWorldObject Object;uint32 School=0,Power=0,Category=0,Flags=0,Icon=0;double Duration=0;FString Name;
    if(!Target||Target==C->GetPlayerGuid()||!Dat->TryGetPluginSpellInfo(Spell,School,Power,Category,Flags,Duration)
        ||(Flags&(4|8))||Duration<=0||!Dat->TryGetSpellInfo(Spell,Name,Icon)||!C->GetWorldObject(Target,Object)
        ||Object.bDying||Object.Name.IsEmpty())return;
    const double Now=FPlatformTime::Seconds();
    PendingDebuffCasts.RemoveAll([&](const auto& E){const auto J=E->AsObject();return Now-N(J,TEXT("sent"))>30
        ||(N(J,TEXT("target"))==uint32(Target)&&N(J,TEXT("spell"))==Spell);});
    auto Record=MakeShared<FJsonObject>();Record->SetNumberField(TEXT("target"),uint32(Target));Record->SetNumberField(TEXT("spell"),Spell);
    Record->SetStringField(TEXT("confirmation"),TEXT("You cast ")+Name+TEXT(" on ")+Object.Name);
    Record->SetStringField(TEXT("resist"),Object.Name+TEXT(" resists your spell"));
    Record->SetNumberField(TEXT("category"),Category);Record->SetNumberField(TEXT("power"),Power);
    Record->SetNumberField(TEXT("duration"),Duration);Record->SetNumberField(TEXT("sent"),Now);
    PendingDebuffCasts.Add(MakeShared<FJsonValueObject>(Record));if(PendingDebuffCasts.Num()>8)PendingDebuffCasts.RemoveAt(0);
}

bool UACEPluginSubsystem::GetTargetEffectTimer(int32 Target,int32 Spell,double& Remaining,double& Duration) const
{
    Remaining=Duration=0;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    const auto Session=C->GetSession();
    const auto* Object=Session?Session->GetWorldObjects().Find(Target):nullptr;
    uint32 School=0,Power=0,Category=0,Flags=0;double BaseDuration=0;
    if(!Target||Target==C->GetPlayerGuid()||C->GetSessionState()!=EACESessionState::InWorld||ObservedSession.Pin()!=Session
        ||DataPlayer!=C->GetPlayerGuid()||DataServer!=C->GetServerName()
        ||!Object||Object->bDying||DefeatedTargets.Contains(Target)
        ||!Dat->TryGetPluginSpellInfo(Spell,School,Power,Category,Flags,BaseDuration)||(Flags&(4|8))||BaseDuration<=0)return false;
    const double Now=FPlatformTime::Seconds();
    for(const auto& Value:Debuffs)
    {
        const auto E=Value->AsObject();
        if(N(E,TEXT("target"))!=uint32(Target)||N(E,TEXT("category"))!=Category||N(E,TEXT("power"))<Power)continue;
        const double Left=N(E,TEXT("expires"))-Now;
        if(Left>Remaining){Remaining=Left;Duration=N(E,TEXT("duration"));}
    }
    return Remaining>0&&Duration>0;
}

void UACEPluginSubsystem::ObserveMetaChat(const FString& Text,const FString& Sender,int32 Type)
{
    if(Type==0x18&&Sender.IsEmpty()&&UCMCraft&&UCMCraft->Pending&&UCMCraft->Preparation.IsEmpty())
        if(auto Policy=PolicyPlugin();Policy&&Policy->Running&&UCMCraft->Catalog.Recipes.IsValidIndex(UCMCraft->PendingRecipe))
        {
            const auto& R=UCMCraft->Catalog.Recipes[UCMCraft->PendingRecipe];
            if(!R.FailMessage.IsEmpty()&&Text.TrimStartAndEnd()==R.FailMessage.TrimStartAndEnd())UCMCraft->FailureMessage=true;
        }
    if(Type==0x18&&Sender.IsEmpty()&&AutoCraft&&AutoCraft->Pending&&AutoCraft->Preparation.IsEmpty())
        if(auto Craft=Find(TEXT("autocraft"));Craft&&Craft->Running&&AutoCraft->Catalog.Recipes.IsValidIndex(AutoCraft->PendingRecipe))
        {
            const auto& R=AutoCraft->Catalog.Recipes[AutoCraft->PendingRecipe];
            if(!R.FailMessage.IsEmpty()&&Text.TrimStartAndEnd()==R.FailMessage.TrimStartAndEnd())AutoCraft->FailureMessage=true;
        }
    ObserveOffensiveSpellChat(Text,Sender,Type);
    if(Sender.IsEmpty()&&Type==0&&Text.TrimStartAndEnd()==TEXT("Your missile attack hit the environment."))RecordCombatOutcome(false);
    // VT starts its backward input when the local spell words arrive, not when
    // requesting the spell (which can still be turning or entering magic mode).
    if(PendingSpell&&Type==ACEChatMessageType::Spellcasting)
    {
        auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Player;
        if(!FastCastStarted&&C->GetWorldObject(C->GetPlayerGuid(),Player)&&!Player.Name.IsEmpty()&&Sender==Player.Name)
        {PendingSpellWords=true;BuffRetryAt=0;if(!FastCastOwner.IsEmpty()){FastCastStarted=true;FastCastStartedAt=FPlatformTime::Seconds();}}
    }
    if(Sender.IsEmpty()&&Type==ACEChatMessageType::Magic)
    {
        if(PendingSpell)
        {
            // UseDone acknowledges the action, including a fizzle. Only the
            // matching server spell result confirms an enchantment was applied.
            const FString& Prefix=PendingSpellConfirmation;
            const auto Matches=[&Text](const FString& Expected)
            {
                return !Expected.IsEmpty()&&(Text==Expected||Text==Expected+TEXT(".")
                    ||Text.StartsWith(Expected+TEXT(", refreshing "))||Text.StartsWith(Expected+TEXT(", surpassing "))
                    ||Text.StartsWith(Expected+TEXT(", but it is surpassed by "))||Text.StartsWith(Expected+TEXT(" and ")));
            };
            const bool Direct=!Prefix.IsEmpty()&&(Text==Prefix||Text==Prefix+TEXT(".")
                ||Text.StartsWith(Prefix+TEXT(", "))||Text.StartsWith(Prefix+TEXT(" and ")));
            const bool Recovery=PendingSpellRecoveryConfirmations.ContainsByPredicate([&Text](const auto& Pattern)
            {
                if(!Text.StartsWith(Pattern.Key)||!Text.EndsWith(Pattern.Value))return false;
                const int32 Digits=Text.Len()-Pattern.Key.Len()-Pattern.Value.Len();
                if(Digits<=0)return false;
                for(int32 I=0;I<Digits;++I)if(!FChar::IsDigit(Text[Pattern.Key.Len()+I]))return false;
                return true;
            });
            if(Direct||Recovery||PendingSpellItemConfirmations.ContainsByPredicate(Matches))
            {
                PendingSpellConfirmed=true;
                // Success is authoritative. Keep at most one completed cast's
                // acknowledgment outstanding; a second overlapping completion
                // falls back to the ordinary UseDone path.
                CompleteConfirmedBuff();
            }
            if(Text==TEXT("Your spell fizzled.")){PendingSpellFizzled=true;FastCastOwner.Empty();OptionalCastOwner.Empty();FastCastStarted=false;}
        }
        const double Now=FPlatformTime::Seconds();
        if(Text==TEXT("Your spell fizzled."))PendingDebuffCasts.RemoveAll([this](const auto& Value)
        {const auto J=Value->AsObject();return N(J,TEXT("spell"))==LastTargetEffectSpell&&N(J,TEXT("target"))==uint32(LastTargetEffectTarget);});
        for(int32 Index=PendingDebuffCasts.Num()-1;Index>=0;--Index)
        {
            const auto E=PendingDebuffCasts[Index]->AsObject();
            if(Now-N(E,TEXT("sent"))>30){PendingDebuffCasts.RemoveAt(Index);continue;}
            const FString Prefix=E->GetStringField(TEXT("confirmation"));
            const bool Success=Text==Prefix||Text==Prefix+TEXT(".")||Text.StartsWith(Prefix+TEXT(", refreshing "))||Text.StartsWith(Prefix+TEXT(", surpassing "))||Text.StartsWith(Prefix+TEXT(", but it is surpassed by "));
            const FString Resist=E->GetStringField(TEXT("resist"));
            if(Success)
            {
                // Server text identifies targets by name. Multiple outstanding
                // casts at identically named creatures cannot be disambiguated.
                const bool Ambiguous=PendingDebuffCasts.ContainsByPredicate([&](const auto& Other)
                {const auto J=Other->AsObject();return J!=E&&Now-N(J,TEXT("sent"))<=30
                    &&S(J,TEXT("confirmation"))==Prefix&&N(J,TEXT("target"))!=N(E,TEXT("target"));});
                if(Ambiguous)
                {
                    PendingDebuffCasts.RemoveAll([&](const auto& Other){return S(Other->AsObject(),TEXT("confirmation"))==Prefix;});
                    break;
                }
                // Use the known spell's base duration conservatively. A stronger
                // pre-existing enchantment still confirms this weaker layer exists.
                E->SetNumberField(TEXT("expires"),Now+N(E,TEXT("duration")));
                // Retain stronger layers when a weaker cast is confirmed.
                Debuffs.RemoveAll([&](const auto& Old){return N(Old->AsObject(),TEXT("expires"))<=Now
                    ||(N(Old->AsObject(),TEXT("target"))==N(E,TEXT("target"))&&N(Old->AsObject(),TEXT("category"))==N(E,TEXT("category"))
                        &&N(Old->AsObject(),TEXT("power"))==N(E,TEXT("power")));});
                Debuffs.Add(MakeShared<FJsonValueObject>(E));
            }
            if(Success||Text==Resist||Text==Resist+TEXT("."))
            {++DebuffRevision;LastDebuffTarget=int32(uint32(N(E,TEXT("target"))));LastDebuffSpell=int32(N(E,TEXT("spell")));PendingDebuffCasts.RemoveAt(Index);break;}
        }
    }
    auto P=PolicyPlugin();if(!P||!P->Running||!P->Profile->HasField(TEXT("vt_meta")))return;
    FString Line=Text;
    // Decal chat conditions see the rendered line, including the speaker.
    // Channel messages are already formatted by the session decoder.
    if(Type==ACEChatMessageType::Tell)Line=FString::Printf(TEXT("%s tells you, \"%s\""),Sender.IsEmpty()?TEXT("Someone"):*Sender,*Text);
    else if(Type==ACEChatMessageType::Speech&&!Sender.IsEmpty())Line=Sender.Equals(TEXT("You"),ESearchCase::IgnoreCase)?FString::Printf(TEXT("You say, \"%s\""),*Text):FString::Printf(TEXT("%s says, \"%s\""),*Sender,*Text);
    else if(Type==ACEChatMessageType::Emote&&!Sender.IsEmpty())Line=Sender+TEXT(" ")+Text;
    auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("serial"),++MetaChatSerial);E->SetStringField(TEXT("text"),Line.Left(4096));E->SetNumberField(TEXT("color"),Type);E->SetStringField(TEXT("sender"),Sender);
    MetaChatEvents.Add(MakeShared<FJsonValueObject>(E));if(MetaChatEvents.Num()>128)MetaChatEvents.RemoveAt(0);
}

void UACEPluginSubsystem::ExtendInventorySnapshot(const TSharedPtr<FJsonObject>& Out)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();if(!Session)return;
    const auto& V=C->GetPlayerVitalsView();
    auto ItemJson=[this](const FACEWorldObject& Item){return PluginItemSnapshot(Item);};
    if(InventoryRevision!=Session->GetInventoryDataRevision() || InventoryAppraisalRevision!=DataRevision)
    {
        InventoryWieldInt64Keys.Reset();
        for(const auto& Pair:Session->GetWorldObjects())if(IsOwnedPluginItem(Pair.Value))
            if(const auto* A=GetEquipmentAppraisal(Pair.Value);A&&A->bSuccess)
                for(int32 Key:{158,270,273,276})if(A->IntProperties.FindRef(Key)==13)
                    InventoryWieldInt64Keys.Add(A->IntProperties.FindRef(Key+1));
    }
    // Keep immutable rows across background assessments. A one-item reply used
    // to reconstruct every item's property/spell tables on the game thread.
    uint32 Eligibility=GetTypeHash(V.Level);
    for(int32 Id=1;Id<=6;++Id){Eligibility=HashCombine(Eligibility,GetTypeHash(V.GetAttributeBase(Id)));Eligibility=HashCombine(Eligibility,GetTypeHash(V.GetAttributeCurrent(Id)));}
    for(const auto& Skill:V.Skills){Eligibility=HashCombine(Eligibility,GetTypeHash(Skill.Base));Eligibility=HashCombine(Eligibility,GetTypeHash(Skill.Current));Eligibility=HashCombine(Eligibility,GetTypeHash(Skill.AdvancementClass));}
    Eligibility=HashCombine(Eligibility,GetTypeHash(V.MaxHealth));Eligibility=HashCombine(Eligibility,GetTypeHash(V.MaxStamina));Eligibility=HashCombine(Eligibility,GetTypeHash(V.MaxMana));
    Eligibility=HashCombine(Eligibility,GetTypeHash(V.HeritageGroup));
    Eligibility=HashCombine(Eligibility,GetTypeHash(V.EffectiveAllegianceRank));
    for(int32 Id:{1,3,5})Eligibility=HashCombine(Eligibility,GetTypeHash(V.GetVitalBase(Id)));
    for(const auto& Pair:V.StatQualityInts)Eligibility=HashCombine(Eligibility,HashCombine(GetTypeHash(Pair.Key),GetTypeHash(Pair.Value)));
    for(const auto& Pair:V.QualityBools)Eligibility=HashCombine(Eligibility,HashCombine(GetTypeHash(Pair.Key),GetTypeHash(Pair.Value)));
    // XP/luminance change throughout combat. Rebuild rows only for Int64
    // qualities actually used by carried equipment's requirements.
    for(int32 Key:InventoryWieldInt64Keys)
    {
        Eligibility=HashCombine(Eligibility,HashCombine(GetTypeHash(Key),GetTypeHash(V.QualityInt64s.Contains(Key))));
        Eligibility=HashCombine(Eligibility,GetTypeHash(V.QualityInt64s.FindRef(Key)));
    }
    if(InventoryRevision!=Session->GetInventoryDataRevision() || InventoryEligibility!=Eligibility
        || (InventoryAppraisalRevision!=DataRevision && DirtyInventoryAppraisals.IsEmpty()))
    {
        CachedInventory.Empty();CachedInventoryIndices.Empty();
        for(const auto& Pair:Session->GetWorldObjects())
            if(IsOwnedPluginItem(Pair.Value) && CachedInventory.Num()<1024)
            {CachedInventoryIndices.Add(Pair.Key,CachedInventory.Num());CachedInventory.Add(ItemJson(Pair.Value));}
        InventoryRevision=Session->GetInventoryDataRevision();InventoryEligibility=Eligibility;
    }
    else if(InventoryAppraisalRevision!=DataRevision)
        for(int32 Id:DirtyInventoryAppraisals)
            if(const auto* Index=CachedInventoryIndices.Find(Id))
                if(const auto* Item=Session->GetWorldObjects().Find(Id))CachedInventory[*Index]=ItemJson(*Item);
    InventoryAppraisalRevision=DataRevision;DirtyInventoryAppraisals.Reset();
    Out->SetArrayField(TEXT("inventory"),CachedInventory);
    Out->SetNumberField(TEXT("inventory_revision"),double(InventoryRevision));
    Out->SetNumberField(TEXT("appraisal_revision"),double(InventoryAppraisalRevision));
}

void UACEPluginSubsystem::ObserveItemUse(int32 Source,int32 Target)
{
    ObservedAction=MakeShared<FJsonObject>();auto& Event=ObservedAction;
    Event->SetStringField(TEXT("type"),TEXT("use"));Event->SetStringField(TEXT("stage"),TEXT("requested"));
    Event->SetNumberField(TEXT("source"),uint32(Source));Event->SetNumberField(TEXT("target"),uint32(Target?Target:Source));
    FACEWorldObject Item;auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(C->GetWorldObject(Source,Item))
    {
        Event->SetNumberField(TEXT("wcid"),Item.WeenieClassId);
        if((Item.ItemType&ACEItemType::ManaStone)&&IsOwnedPluginItem(Item))
        {
            // Manual use, Action Deck and UCM must refresh the same resource
            // data. A charge's depleted/destroyed state is not a new appraisal.
            PendingResourceRefresh.Add(Source);
            PendingManaRefresh=true;
            FACEWorldObject Recipient;
            if(Target!=C->GetPlayerGuid()&&C->GetWorldObject(Target,Recipient)&&IsOwnedPluginItem(Recipient))
                PendingResourceRefresh.Add(Target);
        }
    }
    if(Target&&C->GetWorldObject(Target,Item))Event->SetNumberField(TEXT("target_wcid"),Item.WeenieClassId);
    EmitPluginAction(Event);
}
void UACEPluginSubsystem::EmitPluginAction(const TSharedPtr<FJsonObject>& Event)
{
    Event->SetNumberField(TEXT("time"),FPlatformTime::Seconds());
    if(S(Event,TEXT("stage"))==TEXT("requested"))Event->SetNumberField(TEXT("requested_at"),N(Event,TEXT("time")));
    else if(S(Event,TEXT("stage"))==TEXT("completed"))
    {ActionOutcomes.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>(*Event)));if(ActionOutcomes.Num()>64)ActionOutcomes.RemoveAt(0);}
    for(const auto& P:Plugins)if(P->Running&&P->Enabled&&P->HasPresentation()&&P->Permissions.Contains(TEXT("data")))QueuePluginEvent(*P,MakeShared<FJsonObject>(*Event));
}
