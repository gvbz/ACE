using System;
using System.Numerics;
using System.Linq;
using System.Collections.Generic;
using ACE.Common.Logging;
using ACE.Entity.Enum;
using ACE.Server.Entity;
using ACE.Server.Entity.Actions;
using ACE.Server.Network.GameEvent.Events;
using ACE.Server.Network.GameMessages.Messages;
using ACE.Server.Physics.Animation;
using ACE.Server.Physics;
using ACE.Server.Network.Sequence;

namespace ACE.Server.WorldObjects
{
    partial class Player
    {
        private bool vrNegotiated;
        public bool VRUniformLocomotionSubscribed { get; private set; }
        public bool VRHealthFeedbackSubscribed { get; private set; }
        private bool vrRecoverySubscribed;
        private bool vrCastingSubscribed;
        public bool VRHealthBarsSubscribed { get; private set; }
        private VRCombatRequest vrCast;
        private DateTime vrLastAim;
        private float vrRecoveryDuration;
        private uint vrSequence;
        private DateTime vrNextAttack, vrLastHello, vrLastRejection, vrLastSpellProfile;

        private void RejectVRCombat(VRCombatRequest r, string reason)
        {
            log.Debug($"[VR] {Name} rejected kind={r.Kind} seq={r.Sequence} weapon={r.Weapon:X8} target={r.Target:X8} cell={r.Cell:X8}/{Location?.Cell:X8}: {reason}");
            if ((DateTime.UtcNow - vrLastRejection).TotalSeconds < 1) return;
            vrLastRejection = DateTime.UtcNow;
            Session.Network.EnqueueSend(new GameEventCommunicationTransientString(Session, reason));
        }

        public bool IsVRRequestCurrent(VRCombatRequest r)
        {
            if (Teleporting || Location == null || r.Teleport != BitConverter.ToUInt16(
                Sequences.GetCurrentSequence(SequenceType.ObjectTeleport), 0)) return false;
            if (r.Cell == Location.Cell) return true;
            // Hands are offsets from the server's current feet, not coordinates
            // in the packet's cell. A normal cell crossing can overtake a cast or
            // aim packet. Accept neighboring space within the same teleport epoch;
            // reach, visibility, costs and collisions still use server position.
            if (VRCombatRequest.NeighboringOutdoorCells(r.Cell, Location.Cell)) return true;
            if ((r.Cell >> 16) != (Location.Cell >> 16)) return false;
            var indoor = (Location.Cell & 0xffff) >= 0x100 ? Location.Cell : r.Cell;
            if ((indoor & 0xffff) < 0x100) return false;
            var cell = ACE.DatLoader.DatManager.CellDat.ReadFromDat<ACE.DatLoader.FileTypes.EnvCell>(indoor);
            var other = indoor == r.Cell ? Location.Cell : r.Cell;
            return cell != null && ((other & 0xffff) < 0x100 ? cell.SeenOutside
                : cell.CellPortals.Any(p => p.OtherCellId == (other & 0xffff)));
        }

        public void HandleVRCombat(VRCombatRequest r)
        {
            var now = DateTime.UtcNow;
            if (r.Kind == 0)
            {
                if ((now - vrLastHello).TotalSeconds < 1) return;
                vrLastHello = now;
                vrNegotiated = true;
                if (Teleporting || PhysicsObj == null)
                    Session.Network.EnqueueSend(new GameEventVRCapabilities(Session));
                else
                {
                    // The client keep-ring can revisit records after a generator has
                    // respawned them with new GUIDs. Reconcile against this session's
                    // live known objects, not names or approximate positions.
                    var known = GetKnownObjects().Where(o => !o.IsDestroyed && (!o.Visibility || Adminvision)).ToList();
                    var guids = new HashSet<uint>(known.Select(o => o.Guid.Full));
                    foreach (var creature in known.OfType<Creature>())
                        foreach (var item in creature.EquippedObjects.Values)
                            if (IsInChildLocation(item)) guids.Add(item.Guid.Full);
                    Session.Network.EnqueueSend(new GameEventVRCapabilities(Session,
                        BitConverter.ToUInt16(Sequences.GetCurrentSequence(SequenceType.ObjectTeleport), 0), guids));
                    foreach (var obj in known) TrackObject(obj);
                }
                return;
            }
            if (r.Kind == 4)
            {
                VRHealthFeedbackSubscribed = vrNegotiated && (r.FeedbackFeatures & 1u) != 0;
                VRUniformLocomotionSubscribed = vrNegotiated && (r.FeedbackFeatures & VRLocomotion.Subscription) != 0;
                vrRecoverySubscribed = vrNegotiated && (r.FeedbackFeatures & 2u) != 0;
                vrCastingSubscribed = vrNegotiated && (r.FeedbackFeatures & 4u) != 0;
                VRHealthBarsSubscribed = vrNegotiated && (r.FeedbackFeatures & 8u) != 0;
                if (vrNegotiated && (r.FeedbackFeatures & VRCombatRequest.ReceivePoses) != 0)
                {
                    // Receiving tracked avatars does not imply this player has
                    // tracked hands. Desktop observers keep normal combat rules.
                    vrPoseSubscribed = true;
                    vrPoseVersion = (r.FeedbackFeatures & VRCombatRequest.ReceiveEquipmentPoses) != 0 ? 2u : 1u;
                }
                return;
            }
            if (r.Kind == 5)
            {
                if (!vrNegotiated || (now-vrLastSpellProfile).TotalSeconds < .1) return;
                vrLastSpellProfile = now;
                var profile = new Spell(r.Subject);
                if (profile.NotFound) return;
                Session.Network.EnqueueSend(new GameEventVRSpellProfile(Session,r.Subject,
                    profile.IsProjectile ? GetProjectileSpeed(profile) : 0f,
                    profile.IsProjectile && SpellProjectile.GetProjectileSpellType(profile.Id) == ProjectileSpellType.Arc,
                    (r.FeedbackFeatures & 1u) != 0 ? (profile.IsProjectile ? GetProjectileRadius(profile) : 0f) : null));
                return;
            }
            if (!vrNegotiated || unchecked((int)(r.Sequence - vrSequence)) <= 0) return;
            vrSequence = r.Sequence;
            if (!IsVRRequestCurrent(r))
            {
                // Background windup updates must never produce repeated alerts.
                if (r.Kind != 7) RejectVRCombat(r, "Wait until you arrive before attacking.");
                return;
            }
            if (!IsAlive || PKLogout || suicideInProgress) return;
            if (r.Kind == 7)
            {
                // Refresh the pending cast, never its spell, target, costs or timing.
                // Target carries the original cast sequence for this packet kind.
                if (!vrCastingSubscribed || CombatMode != CombatMode.Magic || GetEquippedWand()?.Guid.Full != r.Weapon
                    || (now-vrLastAim).TotalSeconds < .025) return;
                var aim = MagicState.IsCasting ? MagicState.CastSpellParams?.VRAim : MagicState.CastQueue?.VRAim;
                if (aim == null || aim.Sequence != r.Target || aim.Weapon != r.Weapon || aim.Subject != r.Subject) return;
                aim.Origin = r.Origin; aim.Vector = r.Vector; aim.Cell = r.Cell;
                vrLastAim = now;
                return;
            }
            if (r.Kind == 6)
            {
                if (r.Subject == 0) HandleActionDropItem(r.Weapon, r);
                else HandleActionStackableSplitTo3D(r.Weapon, (int)r.Subject, r);
                return;
            }
            RefreshVRPendingLanding();
            if (r.Kind == 1 && IsBusy && CombatMode == CombatMode.Magic)
            {
                // Match retail's single recoil queue without a face-level alert
                // for every trigger press during the normal cast animation.
                if (MagicState.CanQueue)
                {
                    MagicState.CastQueue = new CastQueue(CastQueueType.Tracked, r.Target, r.Subject, null) { VRAim = r };
                    MagicState.CanQueue = false;
                }
                return;
            }
            if (IsBusy || IsJumping) { RejectVRCombat(r, IsJumping ? "Land before attacking." : "Not ready to attack yet."); return; }
            if (r.Kind == 1) { CastVRSpell(r); return; }
            if (Attacking || now < vrNextAttack || now < NextRefillTime)
            {
                if ((r.Kind == 2 || r.Kind == 3) && vrRecoverySubscribed) SendVRRecovery(r, now);
                else RejectVRCombat(r, "Your weapon is recovering. Swing or release again when ready.");
                return;
            }
            if (r.Kind == 2) SwingVRWeapon(r, now);
            else if (r.Kind == 3) FireVRMissile(r, now);
        }

        private void RefreshVRPendingLanding()
        {
            // AutoPos and an immediately following trigger can be dispatched
            // before the next physics tick. Test the queued landing through
            // normal server collision before consulting the old airborne state.
            // Contact alone never grants permission to cast, and cell/portal
            // transitions retain their normal world-update ordering.
            if (!IsJumping || !LastContact || Teleporting || RequestedLocation == null
                || RequestedLocation.Cell != Location.Cell) return;
            var landing = RequestedLocation;
            RequestedLocation = null;
            var wasUpdating = InUpdate;
            try
            {
                InUpdate = true; // Same cell; no landblock relocation is required.
                UpdatePlayerPosition(landing);
                if (IsJumping && !Teleporting && Location.Cell == landing.Cell
                    && PhysicsObj.Position.ObjCellID == landing.Cell
                    && Vector3.DistanceSquared(PhysicsObj.Position.Frame.Origin, landing.Pos) <= PhysicsGlobals.EpsilonSq)
                    PhysicsObj.RefreshWalkableContact();
            }
            finally { InUpdate = wasUpdating; }
        }

        private void SendVRRecovery(VRCombatRequest r, DateTime now)
        {
            if (!vrRecoverySubscribed) return;
            var ready = vrNextAttack > NextRefillTime ? vrNextAttack : NextRefillTime;
            var remaining = (float)Math.Clamp((ready - now).TotalSeconds, 0, 60);
            Session.Network.EnqueueSend(new GameEventVRRecovery(Session, r.Sequence, r.Teleport,
                remaining, Math.Clamp(Math.Max(remaining, vrRecoveryDuration), .001f, 60f)));
        }

        private void CastVRSpell(VRCombatRequest r)
        {
            // Queued aim is tied to its original world/teleport epoch. Never
            // release it after death, a portal, a jump, or an equipment change.
            if (!IsAlive || PKLogout || suicideInProgress || IsBusy || IsJumping || !IsVRRequestCurrent(r)) return;
            if (CombatMode != CombatMode.Magic) return;
            var wand = GetEquippedWand();
            if (wand == null || wand.Guid.Full != r.Weapon) return;
            var itemCaster = !SpellIsKnown(r.Subject) && IsWeaponSpell(r.Subject, wand) ? wand : null;
            if (!VerifySpell(r.Subject, itemCaster)) return;
            var spell = ValidateSpell(r.Subject, itemCaster != null);
            if (spell == null) return;
            if (!spell.IsProjectile)
            {
                if (spell.IsSelfTargeted || spell.IsFellowshipSpell) r.Target = Guid.Full;
                if (spell.NonComponentTargetType == ItemType.None && !spell.IsSelfTargeted)
                    HandleActionMagicCastUnTargetedSpell(r.Subject);
                else if (r.Target != 0) HandleActionCastTargetedSpell(r.Target, r.Subject, itemCaster);
                else SendUseDoneEvent(WeenieError.TargetNotAcquired);
                if (MagicState.IsCasting && MagicState.CastSpellParams?.Spell.Id == spell.Id) BeginVRCast(r, spell, itemCaster);
                return;
            }
            // Use the same spell prechecks, costs, components, windup and recoil as desktop casts.
            var skill = itemCaster?.ItemSpellcraft != null ? (uint)itemCaster.ItemSpellcraft : GetCreatureSkill(spell.School).Current;
            var status = GetCastingPreCheckStatus(spell, skill, itemCaster != null);
            if (!CalculateManaUsage(status, spell, null, itemCaster, out var mana)) return;
            MagicState.OnCastStart();
            StartPos = new Physics.Common.Position(PhysicsObj.Position);
            DoSpellWords(spell, itemCaster != null);
            var chain = new ActionChain();
            DoWindupGestures(spell, itemCaster != null, chain);
            DoCastGesture(spell, itemCaster, chain);
            MagicState.SetCastParams(spell, itemCaster, skill, mana, null, status);
            MagicState.CastSpellParams.VRAim = r;
            BeginVRCast(r, spell, itemCaster);
            if (!FastTick) chain.AddAction(this, () => DoCastSpell(MagicState));
            chain.EnqueueChain();
        }

        private void SwingVRWeapon(VRCombatRequest r, DateTime now)
        {
            if (CombatMode != CombatMode.Melee) { RejectVRCombat(r, "Enter melee stance before swinging."); return; }
            var weapon = GetEquippedMeleeWeapon();
            var unarmed = r.Weapon == 0;
            if ((weapon?.Guid.Full ?? 0u) != r.Weapon || (unarmed && GetEquippedMainHand() != null))
            { RejectVRCombat(r, "The melee weapon has changed. Try again with the equipped weapon."); return; }
            // Subject identifies dominant (0) or support (1) fist. A shield or
            // offhand weapon cannot be submitted as an empty-hand punch.
            if (unarmed && (r.Subject > 1 || (r.Subject == 1 && EquippedObjects.Values.Any(i => i.CurrentWieldedLocation == EquipMask.Shield))))
            { RejectVRCombat(r, "That hand is holding an item."); return; }
            if (unarmed && (!VRCombatRequest.InReach(r.Origin, 1.35f) || !VRCombatRequest.InReach(r.Vector, 1.35f)))
            { RejectVRCombat(r, "Move closer to punch this creature."); return; }
            var target = CurrentLandblock?.GetObject(r.Target) as Creature;
            if (target == null || target == this || !target.IsAlive || !CanDamage(target)) { RejectVRCombat(r, "That target cannot be attacked."); return; }
            // Visibility is a ray test. Moving the entire player cylinder to the
            // target falsely rejects hand strikes across uneven ground.
            if (!IsDirectVisible(target)) { RejectVRCombat(r, "The strike is blocked by the environment."); return; }
            if (GetCylinderDistance(target) > (unarmed ? 1.35f : 2.8f)) { RejectVRCombat(r, "Move closer to strike this creature."); return; }
            // Include animated heads, limbs and tails outside the locomotion cylinder.
            var offset = PhysicsObj.Position.GetOffset(target.PhysicsObj.Position);
            var hit=VRMeleeContact.Intersects(target.PhysicsObj,r.Origin-offset,r.Vector-offset,target.Height,out var contact);
            // Remote rendering interpolates sparse retail positions. Reconcile
            // only a bounded visual offset; reach, visibility, swing speed,
            // cooldown and the actual server-owned creature geometry still apply.
            if (r.ObservedBody is Vector3 observed)
            {
                var error = observed-offset;
                if (!hit && new Vector2(error.X,error.Y).LengthSquared() <= .8f*.8f && Math.Abs(error.Z) <= .5f)
                {
                    offset = observed;
                    hit=VRMeleeContact.Intersects(target.PhysicsObj,r.Origin-offset,r.Vector-offset,target.Height,out contact);
                }
            }
            if (!hit)
            {
                log.Debug($"[VR] melee geometry seq={r.Sequence} target={target.Guid} start={r.Origin} end={r.Vector} body={offset} height={target.Height:F3}");
                RejectVRCombat(r, "The swing missed the creature's body."); return;
            }
            var sampleHeight = Math.Clamp(Vector3.Lerp(r.Origin, r.Vector, contact).Z, offset.Z, offset.Z + target.Height);
            var speed = Vector3.Distance(r.Origin, r.Vector) / r.Duration;
            PowerLevel = r.RequestedPower ?? Math.Clamp((speed - .8f) / 5f, 0f, 1f);
            AttackHeight = sampleHeight - offset.Z > target.Height * .7f ? ACE.Entity.Enum.AttackHeight.High
                : sampleHeight - offset.Z < target.Height * .35f ? ACE.Entity.Enum.AttackHeight.Low : ACE.Entity.Enum.AttackHeight.Medium;
            MeleeTarget = target; AttackTarget = target;
            // Tracked hands always use fists/gauntlets, even at full power.
            // Desktop attacks retain the retail punch/kick choice.
            var motion = GetSwingAnimation(unarmed);
            var duration = MotionTable.GetAnimationLength(MotionTableId, CurrentMotionState.Stance, motion, GetAnimSpeed());
            vrRecoveryDuration = Math.Max(.35f, duration + PowerLevel * (IsDualWieldAttack ? .8f : 1f));
            vrNextAttack = now.AddSeconds(vrRecoveryDuration);
            SendVRRecovery(r, now);
            // No sticky charge, rotate-to-target or repeat loop for physical swings.
            EnqueueBroadcastMotion(new Motion(this, motion, GetAnimSpeed()));
            UpdateVitalDelta(Stamina, -GetAttackStamina(GetPowerRange()));
            TryProcEquippedItems(this, this, true, weapon);
            var damage = DamageTarget(target, weapon);
            log.Debug($"[VR] {Name} melee seq={r.Sequence} target={target.Guid} speed={speed:F2} damage={damage?.Damage:F1}");
            if (damage != null && damage.HasDamage) TryProcEquippedItems(this, target, false, weapon);
            if (UnderLifestoneProtection) LifestoneProtectionDispel();
            OnAttackDone();
        }

        private void FireVRMissile(VRCombatRequest r, DateTime now)
        {
            if (MovementBlocksMissile()) { RejectVRCombat(r, StationaryMissileMessage); return; }
            var weapon = GetEquippedMissileWeapon();
            if (weapon == null || weapon.Guid.Full != r.Weapon) { RejectVRCombat(r, "The missile weapon has changed. Try again with the equipped weapon."); return; }
            var crossbow = weapon.DefaultCombatStyle == CombatStyle.Crossbow;
            if (!VRCombatRequest.InReach(r.Origin, crossbow ? VRCombatRequest.MaxMissileMuzzleReach : 1.5f))
            { RejectVRCombat(r, "The shot origin is too far from the equipped weapon. Recenter your tracking."); return; }
            var rejection = r.MissileReleaseRejection(CombatMode, weapon.DefaultCombatStyle);
            if (rejection != null) { RejectVRCombat(r, rejection); return; }
            var ammo = weapon.IsAmmoLauncher ? GetEquippedAmmo() : weapon;
            if (ammo == null) { SendWeenieError(WeenieError.YouAreOutOfAmmunition); return; }
            AccuracyLevel = r.RequestedPower ?? r.Amount;
            AttackHeight = ACE.Entity.Enum.AttackHeight.Medium;
            var origin = Location.Pos + r.Origin;
            var speed = GetProjectileSpeed() * (.35f + .65f * r.Amount);
            UpdateVitalDelta(Stamina, -GetAttackStamina(GetAccuracyRange()));
            TryProcEquippedItems(this, this, true, weapon);
            EnqueueBroadcast(new GameMessageSound(Guid, GetLaunchMissileSound(weapon), 1f));
            var projectile = LaunchProjectile(GetEquippedMissileLauncher(), ammo, null, origin, r.AimRotation, r.Vector * speed, true);
            log.Debug($"[VR] {Name} missile seq={r.Sequence} projectile={projectile?.Guid} origin={origin} velocity={r.Vector * speed}");
            UpdateAmmoAfterLaunch(ammo);
            // Thrown weapons are their own equipped stack. Retail's ordinary
            // attack chain re-tracks it; VR has no follow-up reload animation.
            if (!weapon.IsAmmoLauncher && GetEquippedMissileWeapon() == weapon)
            {
                EnqueueActionBroadcast(p => p.TrackEquippedObject(this, weapon));
                var attach = new ActionChain();
                attach.AddDelaySeconds(.001);
                attach.AddAction(this, () =>
                {
                    if (GetEquippedMissileWeapon() == weapon)
                        EnqueueBroadcast(new GameMessageParentEvent(this, weapon, ACE.Entity.Enum.ParentLocation.RightHand,
                            ACE.Entity.Enum.Placement.RightHandCombat));
                });
                attach.EnqueueChain();
            }
            // Consume first: the last arrow must never be reattached by a queued reload.
            var reload = weapon.IsAmmoLauncher && GetEquippedMissileWeapon() != null && GetEquippedAmmo() != null ? ReloadMissileAmmo() : 0;
            vrRecoveryDuration = (float)Math.Max(.5, reload + AccuracyLevel);
            vrNextAttack = now.AddSeconds(vrRecoveryDuration);
            SendVRRecovery(r, now);
            if (GetEquippedMissileWeapon() == null || (weapon.IsAmmoLauncher && GetEquippedAmmo() == null))
                SetCombatMode(CombatMode.NonCombat);
            if (UnderLifestoneProtection) LifestoneProtectionDispel();
            OnAttackDone();
        }
    }
}
