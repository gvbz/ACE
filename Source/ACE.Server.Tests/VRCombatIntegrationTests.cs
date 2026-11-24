using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Numerics;
using System.Reflection;
using System.Text;
using ACE.Common;
using ACE.Database;
using ACE.DatLoader;
using ACE.Entity;
using ACE.Entity.Enum;
using ACE.Entity.Enum.Properties;
using ACE.Server.Entity;
using ACE.Server.Factories;
using ACE.Server.Entity.Actions;
using ACE.Server.Managers;
using ACE.Server.Network;
using ACE.Server.Network.GameMessages.Messages;
using ACE.Server.Network.Managers;
using ACE.Server.WorldObjects;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace ACE.Server.Tests
{
    // Opt-in retail integration fixtures: read templates/DAT, but never start a
    // shard, bind a socket, allocate persistent GUIDs, or save a character.
    [TestClass, DoNotParallelize]
    public class VRCombatIntegrationTests
    {
        private const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;
        private static uint nextGuid = 0x8ffff000;

        [TestMethod]
        public void ReportedDungeonLoginPlacement()
        {
            using var f = new Fixture();
            var original = new Position(0x01D9010A,30.057709f,-15.371094f,0,0,0,-.186291f,.982495f);
            bool Place(Vector3 offset)
            {
                f.Player.PhysicsObj?.DestroyObject();
                typeof(WorldObject).GetProperty("PhysicsObj").SetValue(f.Player,null);
                f.Player.Location = new Position(original);
                f.Player.Location.Pos += offset;
                f.Player.InitPhysicsObj();
                return f.Player.AddPhysicsObj();
            }
            Assert.IsTrue(Place(Vector3.Zero), "The reported floor-contact login must not relocate to sanctuary.");
            Assert.AreEqual(original.Cell, f.Player.Location.Cell);
            Assert.AreEqual(original.PositionX, f.Player.Location.PositionX, .00001f);
            Assert.AreEqual(original.PositionY, f.Player.Location.PositionY, .00001f);
            Assert.AreEqual(.005f, f.Player.Location.PositionZ, .0002f);
            Assert.IsTrue(Place(new Vector3(0,0,.005f)), "Already valid placement still uses the original position.");
            Assert.AreEqual(.005f, f.Player.Location.PositionZ, .0002f);
            Assert.IsFalse(Place(new Vector3(0,0,-1)), "A genuine below-floor position must still fail collision validation.");
            Assert.IsNull(f.Player.PhysicsObj, "Failed placement still cleans up physics for the sanctuary fallback.");
            Assert.IsTrue(Place(new Vector3(0,0,3)), "Airborne portal/logon positions remain valid without a ground requirement.");
            Assert.IsTrue(f.Player.Location.PositionZ > .5f, "A legitimate drop must not be snapped to the floor.");
        }

        [TestMethod]
        public void UniformVRMoveUsesTheSamePhysicsAndLegacyObserverRates()
        {
            using var f = new Fixture();
            f.Player.HandleVRCombat(new VRCombatRequest());
            f.Player.HandleVRCombat(new VRCombatRequest { Kind = 4, FeedbackFeatures = VRLocomotion.Subscription });
            Assert.IsTrue(f.Player.VRUniformLocomotionSubscribed);
            ACE.Server.Network.Structure.MoveToState Packet(Vector2 input, bool extension)
            {
                using var stream = new MemoryStream(); using var w = new BinaryWriter(stream);
                w.Write(0xffu); w.Write((uint)HoldKey.Run); w.Write((uint)MotionStance.NonCombat);
                w.Write((uint)(input.Y < 0 ? MotionCommand.WalkBackwards : MotionCommand.WalkForward));
                w.Write((uint)HoldKey.None); w.Write(MathF.Abs(input.Y));
                w.Write((uint)(input.X < 0 ? MotionCommand.SideStepLeft : MotionCommand.SideStepRight));
                w.Write((uint)HoldKey.None); w.Write(MathF.Abs(input.X));
                w.Write(f.Player.Location.Cell);
                foreach (float v in new[] { f.Player.Location.Pos.X, f.Player.Location.Pos.Y, f.Player.Location.Pos.Z, 1f, 0f, 0f, 0f }) w.Write(v);
                for (int i = 0; i < 4; ++i) w.Write((ushort)0);
                w.Write((byte)1); w.Write(new byte[3]);
                if (extension) w.Write(VRLocomotion.MoveMarker);
                stream.Position = 0;
                return new ACE.Server.Network.Structure.MoveToState(f.Player, new BinaryReader(stream));
            }
            foreach (var stick in new[] { Vector2.UnitY, -Vector2.UnitY, Vector2.UnitX, -Vector2.UnitX,
                Vector2.Normalize(new Vector2(1, -1)), new Vector2(.3f, -.4f), Vector2.Zero })
            {
                var state = Packet(stick, true);
                Assert.IsTrue(state.UniformVRInput.HasValue);
                f.Player.OnMoveToState_ServerMethod(state);
                var interp = f.Player.PhysicsObj.get_minterp();
                var broadcast = new ACE.Server.Network.Structure.MovementData(f.Player, state);
                var observed = broadcast.Invalid.State;
                var expected = VRLocomotion.Resolve(stick, true, f.Player.GetRunRate()).Velocity;
                Assert.AreEqual(expected.X, 1.25f * observed.SidestepSpeed, .00001f);
                Assert.AreEqual(expected.Y, (observed.ForwardCommand == MotionCommand.RunForward ? 4f : 3.12f) * observed.ForwardSpeed, .00001f);
                var velocity = interp.get_state_velocity();
                Assert.AreEqual(expected.X, velocity.X, .0001f);
                Assert.AreEqual(expected.Y, velocity.Y, .0001f);
                if (stick == Vector2.Zero)
                {
                    Assert.AreEqual(MotionCommand.Ready, observed.ForwardCommand);
                    Assert.AreEqual(MotionCommand.Invalid, observed.SidestepCommand);
                }
                Assert.IsTrue(broadcast.Serialize().Length > 4,
                    "Ordinary movement serialization does not require an observer VR subscription.");
            }
            var legacy = Packet(new Vector2(1, -1), false);
            Assert.IsFalse(Packet(new Vector2(float.NaN, 1), true).IsValid,
                "Rejected VR magnitudes must never fall back to unvalidated raw physics.");
            Assert.IsTrue(legacy.IsValid);
            Assert.IsNull(legacy.UniformVRInput, "Opt-in alone cannot change a legacy packet.");
            f.Player.OnMoveToState_ServerMethod(legacy);
            Assert.IsNull(f.Player.PhysicsObj.get_minterp().UniformVRInput);
            var oldMotion = new ACE.Server.Network.Structure.MovementData(f.Player, legacy).Invalid.State;
            Assert.AreEqual(-f.Player.GetRunRate() * .65f, oldMotion.ForwardSpeed, .00001f);
            Assert.IsTrue(oldMotion.SidestepSpeed <= 3);
            f.Player.OnMoveToState_ServerMethod(Packet(Vector2.UnitX, true));
            f.Player.PhysicsObj.StopCompletely(true); // same stop used by server MoveTo/approach
            Assert.IsNull(f.Player.PhysicsObj.get_minterp().UniformVRInput);
            Assert.AreEqual(Vector3.Zero, f.Player.PhysicsObj.get_minterp().get_state_velocity());
        }

        [TestMethod]
        public void StationaryCancelledApproachStillCompletesUse()
        {
            using var f = new Fixture();
            f.Player.PhysicsObj.StopCompletely(true);
            f.Player.PhysicsObj.Velocity = Vector3.Zero;
            Assert.IsFalse(f.Player.PhysicsObj.IsMovingOrAnimating);
            int completions = 0;
            f.Player.MoveToParams = new MoveToParams(_ => ++completions, f.Target);
            f.Player.UpdateObjectPhysics();
            Assert.AreEqual(1, completions, "An already stationary approach must still release its pending use.");
            Assert.IsNull(f.Player.MoveToParams);
            f.Player.UpdateObjectPhysics();
            Assert.AreEqual(1, completions, "The completion may not repeat on idle ticks.");
        }

        [TestMethod]
        [DataRow(PlayerKillerStatus.PK)]
        [DataRow(PlayerKillerStatus.NPK)]
        public void VRTriggerProcessesPendingLandingButDoesNotGrantAirCasting(PlayerKillerStatus status)
        {
            using var f = new Fixture();
            f.Player.HandleVRCombat(new VRCombatRequest());
            f.Player.PlayerKillerStatus = status;
            var floor = new Position(f.Player.Location);
            void Airborne(float height)
            {
                f.Player.Location = new Position(floor);
                f.Player.Location.Pos += new Vector3(0,0,height);
                f.Player.PhysicsObj.Position.Frame.Origin = f.Player.Location.Pos;
                f.Player.PhysicsObj.TransientState &= ~(ACE.Server.Physics.TransientStateFlags.OnWalkable | ACE.Server.Physics.TransientStateFlags.Contact);
                f.Player.PhysicsObj.Velocity = new Vector3(0,0,-1);
                Assert.IsTrue(f.Player.IsJumping);
            }
            Airborne(.4f);
            f.Player.LastContact = true;
            f.Player.SetRequestedLocation(new Position(floor));
            f.Player.HandleVRCombat(new VRCombatRequest { Kind=1,Sequence=1,Cell=floor.Cell });
            Assert.IsNull(f.Player.RequestedLocation, "The queued landing is checked before the trigger's airborne precheck.");
            Assert.IsFalse(f.Player.IsJumping, "Server collision must recognize the actual floor contact.");
            Airborne(0);
            f.Player.LastContact = true;
            f.Player.SetRequestedLocation(new Position(floor));
            f.Player.HandleVRCombat(new VRCombatRequest { Kind=1,Sequence=2,Cell=floor.Cell });
            Assert.IsFalse(f.Player.IsJumping, "An unchanged floor position must still refresh stale airborne contact.");
            Airborne(2);
            f.Player.LastContact = true; // A client assertion cannot create support.
            var inAir = new Position(f.Player.Location);inAir.Pos += new Vector3(.01f,0,0);
            f.Player.SetRequestedLocation(inAir);
            f.Player.HandleVRCombat(new VRCombatRequest { Kind=1,Sequence=3,Cell=floor.Cell });
            Assert.IsTrue(f.Player.IsJumping, "An unsupported position must remain unable to cast.");
        }

        [TestMethod]
        public void VRHealthFeedbackStopsAfterObserverLeavesArea()
        {
            using var f = new Fixture();
            bool Nearby() => (bool)typeof(Creature).GetMethod("IsNearbyVRFeedbackObserver", PrivateInstance)
                .Invoke(f.Target, new object[] { f.Player });
            Assert.IsTrue(Nearby());
            f.Player.Location = new Position(0x00640001, 44, 94, 12, 0, 0, 0, 1);
            Assert.IsFalse(Nearby(), "Stale known-player membership must not broadcast health across a portal.");
            f.Player.Location = new Position(f.Target.Location);
            f.Player.Location.Pos += new Vector3(193, 0, 0);
            Assert.IsFalse(Nearby(), "Walking outside the health feedback radius also stops updates.");
        }

        [ClassInitialize]
        public static void Initialize(TestContext context)
        {
            var config = Environment.GetEnvironmentVariable("ACE_TEST_CONFIG");
            var dat = Environment.GetEnvironmentVariable("ACE_TEST_DAT");
            if (string.IsNullOrEmpty(config) || string.IsNullOrEmpty(dat))
                Assert.Inconclusive("Set ACE_TEST_CONFIG and ACE_TEST_DAT to run retail combat integration tests.");
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
            ConfigManager.Initialize(config);
            DatManager.Initialize(dat);
            _ = new ACE.Server.Physics.PhysicsEngine(new ACE.Server.Physics.Common.ObjectMaint(),
                new ACE.Server.Physics.Common.SmartBox()) { Server = true };
            typeof(SocketManager).GetField("listeners", BindingFlags.Static | BindingFlags.NonPublic)
                .SetValue(null, Array.Empty<ConnectionListener>());
        }

        private sealed class Fixture : IDisposable
        {
            public readonly Player Player;
            public readonly Creature Target;
            public readonly List<WorldObject> Objects = new();

            public Fixture(uint targetTemplate = 7, Position start = null, float range = 2)
            {
                Player = new Player(DatabaseManager.World.GetCachedWeenie(1), new ObjectGuid(0x5fffffff), 0);
                Player.Name = "VR regression fixture";
                Player.Location = start ?? new Position(0x7D64000C, 44, 94, 12, 0, 0, 0, 1);
                Player.InitPhysicsObj(); Objects.Add(Player);
                var session = new Session(null, new IPEndPoint(IPAddress.Loopback, 0), 0, 1);
                typeof(Session).GetProperty("Account").SetValue(session, "vr-regression");
                typeof(Player).GetField("<Session>k__BackingField", PrivateInstance).SetValue(Player, session);
                foreach (var skill in new[] { Skill.HeavyWeapons, Skill.LightWeapons, Skill.FinesseWeapons, Skill.MissileWeapons })
                {
                    var value = Player.GetCreatureSkill(skill);
                    value.AdvancementClass = SkillAdvancementClass.Specialized; value.InitLevel = 400;
                }
                // Avoid random loot creation and its persistent GUID allocator.
                var template = System.Text.Json.JsonSerializer.Deserialize<ACE.Entity.Models.Weenie>(
                    System.Text.Json.JsonSerializer.Serialize(DatabaseManager.World.GetCachedWeenie(targetTemplate)));
                template.PropertiesCreateList?.Clear(); template.PropertiesDID.Remove(PropertyDataId.WieldedTreasureType);
                Target = new Creature(template, new ObjectGuid(nextGuid++)); Objects.Add(Target);
                Target.Attackable = true; Target.Location = new Position(Player.Location);
                Target.Location.Pos += new Vector3(0, range, 0); Target.InitPhysicsObj();
                Target.Health.StartingValue = 10000; Target.Health.Current = Target.Health.MaxValue;
                Target.Stamina.Current = 0; // Exhaustion removes random evasion, not damage calculation.
                Player.IgnoreCollisions = Target.IgnoreCollisions = false;
                Player.ReportCollisions = Target.ReportCollisions = true;
                Player.Hidden = Target.Hidden = Target.Ethereal = false;
                Assert.IsTrue(Player.AddPhysicsObj()); Assert.IsTrue(Target.AddPhysicsObj());
                var block = new Landblock(Player.Location.LandblockId);
                var objects = (Dictionary<ObjectGuid, WorldObject>)typeof(Landblock).GetField("worldObjects", PrivateInstance).GetValue(block);
                foreach (var obj in new WorldObject[] { Player, Target })
                {
                    objects.Add(obj.Guid, obj);
                    typeof(WorldObject).GetProperty("CurrentLandblock").SetValue(obj, block);
                }
            }

            public WorldObject Equip(uint template, EquipMask slot, ParentLocation parent = ParentLocation.RightHand)
            {
                var item = WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(template), new ObjectGuid(nextGuid++));
                Objects.Add(item); item.CurrentWieldedLocation = slot; item.ParentLocation = parent;
                item.Wielder = Player; item.WielderId = Player.Guid.Full; Player.EquippedObjects.Add(item.Guid, item);
                return item;
            }

            public void Mode(CombatMode mode) => typeof(Creature).GetProperty("CombatMode").SetValue(Player, mode);

            public void Dispose()
            {
                // Do not invoke world destruction/unloading: those paths can save
                // biotas. These fixtures own only isolated physics objects.
                foreach (var obj in Objects.AsEnumerable().Reverse()) obj.PhysicsObj?.DestroyObject();
            }
        }

        [TestMethod]
        public void RunningCasterAndOwnProjectileIgnoreEachOtherInBothCollisionDirections()
        {
            using var f=new Fixture();
            var shot=FreeArc(f,f.Player.Location.Pos+new Vector3(0,.15f,1),Vector3.UnitY*20);
            var movingBody=new ACE.Server.Physics.Animation.ObjectInfo();
            movingBody.Init(f.Player.PhysicsObj,ACE.Server.Physics.Animation.ObjectInfoState.Default);
            var movingShot=new ACE.Server.Physics.Animation.ObjectInfo();
            movingShot.Init(shot.PhysicsObj,ACE.Server.Physics.Animation.ObjectInfoState.Default);
            Assert.IsTrue(movingBody.MissileIgnore(shot.PhysicsObj));
            Assert.IsTrue(movingShot.MissileIgnore(f.Player.PhysicsObj));
            Assert.IsFalse(movingShot.MissileIgnore(f.Target.PhysicsObj),"Another creature must still receive the projectile.");
            shot.OnCollideObject(f.Player);
            Assert.IsFalse(shot.IsDestroyed,"A late source overlap must not detonate the spell.");
            var state=shot.PhysicsObj.State;
            var velocity=shot.PhysicsObj.Velocity;
            Assert.IsFalse(shot.PhysicsObj.report_object_collision(f.Player.PhysicsObj,false));
            Assert.IsFalse(f.Player.PhysicsObj.report_object_collision(shot.PhysicsObj,false));
            Assert.AreEqual(state,shot.PhysicsObj.State,"A late own-body contact must preserve missile/gravity/path physics.");
            Assert.AreEqual(velocity,shot.PhysicsObj.Velocity);
            var health=f.Target.Health.Current;
            // Backpedal after releasing an overlapping shot; it must keep flying
            // and resolve normal damage against the enemy ahead.
            f.Player.PhysicsObj.Velocity=-Vector3.UnitY*4;
            for (var step=0;step<100 && shot.PhysicsObj.is_active();++step)
            {
                f.Player.PhysicsObj.UpdateObjectInternal(.01);
                shot.PhysicsObj.UpdateObjectInternal(.01);
            }
            Assert.IsTrue(f.Target.Health.Current<health,"Backpedaling cannot consume the shot before it reaches the enemy.");
        }

        [TestMethod]
        public void MovingAcrossOutdoorCellsKeepsCombatWithinTheCurrentTeleportEpoch()
        {
            using var f=new Fixture();
            var r=new VRCombatRequest();
            r.Teleport=BitConverter.ToUInt16(f.Player.Sequences.GetCurrentSequence(ACE.Server.Network.Sequence.SequenceType.ObjectTeleport),0);
            r.Cell=0x7D64000D;
            Assert.IsTrue(f.Player.IsVRRequestCurrent(r));
            r.Cell=0x7D64003C;Assert.IsFalse(f.Player.IsVRRequestCurrent(r));
            r.Cell=f.Player.Location.Cell;r.Teleport++;Assert.IsFalse(f.Player.IsVRRequestCurrent(r));
        }

        [TestMethod]
        public void MeleeCountdownUsesActualRecoveryAndRequiresExplicitSubscription()
        {
            using var f = new Fixture();
            f.Mode(CombatMode.Melee);
            f.Player.CurrentMotionState = new Motion(MotionStance.HandCombat, MotionCommand.Ready);
            f.Target.Location.Pos = f.Player.Location.Pos + new Vector3(0, 1, 0);
            f.Target.PhysicsObj.Position.Frame.Origin = f.Target.Location.Pos;
            object[] Messages()
            {
                var bundles = (IEnumerable)typeof(NetworkSession).GetField("currentBundles", PrivateInstance).GetValue(f.Player.Session.Network);
                return bundles.Cast<object>().Where(b => b != null).SelectMany(b =>
                    ((IEnumerable)b.GetType().GetField("messages", PrivateInstance).GetValue(b)).Cast<object>()).ToArray();
            }
            f.Player.HandleVRCombat(new VRCombatRequest());
            f.Player.HandleVRCombat(new VRCombatRequest { Kind = 4, FeedbackFeatures = 1 });
            f.Player.HandleVRCombat(Swing(f, null, 1, new Vector3(-.3f,1,1), new Vector3(.3f,1,1)));
            Assert.IsFalse(Messages().Any(m => m is ACE.Server.Network.GameEvent.Events.GameEventVRRecovery), "Older clients receive no unknown event.");
            typeof(Player).GetField("vrNextAttack", PrivateInstance).SetValue(f.Player, DateTime.MinValue);
            f.Player.HandleVRCombat(new VRCombatRequest { Kind = 4, FeedbackFeatures = 3 });
            f.Player.HandleVRCombat(Swing(f, null, 2, new Vector3(-.3f,1,1), new Vector3(.3f,1,1)));
            var packet = Messages().OfType<ACE.Server.Network.GameEvent.Events.GameEventVRRecovery>().Single();
            using var wire = new BinaryReader(new MemoryStream(packet.Data.ToArray())); wire.BaseStream.Position = 16;
            Assert.AreEqual(2u, wire.ReadUInt32()); Assert.AreEqual(0u, wire.ReadUInt32());
            var remaining = wire.ReadSingle(); var duration = wire.ReadSingle();
            Assert.AreEqual(duration, remaining); Assert.IsTrue(duration >= .35f);
            var ready = (DateTime)typeof(Player).GetField("vrNextAttack", PrivateInstance).GetValue(f.Player);
            Assert.AreEqual(duration, (float)(ready-DateTime.UtcNow).TotalSeconds, .1f);
            Assert.AreEqual(wire.BaseStream.Length, wire.BaseStream.Position);
            var health = f.Target.Health.Current;
            var warnings = Messages().Count(m => m is ACE.Server.Network.GameEvent.Events.GameEventCommunicationTransientString);
            f.Player.HandleVRCombat(Swing(f, null, 3, new Vector3(-.3f,1,1), new Vector3(.3f,1,1)));
            Assert.AreEqual(health, f.Target.Health.Current, "An early swing does no damage.");
            Assert.AreEqual(2, Messages().OfType<ACE.Server.Network.GameEvent.Events.GameEventVRRecovery>().Count());
            Assert.AreEqual(warnings, Messages().Count(m => m is ACE.Server.Network.GameEvent.Events.GameEventCommunicationTransientString), "Recovery updates replace warning text.");
        }

        [TestMethod]
        public void SelectedMeleePowerChangesRetailDamageMultiplierAndRecovery()
        {
            using var f = new Fixture(); f.Mode(CombatMode.Melee);
            f.Player.CurrentMotionState = new Motion(MotionStance.HandCombat, MotionCommand.Ready);
            f.Target.Location.Pos = f.Player.Location.Pos + new Vector3(0,1,0);
            f.Target.PhysicsObj.Position.Frame.Origin = f.Target.Location.Pos;
            f.Player.HandleVRCombat(new VRCombatRequest());
            var nextAttack = typeof(Player).GetField("vrNextAttack", PrivateInstance);
            var slow = 0.0;
            for (uint i = 1; i <= 2; ++i)
            {
                nextAttack.SetValue(f.Player, DateTime.MinValue);
                var request = Swing(f,null,i,new Vector3(-.3f,1,1),new Vector3(.3f,1,1));
                request.RequestedPower = i == 1 ? 1f : 0f;
                var start = DateTime.UtcNow; f.Player.HandleVRCombat(request);
                Assert.AreEqual(request.RequestedPower.Value, f.Player.PowerLevel);
                Assert.AreEqual(request.RequestedPower.Value + .5f, f.Player.GetPowerMod(null));
                var delay = ((DateTime)nextAttack.GetValue(f.Player) - start).TotalSeconds;
                if (i == 1) slow = delay;
                else Assert.IsTrue(slow > delay + .5, "The same tracked gesture recovers sooner at minimum power.");
            }
        }

        private static VRCombatRequest Swing(Fixture f, WorldObject weapon, uint sequence, Vector3 a, Vector3 b, uint hand = 0)
        {
            using var data = new MemoryStream();
            using var writer = new BinaryWriter(data, Encoding.UTF8, true);
            foreach (var value in new[] { 1u, 2u, sequence, f.Player.Location.Cell, 0u, weapon?.Guid.Full ?? 0u, hand, f.Target.Guid.Full }) writer.Write(value);
            foreach (var value in new[] { a.X, a.Y, a.Z, b.X, b.Y, b.Z, 1f, .1f }) writer.Write(value);
            data.Position = 0; using var reader = new BinaryReader(data);
            Assert.IsTrue(VRCombatRequest.TryRead(reader, out var request)); return request;
        }

        [TestMethod]
        [DataRow(0u)]
        [DataRow(1u)]
        public void TrackedEmptyHandPunchUsesRetailDamageAndRecovery(uint hand)
        {
            using var f = new Fixture();
            f.Mode(CombatMode.Melee);
            f.Player.CurrentMotionState = new Motion(MotionStance.HandCombat, MotionCommand.Ready);
            f.Target.Location.Pos = f.Player.Location.Pos + new Vector3(0, 1, 0);
            f.Target.PhysicsObj.Position.Frame.Origin = f.Target.Location.Pos;
            f.Player.HandleVRCombat(new VRCombatRequest());
            var hit = Swing(f, null, 1, new Vector3(-.3f, 1, 1), new Vector3(.3f, 1, 1), hand);
            var health = f.Target.Health.Current;
            var stamina = f.Player.Stamina.Current;
            f.Player.HandleVRCombat(hit);
            Assert.IsTrue(f.Target.Health.Current < health, "Either empty fist reaches the normal damage pipeline.");
            Assert.AreEqual(AttackType.Punch, f.Player.AttackType, "A strong tracked punch must not use boot/kick damage.");
            Assert.IsTrue(f.Player.Stamina.Current < stamina, "Punches consume retail attack stamina.");
            health = f.Target.Health.Current;
            f.Player.HandleVRCombat(hit);
            f.Player.HandleVRCombat(Swing(f,null,2,hit.Origin,hit.Vector,hand));
            Assert.AreEqual(health, f.Target.Health.Current, "Replay and the other fist cannot bypass recovery.");
            typeof(Player).GetField("vrNextAttack", PrivateInstance).SetValue(f.Player, DateTime.MinValue);
            f.Player.HandleVRCombat(Swing(f,null,3,new Vector3(-.3f,2,1),new Vector3(.3f,2,1),hand));
            Assert.AreEqual(health, f.Target.Health.Current, "A fist cannot use the longer weapon reach.");
            if (hand == 1) f.Equip(350, EquipMask.Shield, ParentLocation.LeftHand);
            else f.Equip(350, EquipMask.MeleeWeapon);
            f.Player.HandleVRCombat(Swing(f,null,4,hit.Origin,hit.Vector,hand));
            Assert.AreEqual(health, f.Target.Health.Current, "An occupied hand cannot claim an unarmed strike.");
        }

        [TestMethod]
        public void TrackedSwingDamagesAnActualCreatureAndRejectsReplayCooldownAndMisses()
        {
            using var f = new Fixture();
            var sword = f.Equip(350, EquipMask.MeleeWeapon); f.Mode(CombatMode.Melee);
            f.Player.CurrentMotionState = new Motion(MotionStance.SwordCombat, MotionCommand.Ready);
            Assert.IsTrue(f.Player.IsDirectVisible(f.Target), "The blade has a clear line to the creature.");
            f.Player.HandleVRCombat(new VRCombatRequest());
            var health = f.Target.Health.Current;
            var hit = Swing(f, sword, 1, new Vector3(-.3f, 2, 1), new Vector3(.3f, 2, 1));
            f.Player.HandleVRCombat(hit);
            Assert.IsTrue(f.Target.Health.Current < health, "A valid tracked sweep must reach normal server damage calculation.");
            health = f.Target.Health.Current; f.Player.HandleVRCombat(hit);
            Assert.AreEqual(health, f.Target.Health.Current, "Replayed contact cannot damage twice.");
            f.Player.HandleVRCombat(Swing(f, sword, 2, new Vector3(-.3f, 2, 1), new Vector3(.3f, 2, 1)));
            Assert.AreEqual(health, f.Target.Health.Current, "A fresh packet cannot bypass weapon recovery.");
            typeof(Player).GetField("vrNextAttack", PrivateInstance).SetValue(f.Player, DateTime.MinValue);
            f.Player.HandleVRCombat(Swing(f, sword, 3, new Vector3(-.3f, -1, 1), new Vector3(.3f, -1, 1)));
            Assert.AreEqual(health, f.Target.Health.Current, "Selecting a creature cannot turn a missed sweep into damage.");
        }

        [TestMethod]
        public void MaceHitsGnawerShrethVisiblePartsOutsideItsMovementCylinder()
        {
            using var db = new ACE.Database.Models.World.WorldDbContext();
            var shrethId = db.WeeniePropertiesString.Where(p => p.Type == (ushort)PropertyString.Name && p.Value == "Gnawer Shreth")
                .Select(p => p.ObjectId).OrderBy(id => id).First();
            var maceId = db.WeeniePropertiesString.Where(p => p.Type == (ushort)PropertyString.Name && p.Value == "Mace")
                .Select(p => p.ObjectId).OrderBy(id => id).First();
            using var f = new Fixture(shrethId);
            var mace = f.Equip(maceId,EquipMask.MeleeWeapon); f.Mode(CombatMode.Melee);
            f.Player.CurrentMotionState = new Motion(MotionStance.SwordCombat,MotionCommand.Ready);
            f.Target.Location.Pos = f.Player.Location.Pos + Vector3.UnitY;
            f.Target.PhysicsObj.Position.Frame.Origin = f.Target.Location.Pos;
            var parts = f.Target.PhysicsObj.PartArray; var frame = parts.Sequence.GetCurrAnimFrame();
            Assert.IsNotNull(frame);
            var best = Vector3.Zero; var distance = 0f;
            for (var i=0; i<parts.Parts.Count && i<frame.Frames.Count; i++)
                foreach (var vertex in parts.Parts[i].GfxObj.VertexArray.Vertices.Values)
                {
                    var point = frame.Frames[i].Origin*parts.Scale + Vector3.Transform(vertex.Origin*parts.Parts[i].GfxObjScale,frame.Frames[i].Orientation);
                    var radial = new Vector2(point.X,point.Y).Length();
                    if (point.Z >= 0 && VRCombatRequest.InReach(point+Vector3.UnitY,2.4f) && radial>distance)
                    { best=point; distance=radial; }
                }
            Console.WriteLine($"Gnawer fixture: weenie={shrethId} setup={f.Target.SetupTableId:X8} motion={f.Target.MotionTableId:X8} scale={f.Target.ObjScale} mace={maceId} maceSetup={mace.SetupTableId:X8} radius={f.Target.PhysicsObj.GetRadius()} visibleContact={best}");
            Assert.IsTrue(distance>f.Target.PhysicsObj.GetRadius()+.18f,"Use visible geometry outside the old cylinder.");
            f.Player.HandleVRCombat(new VRCombatRequest());
            var contact = Vector3.Transform(best,f.Target.PhysicsObj.Position.Frame.Orientation)+Vector3.UnitY;
            var a=contact-new Vector3(.18f,0,0); var b=contact+new Vector3(.18f,0,0);
            var health=f.Target.Health.Current;
            Assert.IsTrue(VRMeleeContact.Intersects(f.Target.PhysicsObj,a-Vector3.UnitY,b-Vector3.UnitY,f.Target.Height,out _),"Authoritative visible-part contact");
            Assert.IsTrue(f.Player.CanDamage(f.Target));
            Assert.IsTrue(f.Player.IsDirectVisible(f.Target));
            Console.WriteLine($"Before hit: busy={f.Player.IsBusy} jumping={f.Player.IsJumping} stamina={f.Player.Stamina.Current} health={health} skill={f.Player.GetEffectiveAttackSkill()}");
            f.Player.HandleVRCombat(Swing(f,mace,1,a,b));
            Console.WriteLine($"After hit: stamina={f.Player.Stamina.Current} health={f.Target.Health.Current} rejected={typeof(Player).GetField("vrLastRejection",PrivateInstance).GetValue(f.Player)}");
            Assert.IsTrue(f.Target.Health.Current<health,"A mace contacting a visible Shreth extremity deals normal damage.");
            health=f.Target.Health.Current;
            f.Player.HandleVRCombat(Swing(f,mace,2,b,a));
            Assert.AreEqual(health,f.Target.Health.Current,"Whole-body contact does not bypass recovery.");
            typeof(Player).GetField("vrNextAttack",PrivateInstance).SetValue(f.Player,DateTime.MinValue);
            f.Player.HandleVRCombat(Swing(f,mace,3,b,a));
            Assert.IsTrue(f.Target.Health.Current<health,"A recovered backhand can hit the same body part.");
            Assert.IsFalse(VRMeleeContact.Intersects(f.Target.PhysicsObj,new Vector3(-.2f,-3,3),new Vector3(.2f,-3,3),f.Target.Height,out _),"A clear miss remains a miss.");
        }

        [TestMethod]
        public void OnlyFreshTrackedVRDropsSkipTheRetailPlacementAnimation()
        {
            using var f = new Fixture();
            f.Player.CurrentMotionState = new Motion(MotionStance.NonCombat,MotionCommand.Ready);
            var start = typeof(Player).GetMethod("StartDropChain",PrivateInstance);
            ACE.Server.Entity.Actions.ActionChain Drop(bool explicitVR = false) => (ACE.Server.Entity.Actions.ActionChain)start.Invoke(f.Player,new object[] { explicitVR });
            f.Player.HandleVRCombat(new VRCombatRequest());
            Assert.IsNotNull(Drop().FirstElement,"A desktop client negotiating capabilities keeps its retail placement delay.");
            f.Player.HandleVRPose(new VRPose { Sequence=1,Cell=f.Player.Location.Cell,Flags=7 });
            Assert.IsNull(Drop().FirstElement,"Currently tracked VR hands release without waiting for a placement animation.");
            typeof(Player).GetField("vrLastTrackedPose",PrivateInstance).SetValue(f.Player,DateTime.UtcNow.AddSeconds(-2));
            Assert.IsNotNull(Drop().FirstElement,"Stale headset tracking cannot leave the retail animation bypass enabled.");
            Assert.IsNull(Drop(true).FirstElement,"An explicit VR release does not depend on the cosmetic pose stream.");
        }

        [TestMethod]
        [DataRow(350u)]
        [DataRow(43952u)]
        [DataRow(273u)]
        public void VRInventoryReleaseFallsFromHandAndStopsAtTheFloor(uint template)
        {
            using var f = new Fixture();
            var item = WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(template), new ObjectGuid(nextGuid++));
            f.Objects.Add(item);
            var release = typeof(Player).GetMethod("TryReleaseVRItem", PrivateInstance);
            Assert.IsTrue((bool)release.Invoke(f.Player, new object[] { item, new Vector3(.6f, .4f, 1.1f) }));
            var initial = item.Location.Pos;
            Console.WriteLine($"VR drop {template}: player={f.Player.Location.Pos} release={initial} state={item.PhysicsObj.State} spheres={item.PhysicsObj.PartArray.GetNumSphere()} velocity={item.PhysicsObj.Velocity}");
            Assert.IsTrue(initial.Z > f.Player.Location.PositionZ + .7f, "The spawn is at hand height, not pre-placed on the floor.");
            Assert.IsTrue(item.IsVRFallingDrop);
            Assert.IsTrue(item.PhysicsObj.Velocity.Z > 0 && item.PhysicsObj.Velocity.Z < 1);
            Assert.AreEqual(.8f, new Vector2(item.PhysicsObj.Velocity.X,item.PhysicsObj.Velocity.Y).Length(), .001f);
            var time = Timers.PortalYearTicks;
            var clock = typeof(Timers).GetProperty(nameof(Timers.PortalYearTicks));
            try
            {
                for (var step = 0; step < 120 && item.IsVRFallingDrop; ++step)
                {
                    clock.SetValue(null, time + (step + 1) * .04);
                    item.UpdateObjectPhysics();
                }
                Assert.IsFalse(item.IsVRFallingDrop, "The ordinary-item tick path must run beyond the first two updates and settle.");
                Assert.IsTrue(item.Location.PositionZ < initial.Z - .5f, "The object visibly falls.");
                Assert.IsTrue(new Vector2(item.Location.Pos.X-initial.X,item.Location.Pos.Y-initial.Y).Length()>.15f, "The release travels outward before settling.");
                Assert.IsTrue(item.Location.PositionZ >= f.Player.Location.PositionZ - .05f, "The floor stops the item.");
                Assert.AreEqual(Vector3.Zero, item.PhysicsObj.Velocity, "Final network position has no residual fall velocity.");
                Assert.IsFalse(item.PhysicsObj.is_active(), "Settled loot does no ongoing drop simulation.");
                Assert.AreSame(f.Player.CurrentLandblock, item.CurrentLandblock);
            }
            finally { clock.SetValue(null, time); }
        }

        [TestMethod]
        public void VRInventoryReleaseCannotPlaceThroughSolidScenery()
        {
            using var f = new Fixture();
            var template = System.Text.Json.JsonSerializer.Deserialize<ACE.Entity.Models.Weenie>(
                System.Text.Json.JsonSerializer.Serialize(DatabaseManager.World.GetCachedWeenie(7)));
            template.PropertiesCreateList?.Clear(); template.PropertiesDID.Remove(PropertyDataId.WieldedTreasureType);
            var wall = new GenericObject(template, new ObjectGuid(nextGuid++)); f.Objects.Add(wall);
            wall.Location = new Position(f.Player.Location); wall.Location.Pos += Vector3.UnitX * .85f;
            f.Player.PhysicsObj.Position.Frame.Origin -= Vector3.UnitX * 5;
            wall.InitPhysicsObj(); wall.Ethereal = wall.IgnoreCollisions = false;
            wall.PhysicsObj.State |= PhysicsState.Static | PhysicsState.ReportCollisionsAsEnvironment;
            Assert.IsTrue(wall.AddPhysicsObj());
            var item = WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(350), new ObjectGuid(nextGuid++));
            f.Objects.Add(item);
            var release = typeof(Player).GetMethod("TryReleaseVRItem", PrivateInstance);
            Assert.IsTrue((bool)release.Invoke(f.Player,new object[] { item,new Vector3(1.7f,0,1f) }));
            Assert.IsTrue(item.Location.PositionX < wall.Location.PositionX,"The item stays on the player's side of the obstruction.");
        }

        [TestMethod]
        [DataRow(13u)]
        [DataRow(4131u)]
        public void LowTrackedSwingsDamageRetailRats(uint template)
        {
            using var f = new Fixture(template);
            var sword = f.Equip(350, EquipMask.MeleeWeapon); f.Mode(CombatMode.Melee);
            f.Player.CurrentMotionState = new Motion(MotionStance.SwordCombat, MotionCommand.Ready);
            f.Player.HandleVRCombat(new VRCombatRequest());
            Assert.IsTrue(f.Target.Height > .5f && f.Target.Height < 1.1f, "Use the scaled retail rat, not a human-sized fixture.");
            var health = f.Target.Health.Current;
            f.Player.HandleVRCombat(Swing(f, sword, 1, new Vector3(-.4f, 2, .25f), new Vector3(.4f, 2, .25f)));
            Assert.IsTrue(f.Target.Health.Current < health, "A low strike through the rat must apply normal melee damage.");
            health = f.Target.Health.Current;
            typeof(Player).GetField("vrNextAttack", PrivateInstance).SetValue(f.Player, DateTime.MinValue);
            f.Player.HandleVRCombat(Swing(f, sword, 2, new Vector3(-.4f, 2, 2.5f), new Vector3(.4f, 2, 2.5f)));
            Assert.AreEqual(health, f.Target.Health.Current, "A swing well above a rat must still miss.");
        }

        [TestMethod]
        [DataRow(1, false)]
        [DataRow(5, false)]
        [DataRow(5, true)]
        public void StationaryMissilesCheckRequestAndActualRelease(int arrows, bool positionOnly)
        {
            using var f = new Fixture();
            var player = f.Player;
            player.Health.StartingValue = player.Stamina.StartingValue = 200;
            player.Health.Current = player.Health.MaxValue;
            player.Stamina.Current = player.Stamina.MaxValue;
            f.Equip(33116, EquipMask.MissileWeapon, ParentLocation.LeftHand);
            var ammo = f.Equip(43952, EquipMask.MissileAmmo);
            ammo.StackSize = arrows;
            ammo.UnlimitedUse = false;
            f.Mode(CombatMode.Missile);
            player.CurrentMotionState = new Motion(MotionStance.BowCombat, MotionCommand.Ready);
            var raw = new Network.Structure.RawMotionState { ForwardCommand = MotionCommand.WalkForward };
            player.CurrentMoveToState.RawMotionState = raw;
            const string property = "missile_block_while_moving";
            var previous = PropertyManager.GetBool(property).Item;
            var time = typeof(Timers).GetProperty(nameof(Timers.PortalYearTicks));
            var start = Timers.PortalYearTicks;
            var queue = (ActionQueue)typeof(Player).GetField("actionQueue", PrivateInstance).GetValue(player);
            void Step()
            {
                time.SetValue(null, Timers.PortalYearTicks + 2);
                WorldManager.DelayManager.RunActions();
                queue.RunActions();
            }
            try
            {
                Assert.IsTrue(PropertyManager.ModifyBool(property, true));
                player.HandleActionTargetedMissileAttack(f.Target.Guid.Full, 2, .5f);
                Assert.IsNull(player.MissileTarget, "A moving client cannot queue the first shot.");

                raw.ForwardCommand = MotionCommand.Ready;
                raw.TurnCommand = MotionCommand.TurnLeft;
                player.HandleActionTargetedMissileAttack(f.Target.Guid.Full, 2, .5f);
                Assert.AreSame(f.Target, player.MissileTarget, "Turning in place must allow aim to start.");
                for (int i = 0; i < 16 && !player.Attacking; ++i) Step();
                Assert.IsTrue(player.Attacking, "The shot reached its launch animation.");
                var stamina = player.Stamina.Current;
                if (positionOnly) player.Location.Pos += Vector3.UnitY;
                else raw.SidestepCommand = MotionCommand.SideStepRight;
                for (int i = 0; i < 24; ++i) Step();
                Assert.AreEqual(arrows, ammo.StackSize);
                Assert.AreEqual(stamina, player.Stamina.Current);
                Assert.AreEqual(CombatMode.Missile, player.CombatMode, "A blocked last arrow must not unequip the bow.");
                Assert.IsFalse(player.Attacking);
                Assert.IsNull(player.MissileTarget, "A rejected release ends the repeat chain.");

                raw.SidestepCommand = MotionCommand.SideStepRight;
                player.MissileTarget = f.Target;
                player.LaunchMissile(f.Target, player.AttackSequence, MotionStance.BowCombat, subsequent: true);
                Assert.IsNull(player.MissileTarget, "Server repeats check movement before charging again.");

                Assert.IsTrue(PropertyManager.ModifyBool(property, false));
                player.HandleActionTargetedMissileAttack(f.Target.Guid.Full, 2, .5f);
                Assert.AreSame(f.Target, player.MissileTarget, "The disabled rule preserves the server's moving-shot behavior.");
                player.OnAttackDone();
                for (int i = 0; i < 4; ++i) Step();
            }
            finally
            {
                time.SetValue(null, start);
                PropertyManager.ModifyBool(property, previous);
            }
        }

        [TestMethod]
        [DataRow(false)]
        [DataRow(true)]
        public void TargetedProjectileCanKillMoarUnderWater(bool castSpell)
        {
            const uint blockId = 0xDC560000;
            var land = ACE.Server.Physics.Common.LScape.get_landblock(blockId | 1);
            Position start = null;
            float surface = 0;
            for (int i = 0; i < 64 && start == null; ++i)
            {
                var point = new Vector3(i / 8 * 24 + 12, i % 8 * 24 + 12, 0);
                var cell = land.GetCell(point);
                float depth = cell.get_water_depth(point);
                if (depth < .85f) continue;
                surface = land.GetZ(point);
                start = new Position(blockId | (uint)(i + 1), point.X, point.Y, surface - depth + .005f, 0, 0, 0, 1);
            }
            Assert.IsNotNull(start, "The coastal block contains a full-depth wading cell.");
            using var f = new Fixture(34869, start, 8); // stock Pallid Moar
            f.Target.Health.Current = 1;
            Assert.IsTrue(f.Target.Location.PositionZ + f.Target.Height < surface, "Entire target must be submerged.");
            if (castSpell)
            {
                f.Mode(CombatMode.Magic);
                var spell = new Spell((uint)SpellId.FlameBolt1);
                var skill = f.Player.GetCreatureSkill(spell.School);
                skill.AdvancementClass = SkillAdvancementClass.Specialized; skill.InitLevel = 500;
                var local = f.Player.CalculateProjectileOrigins(spell, ProjectileSpellType.Bolt, f.Target).Single();
                var bolt = (SpellProjectile)WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(spell.Wcid), new ObjectGuid(nextGuid++));
                f.Objects.Add(bolt); bolt.Setup(spell, ProjectileSpellType.Bolt);
                bolt.Location = new Position(start); bolt.Location.Pos += local; // target due north, identity aim rotation
                bolt.ProjectileSource = f.Player; bolt.ProjectileTarget = f.Target;
                bolt.PhysicsObj.Velocity = f.Player.CalculateProjectileVelocity(spell, f.Target, ProjectileSpellType.Bolt, local);
                bolt.SetProjectilePhysicsState(f.Target, false);
                Assert.IsTrue(bolt.Location.PositionZ > surface && bolt.AddPhysicsObj());
                Assert.IsTrue(f.Player.IsProjectileVisible(bolt));
                for (int i = 0; i < 100 && bolt.PhysicsObj.is_active(); ++i) bolt.PhysicsObj.UpdateObjectInternal(.01);
                Assert.IsFalse(f.Target.IsAlive, "Flame Bolt must cross the waterline and deal lethal damage.");
                return;
            }
            f.Mode(CombatMode.Missile);
            var bow = f.Equip(33116, EquipMask.MissileWeapon, ParentLocation.LeftHand);
            var ammo = f.Equip(43952, EquipMask.MissileAmmo);
            f.Player.AccuracyLevel = 1;
            f.Player.AttackHeight = AttackHeight.Medium;
            float speed = f.Player.GetProjectileSpeed();
            var level = Creature.GetAimLevel(f.Player.GetAimVelocity(f.Target, speed));
            var offset = f.Player.GetProjectileSpawnOrigin(ammo.WeenieClassId, level);
            var velocity = f.Player.CalculateProjectileVelocity(offset, f.Target, speed, out var origin, out var rotation);
            Assert.IsTrue(origin.Z > surface, $"Arrow starts above the waterline (feet {f.Player.Location.Pos}, height {f.Player.Height}, origin {origin}, water {surface}).");
            var arrow = WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(ammo.WeenieClassId), new ObjectGuid(nextGuid++));
            f.Objects.Add(arrow);
            arrow.ProjectileSource = f.Player; arrow.ProjectileTarget = f.Target;
            arrow.ProjectileLauncher = bow; arrow.ProjectileAmmo = ammo;
            arrow.Location = new Position(start) { Pos = origin, Rotation = rotation };
            f.Player.SetProjectilePhysicsState(arrow, f.Target, velocity);
            Assert.IsTrue(arrow.AddPhysicsObj());
            Assert.IsTrue(f.Player.IsProjectileVisible(arrow));
            for (int i = 0; i < 100 && arrow.PhysicsObj.is_active(); ++i) arrow.PhysicsObj.UpdateObjectInternal(.01);
            Assert.IsFalse(f.Target.IsAlive, "Server collision and normal targeted missile damage must kill the submerged Moar.");
        }

        [TestMethod]
        public void UntargetedEquippedArrowFliesIntoCreatureAndDealsNormalMissileDamage()
        {
            using var f = new Fixture();
            var bow = f.Equip(33116, EquipMask.MissileWeapon, ParentLocation.LeftHand);
            var ammo = f.Equip(43952, EquipMask.MissileAmmo); f.Mode(CombatMode.Missile); f.Player.AccuracyLevel = 1;
            WorldObject Launch(Vector3 velocity)
            {
                var arrow = WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(43952), new ObjectGuid(nextGuid++));
                f.Objects.Add(arrow); arrow.ProjectileSource = f.Player; arrow.ProjectileLauncher = bow; arrow.ProjectileAmmo = ammo;
                arrow.ProjectileTarget = null; arrow.IsVRFreeAimProjectile = true;
                arrow.VRMissileAttackSkill = f.Player.GetEffectiveAttackSkill(); arrow.VRMissileAccuracy = 1.6f;
                arrow.Location = new Position(f.Player.Location); arrow.Location.Pos += new Vector3(0, .7f, 1.5f);
                f.Player.SetProjectilePhysicsState(arrow, null, velocity); Assert.IsTrue(arrow.AddPhysicsObj());
                for (var step = 0; step < 100 && arrow.PhysicsObj.is_active(); ++step) arrow.PhysicsObj.UpdateObjectInternal(.01);
                return arrow;
            }
            var health = f.Target.Health.Current;
            var miss = Launch(new Vector3(20, 0, 0));
            Assert.AreEqual(health, f.Target.Health.Current, "An arrow aimed away from the creature must not damage it.");
            Assert.IsNull(miss.ProjectileTarget);
            var hit = Launch(new Vector3(0, 20, 0));
            Assert.AreSame(f.Target, hit.ProjectileTarget, "Physics contact acquires a target without selecting it in advance.");
            Assert.IsTrue(f.Target.Health.Current < health, "An equipped prismatic arrow must apply actual missile damage.");
            Assert.IsFalse(hit.PhysicsObj.is_active(), "The hit projectile stops instead of repeatedly damaging the creature.");
        }

        [TestMethod]
        public void PosesRelayToSubscribedDesktopAndVrObserversButNeverRetail()
        {
            using var f = new Fixture();
            Player Observer(uint guid, bool vr)
            {
                var player = new Player(DatabaseManager.World.GetCachedWeenie(1), new ObjectGuid(guid), 0);
                player.Location = new Position(f.Player.Location); player.InitPhysicsObj(); f.Objects.Add(player);
                var session = new Session(null, new IPEndPoint(IPAddress.Loopback, 0), 0, 1);
                typeof(Player).GetField("<Session>k__BackingField", PrivateInstance).SetValue(player, session);
                if (vr)
                {
                    player.HandleVRCombat(new VRCombatRequest());
                    player.HandleVRPose(new VRPose { Sequence=1, Cell=player.Location.Cell, Flags=7 });
                }
                return player;
            }
            object[] MessagesFor(Player player)
            {
                var bundles = (IEnumerable)typeof(NetworkSession).GetField("currentBundles", PrivateInstance).GetValue(player.Session.Network);
                return bundles.Cast<object>().Where(b => b != null).SelectMany(b =>
                    ((IEnumerable)b.GetType().GetField("messages", PrivateInstance).GetValue(b)).Cast<object>()).ToArray();
            }
            int Messages(Player player) => MessagesFor(player).OfType<ACE.Server.Network.GameEvent.Events.GameEventVRPose>().Count();
            var vr = Observer(0x5ffffffd, true); var retail = Observer(0x5ffffffe, false);
            var desktop = Observer(0x5ffffffc, false);
            var subscribe = new VRCombatRequest { Kind=4,
                FeedbackFeatures=VRCombatRequest.ReceivePoses | VRCombatRequest.ReceiveEquipmentPoses };
            desktop.HandleVRCombat(subscribe);
            Assert.IsFalse((bool)typeof(Player).GetField("vrPoseSubscribed",PrivateInstance).GetValue(desktop),
                "An unnegotiated subscription cannot receive extension events.");
            desktop.HandleVRCombat(new VRCombatRequest()); desktop.HandleVRCombat(subscribe);
            Assert.IsFalse((bool)typeof(Player).GetProperty("HasActiveVRHands",PrivateInstance).GetValue(desktop),
                "Receiving tracking must not switch desktop combat to tracked-hand rules.");
            f.Player.PhysicsObj.ObjMaint.AddKnownPlayers(new[] { vr.PhysicsObj, retail.PhysicsObj, desktop.PhysicsObj });
            f.Player.HandleVRCombat(new VRCombatRequest());
            var pose = new VRPose { Sequence=1, Cell=f.Player.Location.Cell, Flags=7, EyeHeight=1.7575f,
                Positions=new[] { new Vector3(0,0,1.7575f),new Vector3(.35f,-.4f,2.1f),new Vector3(.4f,.35f,1.2f) },
                Rotations=new[] { Quaternion.Identity,Quaternion.Identity,Quaternion.Identity } };
            using var stream = new MemoryStream(); using var writer = new BinaryWriter(stream);
            pose.Write(writer); Assert.AreEqual(VRPose.WireSize, stream.Length);
            stream.Position=0; using var reader=new BinaryReader(stream);
            Assert.IsTrue(VRPose.TryRead(reader,out pose));
            var before=Messages(vr); var retailBefore=Messages(retail);
            pose.Teleport=1; f.Player.HandleVRPose(pose);
            Assert.AreEqual(before,Messages(vr),"A stale location epoch cannot animate an observer's avatar.");
            Assert.AreEqual(0,Messages(desktop));
            pose.Teleport=0; f.Player.HandleVRPose(pose);
            Assert.AreEqual(before+1,Messages(vr),"Validated poses reach a nearby VR observer.");
            Assert.AreEqual(retailBefore,Messages(retail),"Retail clients receive no custom pose opcode.");
            Assert.AreEqual(1,Messages(desktop),"A desktop observer receives tracking without ever transmitting a pose.");
            f.Player.HandleVRPose(pose);
            Assert.AreEqual(before+1,Messages(vr),"Replayed poses are discarded.");
            Assert.AreEqual(1,Messages(desktop));
            // Both the legacy headset and the receive-only desktop can observe
            // a current headset; the former still receives the v1 wire layout.
            pose.Version=2; pose.Sequence=2;
            pose.Rotations[3]=pose.Rotations[4]=Quaternion.Identity;
            typeof(Player).GetField("vrNextPose",PrivateInstance).SetValue(f.Player,DateTime.MinValue);
            f.Player.HandleVRPose(pose);
            Assert.AreEqual(2,Messages(desktop)); Assert.AreEqual(before+2,Messages(vr));
            foreach (var observer in new[] { desktop,vr })
            {
                var bundles=(IEnumerable)typeof(NetworkSession).GetField("currentBundles",PrivateInstance).GetValue(observer.Session.Network);
                var message=bundles.Cast<object>().Where(b=>b!=null).SelectMany(b=>
                    ((IEnumerable)b.GetType().GetField("messages",PrivateInstance).GetValue(b)).Cast<object>())
                    .OfType<ACE.Server.Network.GameEvent.Events.GameEventVRPose>().Last();
                using var payload=new BinaryReader(new MemoryStream(message.Data.ToArray())); payload.BaseStream.Position=20;
                Assert.AreEqual(observer==desktop ? 2u : 1u,payload.ReadUInt32());
            }
            Assert.AreEqual(retailBefore,Messages(retail));

            // VR supplements the normal movement stream; it must never replace
            // or filter it. Verify both kinds of sender against all client types.
            Assert.IsTrue((bool)typeof(Player).GetProperty("HasActiveVRHands",PrivateInstance).GetValue(f.Player));
            foreach (var sender in new[] { f.Player, desktop })
            {
                var observers = new[] { f.Player, desktop, vr, retail }.Where(p => p != sender).ToArray();
                sender.PhysicsObj.ObjMaint.AddKnownPlayers(observers.Select(p => p.PhysicsObj));
                var beforePosition = observers.Select(p => MessagesFor(p).OfType<GameMessageUpdatePosition>().Count()).ToArray();
                var beforeMotion = observers.Select(p => MessagesFor(p).OfType<GameMessageUpdateMotion>().Count()).ToArray();
                sender.SendUpdatePosition();
                sender.EnqueueBroadcastMotion(new Motion(MotionStance.NonCombat, MotionCommand.RunForward), applyPhysics:false);
                byte[] positionBytes = null, motionBytes = null;
                for (var i = 0; i < observers.Length; ++i)
                {
                    var positions = MessagesFor(observers[i]).OfType<GameMessageUpdatePosition>().ToArray();
                    var motions = MessagesFor(observers[i]).OfType<GameMessageUpdateMotion>().ToArray();
                    Assert.AreEqual(beforePosition[i]+1, positions.Length, "Each mode receives ordinary position updates.");
                    Assert.AreEqual(beforeMotion[i]+1, motions.Length, "Each mode receives ordinary animation/movement updates.");
                    positionBytes ??= positions.Last().Data.ToArray(); motionBytes ??= motions.Last().Data.ToArray();
                    CollectionAssert.AreEqual(positionBytes, positions.Last().Data.ToArray());
                    CollectionAssert.AreEqual(motionBytes, motions.Last().Data.ToArray());
                }
            }
            Assert.AreEqual(retailBefore,Messages(retail),"Ordinary compatibility never sends a custom opcode to retail.");
        }

        [TestMethod]
        public void TrackedPoseConsumesValidatedSameCellMovementBeforeRelayingRoot()
        {
            using var f = new Fixture();
            f.Player.HandleVRCombat(new VRCombatRequest());
            var expected = new Position(f.Player.Location);
            expected.Pos += new Vector3(.1f, 0, 0);
            f.Player.SetRequestedLocation(expected);
            var pose = new VRPose { Version=2, Sequence=1, Cell=expected.Cell, Teleport=1, Flags=7 };
            f.Player.HandleVRPose(pose);
            Assert.IsNotNull(f.Player.RequestedLocation, "An old-epoch pose must not consume pending movement.");
            pose.Teleport=0;
            f.Player.HandleVRPose(pose);
            Assert.IsNull(f.Player.RequestedLocation, "Same-cell AutoPos is consumed before publishing tracked feet.");
            Assert.AreEqual(expected.Pos, f.Player.Location.Pos);
            var global=expected.ToGlobal(false);
            Assert.AreEqual(new Vector3(-global.X,global.Y,global.Z),pose.Root,
                "Current hands must not be attached to the previous physics tick's root.");
            Assert.IsFalse(f.Player.InUpdate, "Early physics processing restores its caller's update state.");

            var crossing=new Position(expected);crossing.LandblockId=new LandblockId(expected.Cell+1);
            f.Player.SetRequestedLocation(crossing);
            typeof(Player).GetField("vrNextPose",PrivateInstance).SetValue(f.Player,DateTime.MinValue);
            pose.Sequence++;
            f.Player.HandleVRPose(pose);
            Assert.AreSame(crossing,f.Player.RequestedLocation,"Cell transitions keep the ordinary landblock update ordering.");
        }

        [TestMethod]
        public void MissedSpellsExpireEvenWhenPhysicsIsInactiveWithoutAcquisitionRangeCutoff()
        {
            using var f = new Fixture();
            var spell = new Spell((uint)SpellId.AcidStream1);
            var projectile = (SpellProjectile)WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(spell.Wcid), new ObjectGuid(0x70001000));
            f.Objects.Add(projectile);projectile.Setup(spell, ProjectileSpellType.Bolt);
            projectile.Location = new Position(f.Player.Location);
            projectile.SpawnPos = new Position(projectile.Location);
            projectile.Location.Pos += new Vector3(0, 45, -40);
            projectile.PhysicsObj.set_active(false);
            projectile.UpdateObjectPhysics();
            Assert.IsFalse(projectile.IsDestroyed, "A downhill projectile may travel beyond acquisition range before impact.");
            var timeout = WorldObject.ProjectileTimeout;
            try
            {
                WorldObject.ProjectileTimeout = -1; // Advance beyond the deadline without sleeping.
                projectile.WielderId = f.Player.Guid.Full;
                projectile.UpdateObjectPhysics();
                Assert.IsFalse(projectile.IsDestroyed, "Equipped ammunition is not a missed projectile.");
                projectile.WielderId = null;
                projectile.UpdateObjectPhysics();
                Assert.IsTrue(projectile.IsDestroyed, "Inactive physics must not bypass missed-projectile cleanup.");
            }
            finally { WorldObject.ProjectileTimeout = timeout; }
        }

        [TestMethod]
        public void TrackedCastingUsesOneRecoilQueueAndDiscardsStaleAim()
        {
            using var f = new Fixture(); f.Mode(CombatMode.Magic);
            f.Player.HandleVRCombat(new VRCombatRequest());
            f.Player.IsBusy = true; f.Player.MagicState.CanQueue = true;
            var cast = new VRCombatRequest { Kind=1, Sequence=1, Cell=f.Player.Location.Cell, Subject=(uint)SpellId.FlameBolt1 };
            f.Player.HandleVRCombat(cast);
            Assert.AreSame(cast, f.Player.MagicState.CastQueue?.VRAim);
            f.Player.HandleVRCombat(new VRCombatRequest { Kind=1, Sequence=2, Cell=cast.Cell, Subject=(uint)SpellId.NetherBolt1 });
            Assert.AreSame(cast, f.Player.MagicState.CastQueue?.VRAim, "Spam cannot add a second queued attack.");
            f.Player.IsBusy = false; cast.Teleport = 1;
            f.Player.HandleCastQueue();
            Assert.IsNull(f.Player.MagicState.CastQueue, "The queue is consumed exactly once, including rejected stale aim.");
            Assert.IsFalse(f.Player.IsBusy, "Stale aim must not start a spell after a transition.");
        }

        [TestMethod]
        [DataRow(SpellId.FlameBolt1,false)]
        [DataRow(SpellId.NetherBolt1,false)]
        [DataRow(SpellId.AcidArc1,false)]
        [DataRow(SpellId.FlameBolt1,true)]
        [DataRow(SpellId.NetherBolt1,true)]
        [DataRow(SpellId.AcidArc1,true)]
        public void UntargetedSpellsCollideWithCreatureThroughServerPhysics(SpellId id, bool handNearChest)
        {
            using var f = new Fixture();
            f.Player.Ethereal = false;
            var spell = new Spell((uint)id);
            var skill = f.Player.GetCreatureSkill(spell.School);
            skill.AdvancementClass = SkillAdvancementClass.Specialized; skill.InitLevel = 500;
            f.Target.GetCreatureSkill(Skill.MagicDefense).InitLevel = 0;
            SpellProjectile Launch(Vector3 velocity)
            {
                var p = (SpellProjectile)WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(spell.Wcid), new ObjectGuid(nextGuid++));
                f.Objects.Add(p); p.Setup(spell, SpellProjectile.GetProjectileSpellType(spell.Id));
                p.ProjectileSource = f.Player; p.Location = new Position(f.Player.Location);
                p.IsVRFreeAimProjectile = true;
                p.Location.Pos += new Vector3(0,handNearChest ? 0f : .7f,1.0f); p.PhysicsObj.Velocity = velocity;
                p.SetProjectilePhysicsState(null,p.SpellType == ProjectileSpellType.Arc);
                Assert.IsTrue(p.AddPhysicsObj());
                Assert.IsTrue(f.Player.IsProjectileVisible(p),"A real launch must pass its caster-to-muzzle visibility test.");
                for (var step=0;step<100 && p.PhysicsObj.is_active();++step) p.PhysicsObj.UpdateObjectInternal(.01);
                return p;
            }
            var health = f.Target.Health.Current;
            Launch(new Vector3(20,0,0));
            Assert.AreEqual(health,f.Target.Health.Current,"A free shot aimed away must not acquire or damage a creature.");
            var hit = Launch(new Vector3(0,20,0));
            Assert.IsTrue(f.Target.Health.Current<health,"War/void/arc contacts must run normal damage on an unselected creature.");
            Assert.IsFalse(hit.PhysicsObj.is_active(),"Contact stops the projectile instead of passing through or causing repeated damage.");
        }

        private static SpellProjectile FreeArc(Fixture f, Vector3 origin, Vector3 velocity)
        {
            var spell=new Spell((uint)SpellId.AcidArc1);
            f.Player.GetCreatureSkill(spell.School).AdvancementClass=SkillAdvancementClass.Specialized;
            f.Player.GetCreatureSkill(spell.School).InitLevel=500;
            f.Target.GetCreatureSkill(Skill.MagicDefense).InitLevel=0;
            var p=(SpellProjectile)WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(spell.Wcid),new ObjectGuid(nextGuid++));
            f.Objects.Add(p);p.Setup(spell,ProjectileSpellType.Arc);
            p.ProjectileSource=f.Player;p.IsVRFreeAimProjectile=true;
            p.Location=new Position(f.Player.Location);p.Location.Pos=origin;p.PhysicsObj.Velocity=velocity;
            p.SetProjectilePhysicsState(null,true);Assert.IsTrue(p.AddPhysicsObj());return p;
        }

        [TestMethod]
        public void FreeArcHitsShrethVisibleExtremityOutsideItsMovementCylinder()
        {
            using var f=new Fixture(4108);
            var body=f.Target.PhysicsObj;var parts=body.PartArray;var frame=parts.Sequence.GetCurrAnimFrame();
            var point=Vector3.Zero;float radial=0;
            for(int i=0;i<parts.Parts.Count && i<frame.Frames.Count;++i)
                foreach(var vertex in parts.Parts[i].GfxObj.VertexArray.Vertices.Values)
                {
                    var v=frame.Frames[i].Origin*parts.Scale+Vector3.Transform(vertex.Origin*parts.Parts[i].GfxObjScale,frame.Frames[i].Orientation);
                    var r=new Vector2(v.X,v.Y).Length();
                    if(v.Z>.30f && r>radial) { point=v;radial=r; }
                }
            point=Vector3.Transform(point,body.Position.Frame.Orientation);
            Assert.IsTrue(radial>body.GetRadius()+.1f,"Exercise a visible part outside the old movement cylinder.");
            var p=FreeArc(f,f.Target.Location.Pos+point+Vector3.UnitZ*2,-Vector3.UnitZ*15);
            var before=f.Target.Health.Current;
            for(int i=0;i<100 && p.PhysicsObj.is_active();++i)p.PhysicsObj.UpdateObjectInternal(.01);
            Assert.IsTrue(f.Target.Health.Current<before,"Falling arc contacts visible Shreth body.");
            Assert.IsFalse(p.PhysicsObj.is_active());
            var after=f.Target.Health.Current;p.PhysicsObj.UpdateObjectInternal(.05);
            Assert.AreEqual(after,f.Target.Health.Current,"Impact cannot apply twice.");
        }

        [TestMethod]
        public void OverlappingCreaturesUseFirstSurfaceInFlightDirectionNotEnumerationOrSelection()
        {
            using var f=new Fixture(4108);
            var template=System.Text.Json.JsonSerializer.Deserialize<ACE.Entity.Models.Weenie>(System.Text.Json.JsonSerializer.Serialize(DatabaseManager.World.GetCachedWeenie(4108)));
            template.PropertiesCreateList?.Clear();template.PropertiesDID.Remove(PropertyDataId.WieldedTreasureType);
            var other=new Creature(template,new ObjectGuid(nextGuid++));f.Objects.Add(other);
            other.Location=new Position(f.Target.Location);other.Location.Pos+=Vector3.UnitY*.25f;
            other.InitPhysicsObj();other.IgnoreCollisions=other.Ethereal=other.Hidden=false;
            Assert.IsTrue(other.AddPhysicsObj());
            var start=f.Target.Location.Pos-new Vector3(0,3,-.5f);
            var end=new ACE.Server.Physics.Common.Position(f.Target.Location);end.Frame.Origin+=new Vector3(0,3,.5f);
            var p=FreeArc(f,start,Vector3.UnitY*20);
            Assert.AreSame(f.Target.PhysicsObj,VRProjectileContact.FirstContact(p.PhysicsObj,end,out var first));
            var list=p.PhysicsObj.CurLandblock.ServerObjects;
            list.Reverse();
            Assert.AreSame(f.Target.PhysicsObj,VRProjectileContact.FirstContact(p.PhysicsObj,end,out var reversed));
            Assert.AreEqual(first,reversed);
            p.PhysicsObj.Position.Frame.Origin=end.Frame.Origin;end.Frame.Origin=start;
            Assert.AreSame(other.PhysicsObj,VRProjectileContact.FirstContact(p.PhysicsObj,end,out _),"From behind, the other touching body is physically first.");
            end.Frame.Origin+=Vector3.UnitX*8;p.PhysicsObj.Position.Frame.Origin+=Vector3.UnitX*8;
            Assert.IsNull(VRProjectileContact.FirstContact(p.PhysicsObj,end,out _),"A clear miss cannot home onto either body.");
            p.PhysicsObj.Position.Frame.Origin=start;
            var before=f.Target.Health.Current;var otherBefore=other.Health.Current;
            for(int i=0;i<100 && p.PhysicsObj.is_active();++i)p.PhysicsObj.UpdateObjectInternal(.01);
            Assert.IsTrue(f.Target.Health.Current<before,"The first body's impact reaches normal damage.");
            Assert.AreEqual(otherBefore,other.Health.Current,"A single arc cannot also damage the overlapping body behind it.");
        }

        [TestMethod]
        public void SolidSceneryBeforeCreatureStopsAFreeArc()
        {
            using var f=new Fixture(4108);
            var template=System.Text.Json.JsonSerializer.Deserialize<ACE.Entity.Models.Weenie>(System.Text.Json.JsonSerializer.Serialize(DatabaseManager.World.GetCachedWeenie(7)));
            template.PropertiesCreateList?.Clear();template.PropertiesDID.Remove(PropertyDataId.WieldedTreasureType);
            var obstacle=new GenericObject(template,new ObjectGuid(nextGuid++));f.Objects.Add(obstacle);
            obstacle.Location=new Position(f.Target.Location);obstacle.Location.Pos-=Vector3.UnitY*2;
            f.Player.PhysicsObj.Position.Frame.Origin-=Vector3.UnitX*5;
            obstacle.InitPhysicsObj();obstacle.Ethereal=obstacle.IgnoreCollisions=obstacle.Hidden=false;
            obstacle.PhysicsObj.State|=PhysicsState.Static|PhysicsState.ReportCollisionsAsEnvironment;
            Assert.IsTrue(obstacle.AddPhysicsObj());
            Console.WriteLine($"Obstacle pos={obstacle.PhysicsObj.Position.Frame.Origin} r={obstacle.PhysicsObj.GetRadius()} h={obstacle.PhysicsObj.GetHeight()} flags={obstacle.PhysicsObj.State} spheres={obstacle.PhysicsObj.PartArray.GetNumCylsphere()}");
            var p=FreeArc(f,f.Target.Location.Pos+new Vector3(0,-4,1),Vector3.UnitY*20);
            var before=f.Target.Health.Current;
            for(int i=0;i<100 && p.PhysicsObj.is_active();++i)p.PhysicsObj.UpdateObjectInternal(.02);
            Assert.IsFalse(p.PhysicsObj.is_active());
            Assert.AreEqual(before,f.Target.Health.Current,"A solid object before the body blocks damage.");
        }

        [TestMethod]
        public void TerrainImpactBeforeBodyDoesNotDamageTheCreature()
        {
            using var f=new Fixture(4108);
            // A downward shot reaches the floor long before the creature below it.
            f.Target.PhysicsObj.Position.Frame.Origin-=Vector3.UnitZ*5;
            var p=FreeArc(f,f.Target.Location.Pos+Vector3.UnitZ*2,-Vector3.UnitZ*20);
            var before=f.Target.Health.Current;
            for(int i=0;i<100 && p.PhysicsObj.is_active();++i)p.PhysicsObj.UpdateObjectInternal(.02);
            Assert.IsFalse(p.PhysicsObj.is_active(),"Terrain ends the projectile.");
            Assert.AreEqual(before,f.Target.Health.Current,"The first world obstruction wins over a later body.");
        }

        [TestMethod]
        public void ConfirmedHealthFeedbackIncludesLethalMagicHealingAndNoDuplicateOrLegacyEvents()
        {
            using var f = new Fixture();
            var observer = f.Player;
            f.Target.PhysicsObj.ObjMaint.AddKnownPlayers(new[] { observer.PhysicsObj });
            object[] Messages()
            {
                var bundles=(IEnumerable)typeof(NetworkSession).GetField("currentBundles",PrivateInstance).GetValue(observer.Session.Network);
                return bundles.Cast<object>().Where(b=>b!=null).SelectMany(b=>
                    ((IEnumerable)b.GetType().GetField("messages",PrivateInstance).GetValue(b)).Cast<object>()).ToArray();
            }
            int Numbers()=>Messages().Count(m=>m is ACE.Server.Network.GameEvent.Events.GameEventVRHealthChange);
            observer.HandleVRCombat(new VRCombatRequest { Kind=4,FeedbackFeatures=1 });
            Assert.IsFalse(observer.VRHealthFeedbackSubscribed,"Subscription requires a negotiated client.");
            f.Target.UpdateVitalDelta(f.Target.Health,-10);
            Assert.AreEqual(0,Numbers(),"Retail clients receive no custom event.");
            observer.HandleVRCombat(new VRCombatRequest());
            f.Target.UpdateVitalDelta(f.Target.Health,-10);
            Assert.AreEqual(0,Numbers(),"Older VR clients also receive no custom event.");
            observer.HandleVRCombat(new VRCombatRequest { Kind=4,FeedbackFeatures=1 });
            var actual=f.Target.UpdateVitalDelta(f.Target.Health,-25,3u);
            Assert.AreEqual(-25,actual); Assert.AreEqual(1,Numbers());
            var packet=(ACE.Server.Network.GameEvent.Events.GameEventVRHealthChange)Messages().Last(m=>m is ACE.Server.Network.GameEvent.Events.GameEventVRHealthChange);
            using(var reader=new BinaryReader(new MemoryStream(packet.Data.ToArray())))
            {
                reader.BaseStream.Position=16;
                Assert.AreEqual(f.Target.Guid.Full,reader.ReadUInt32()); Assert.AreEqual(-25,reader.ReadInt32()); Assert.AreEqual(3u,reader.ReadUInt32());
                Assert.AreEqual(reader.BaseStream.Length,reader.BaseStream.Position);
            }
            Assert.AreEqual(45,f.Target.UpdateVitalDelta(f.Target.Health,1000,1u),"Overhealing reports only applied healing.");
            Assert.AreEqual(2,Numbers());
            f.Target.UpdateVitalDelta(f.Target.Health,1000,1u); Assert.AreEqual(2,Numbers(),"Zero effective healing does not create a number.");
            var effects=Messages().Count(m=>m is GameMessageScript);
            var remaining=(int)f.Target.Health.Current;
            Assert.AreEqual(-remaining,f.Target.UpdateVitalDelta(f.Target.Health,-remaining-100,1u));
            Assert.AreEqual(3,Numbers(),"Lethal magic still reports the confirmed damage.");
            Assert.IsTrue(Messages().Count(m=>m is GameMessageScript)>effects,"A lethal magical hit still emits blood.");
            effects=Messages().Count(m=>m is GameMessageScript);
            f.Player.EmitSplatter(f.Target,remaining);
            Assert.IsTrue(Messages().Count(m=>m is GameMessageScript)>effects,"Physical blood is also permitted after health reaches zero.");
            effects=Messages().Count(m=>m is GameMessageScript);f.Player.EmitSplatter(f.Target,0);
            Assert.AreEqual(effects,Messages().Count(m=>m is GameMessageScript),"A zero-damage contact is not blood.");
            // Self-health follows the same route; stamina costs cannot create damage numbers.
            var before=Numbers(); observer.UpdateVitalDelta(observer.Health,-5); Assert.AreEqual(before+1,Numbers());
            observer.UpdateVitalDelta(observer.Stamina,-1); Assert.AreEqual(before+1,Numbers());
        }

        [TestMethod]
        public void EquippingEnchantedArmorAppliesBuffAndEnqueuesActivationEffects()
        {
            using var f = new Fixture();
            var armor = f.Equip(33598, EquipMask.UpperArmWear);
            var spell = armor.Biota.PropertiesSpellBook.Keys.Select(id => new Spell((uint)id)).First(s =>
                s.School == MagicSchool.CreatureEnchantment && (s.CasterEffect != 0 || s.TargetEffect != 0));
            int Effects()
            {
                var bundles = (IEnumerable)typeof(NetworkSession).GetField("currentBundles", PrivateInstance).GetValue(f.Player.Session.Network);
                return bundles.Cast<object>().Where(b => b != null).Sum(b =>
                    ((IEnumerable)b.GetType().GetField("messages", PrivateInstance).GetValue(b)).Cast<object>().Count(m => m is GameMessageScript));
            }
            var before = Effects();
            Assert.IsTrue(f.Player.CreateItemSpell(armor, spell.Id));
            Assert.IsNotNull(f.Player.EnchantmentManager.GetEnchantment(spell.Id, armor.Guid.Full));
            Assert.IsTrue(Effects() > before, "Equip buffs must send PlayEffect messages as well as changing stats.");
        }

        [TestMethod]
        [DataRow(CombatStyle.Bow, AmmoType.Arrow)]
        [DataRow(CombatStyle.Crossbow, AmmoType.Bolt)]
        [DataRow(CombatStyle.Atlatl, AmmoType.Atlatl)]
        public void EachLauncherUsesItsOwnAmmoAndDamagesOnPhysicalContact(CombatStyle style, AmmoType type)
        {
            using var db = new ACE.Database.Models.World.WorldDbContext();
            var weaponId = db.WeeniePropertiesInt.Where(p => p.Type == (ushort)PropertyInt.DefaultCombatStyle && p.Value == (int)style
                && db.WeeniePropertiesInt.Any(a => a.ObjectId == p.ObjectId && a.Type == (ushort)PropertyInt.AmmoType && a.Value == (int)type))
                .Select(p => p.ObjectId).OrderBy(id => id).First();
            var ammoId = db.WeeniePropertiesInt.Where(p => p.Type == (ushort)PropertyInt.AmmoType && p.Value == (int)type
                && db.WeeniePropertiesInt.Any(a => a.ObjectId == p.ObjectId && a.Type == (ushort)PropertyInt.MaxStackSize && a.Value >= 5)
                && db.WeeniePropertiesInt.Any(a => a.ObjectId == p.ObjectId && a.Type == (ushort)PropertyInt.Damage && a.Value > 0))
                .Select(p => p.ObjectId).OrderBy(id => id).First();
            using var f = new Fixture();
            var weapon = f.Equip(weaponId, EquipMask.MissileWeapon);
            var ammo = f.Equip(ammoId, EquipMask.MissileAmmo); ammo.StackSize = 5; ammo.UnlimitedUse = false;
            f.Mode(CombatMode.Missile); f.Player.AccuracyLevel = 1;
            Assert.IsTrue(weapon.IsAmmoLauncher); Assert.AreEqual(weapon.AmmoType, ammo.AmmoType);
            var request = new VRCombatRequest { Amount = 1, Duration = style == CombatStyle.Bow ? .2f : 0 };
            Assert.IsNull(request.MissileReleaseRejection(f.Player.CombatMode, weapon.DefaultCombatStyle));
            var shot = WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(ammoId), new ObjectGuid(nextGuid++));
            f.Objects.Add(shot); shot.ProjectileSource = f.Player; shot.ProjectileLauncher = weapon; shot.ProjectileAmmo = ammo;
            shot.IsVRFreeAimProjectile = true; shot.VRMissileAttackSkill = f.Player.GetEffectiveAttackSkill(); shot.VRMissileAccuracy = 1.6f;
            shot.Location = new Position(f.Player.Location); shot.Location.Pos += new Vector3(0, .7f, 1.5f);
            f.Player.SetProjectilePhysicsState(shot, null, new Vector3(0, 20, 0)); Assert.IsTrue(shot.AddPhysicsObj());
            var health = f.Target.Health.Current;
            for (int i = 0; i < 100 && shot.PhysicsObj.is_active(); ++i) shot.PhysicsObj.UpdateObjectInternal(.01);
            Assert.AreSame(f.Target, shot.ProjectileTarget);
            Assert.IsTrue(f.Target.Health.Current < health, $"{style} ammunition must deal damage.");
            f.Player.UpdateAmmoAfterLaunch(ammo); Assert.AreEqual(4, ammo.StackSize);
        }

        [TestMethod]
        public void IndividuallyThrownWeaponAcquiresPhysicalTargetAndConsumesOneItem()
        {
            using var db=new ACE.Database.Models.World.WorldDbContext();
            var id=db.WeeniePropertiesInt.Where(p=>p.Type==(ushort)PropertyInt.DefaultCombatStyle && p.Value==(int)CombatStyle.ThrownWeapon
                && db.WeeniePropertiesInt.Any(s=>s.ObjectId==p.ObjectId && s.Type==(ushort)PropertyInt.MaxStackSize && s.Value>=5))
                .Select(p=>p.ObjectId).OrderBy(i=>i).First();
            using var f=new Fixture();var item=f.Equip(id,EquipMask.MissileWeapon); item.StackSize=5;
            item.UnlimitedUse=false;
            f.Mode(CombatMode.Missile); f.Player.AccuracyLevel=1;Assert.IsFalse(item.IsAmmoLauncher);
            WorldObject Shot(bool hit)
            {
                var shot=WorldObjectFactory.CreateWorldObject(DatabaseManager.World.GetCachedWeenie(id),new ObjectGuid(nextGuid++));
                f.Objects.Add(shot);shot.ProjectileSource=f.Player;shot.ProjectileAmmo=item;shot.IsVRFreeAimProjectile=true;
                shot.VRMissileAttackSkill=f.Player.GetEffectiveAttackSkill();shot.VRMissileAccuracy=1.6f;
                shot.Location=new Position(f.Player.Location);shot.Location.Pos+=new Vector3(0,.7f,1.5f);
                f.Player.SetProjectilePhysicsState(shot,null,hit ? new Vector3(0,20,0) : new Vector3(20,0,0));Assert.IsTrue(shot.AddPhysicsObj());
                for(int step=0;step<100 && shot.PhysicsObj.is_active();++step)shot.PhysicsObj.UpdateObjectInternal(.01);
                return shot;
            }
            var before=f.Target.Health.Current;Shot(false);Assert.AreEqual(before,f.Target.Health.Current);
            var hit=Shot(true);Assert.AreSame(f.Target,hit.ProjectileTarget);
            Assert.IsTrue(f.Target.Health.Current<before,"A thrown item follows the same physical-hit damage pipeline as an arrow.");
            Assert.IsFalse(hit.PhysicsObj.is_active());
            f.Player.UpdateAmmoAfterLaunch(item);Assert.AreEqual(4,item.StackSize);
            Assert.AreSame(item,f.Player.GetEquippedMissileWeapon(),"The remaining equipped stack stays usable.");
            Assert.IsNull(f.Player.GetEquippedAmmo(),"An individually thrown item never requires arrows.");
        }

        [TestMethod]
        public void MonstersKeepMeleeSpaceOnlyForAnActivelyTrackedVrTarget()
        {
            using var f=new Fixture();f.Target.AttackTarget=f.Player;
            Assert.IsTrue(f.Target.GetMovementParameters().Sticky);
            typeof(Player).GetField("vrLastTrackedPose",PrivateInstance).SetValue(f.Player,DateTime.UtcNow);
            var p=f.Target.GetMovementParameters();Assert.IsFalse(p.Sticky);Assert.AreEqual(.65f,p.DistanceToObject);
            Assert.IsTrue(p.DistanceToObject<Creature.MaxMeleeRange);
            var motion=f.Target.GetMoveToMotion(f.Player,1);
            Assert.AreEqual(p.DistanceToObject,motion.MoveToParameters.DistanceToObject);
            Assert.AreEqual(p.MinDistance,motion.MoveToParameters.MinDistance);
            typeof(Player).GetField("vrLastTrackedPose",PrivateInstance).SetValue(f.Player,DateTime.MinValue);
            Assert.IsTrue(f.Target.GetMovementParameters().Sticky,"Retail or stale tracking retains ordinary movement.");
        }

        [TestMethod]
        public void RingOriginsRemainRadialAndRetailAssetsExist()
        {
            using var f=new Fixture();
            foreach (var id in new[] {1789u,(uint)SpellId.RabbitRing})
            {
                var spell=new Spell(id); var type=SpellProjectile.GetProjectileSpellType(id);
                Assert.AreEqual(ProjectileSpellType.Ring,type);
                var template=DatabaseManager.World.GetCachedWeenie(spell.Wcid);
                var setup=template.PropertiesDID[PropertyDataId.Setup];
                template.PropertiesDID.TryGetValue(PropertyDataId.PhysicsEffectTable,out var table);
                Console.WriteLine($"Ring asset: spell={id} name={spell.Name} wcid={spell.Wcid} setup={setup:X8} pe={table:X8} offset={spell.CreateOffset} num={spell.NumProjectiles}");
                var origins=f.Player.CalculateProjectileOrigins(spell,type,null);
                var center=origins.Aggregate(Vector3.Zero,(a,b)=>a+b)/origins.Count;
                Console.WriteLine($"Ring center={center} origins={string.Join(';',origins)}");
                // Retail's nine-projectile rings repeat the closing 180-degree
                // direction. Test the ring radius, not the centroid of that list.
                var radius=new Vector2(origins[0].X,origins[0].Y).Length();
                Assert.IsTrue(radius>0);
                foreach(var origin in origins)
                    Assert.AreEqual(radius,new Vector2(origin.X,origin.Y).Length(),.001f);
                Assert.IsTrue(origins.Any(p=>p.X<-.1f) && origins.Any(p=>p.X>.1f)
                    && origins.Any(p=>p.Y<-.1f) && origins.Any(p=>p.Y>.1f),"Rings surround the caster on every side.");
                Assert.AreEqual((int)spell.NumProjectiles,origins.Count);
            }
        }

        [TestMethod]
        public void HealthMetersBroadcastIdenticalAuthoritativeFractionsOnlyToSubscribers()
        {
            using var f=new Fixture();
            Player Observer(uint guid,bool subscribe)
            {
                var p=new Player(DatabaseManager.World.GetCachedWeenie(1),new ObjectGuid(guid),0);
                p.Location=new Position(f.Player.Location);p.InitPhysicsObj();f.Objects.Add(p);
                var s=new Session(null,new IPEndPoint(IPAddress.Loopback,0),0,1);
                typeof(Player).GetField("<Session>k__BackingField",PrivateInstance).SetValue(p,s);
                if(subscribe){p.HandleVRCombat(new VRCombatRequest());p.HandleVRCombat(new VRCombatRequest{Kind=4,FeedbackFeatures=8});}
                return p;
            }
            float[] Fractions(Player p)
            {
                var bundles=(IEnumerable)typeof(NetworkSession).GetField("currentBundles",PrivateInstance).GetValue(p.Session.Network);
                return bundles.Cast<object>().Where(b=>b!=null).SelectMany(b=>((IEnumerable)b.GetType().GetField("messages",PrivateInstance).GetValue(b)).Cast<object>())
                    .OfType<ACE.Server.Network.GameEvent.Events.GameEventUpdateHealth>().Select(m=>{
                        using var r=new BinaryReader(new MemoryStream(m.Data.ToArray()));r.BaseStream.Position=16;
                        Assert.AreEqual(f.Target.Guid.Full,r.ReadUInt32());return r.ReadSingle();}).ToArray();
            }
            var a=Observer(0x5ffffffc,true);var b=Observer(0x5ffffffd,true);var legacy=Observer(0x5ffffffe,false);
            f.Target.PhysicsObj.ObjMaint.AddKnownPlayers(new[]{a.PhysicsObj,b.PhysicsObj,legacy.PhysicsObj});
            f.Target.UpdateVitalDelta(f.Target.Health,-2500);
            f.Target.UpdateVitalDelta(f.Target.Health,1000);
            f.Target.UpdateVitalDelta(f.Target.Health,-20000);
            CollectionAssert.AreEqual(Fractions(a),Fractions(b));
            var actual=Fractions(a);Assert.AreEqual(3,actual.Length);
            Assert.AreEqual(.75f,actual[0],.001f);Assert.AreEqual(.85f,actual[1],.001f);Assert.AreEqual(0f,actual[2]);
            Assert.AreEqual(0,Fractions(legacy).Length,"Unsubscribed clients retain normal retail health queries.");
        }

        [TestMethod]
        public void LiveAimRefreshChangesOnlyTheMatchingPendingCast()
        {
            using var f=new Fixture();f.Mode(CombatMode.Magic);
            using var db=new ACE.Database.Models.World.WorldDbContext();
            var wandId=db.WeeniePropertiesInt.Where(p=>p.Type==(ushort)PropertyInt.ItemType && p.Value==(int)ItemType.Caster)
                .Select(p=>p.ObjectId).OrderBy(id=>id).First();
            var wand=f.Equip(wandId,EquipMask.Held);
            f.Player.HandleVRCombat(new VRCombatRequest());f.Player.HandleVRCombat(new VRCombatRequest{Kind=4,FeedbackFeatures=4});
            var spell=new Spell((uint)SpellId.FlameBolt1);
            var cast=new VRCombatRequest{Kind=1,Sequence=1,Cell=f.Player.Location.Cell,Weapon=wand.Guid.Full,Subject=spell.Id,
                Target=f.Target.Guid.Full,Origin=new Vector3(0,.3f,1.5f),Vector=Vector3.UnitY};
            f.Player.MagicState.IsCasting=true;
            f.Player.MagicState.CastSpellParams=new CastSpellParams(spell,null,400,20,f.Target,Player.CastingPreCheckStatus.Success){VRAim=cast};
            f.Player.HandleVRCombat(new VRCombatRequest{Kind=7,Sequence=2,Cell=cast.Cell,Weapon=cast.Weapon,Subject=cast.Subject,Target=1,
                Origin=new Vector3(.2f,.3f,1.5f),Vector=Vector3.UnitX});
            Assert.AreEqual(Vector3.UnitX,cast.Vector,"Release uses the last valid tracked aim, not trigger-time aim.");
            Assert.AreEqual(new Vector3(.2f,.3f,1.5f),cast.Origin);
            Assert.AreEqual(f.Target.Guid.Full,cast.Target);Assert.AreEqual(20u,f.Player.MagicState.CastSpellParams.ManaUsed);
            typeof(Player).GetField("vrLastAim",PrivateInstance).SetValue(f.Player,DateTime.MinValue);
            f.Player.HandleVRCombat(new VRCombatRequest{Kind=7,Sequence=3,Cell=cast.Cell,Weapon=cast.Weapon,Subject=cast.Subject,Target=99,
                Origin=new Vector3(0,0,1.5f),Vector=-Vector3.UnitX});
            Assert.AreEqual(Vector3.UnitX,cast.Vector,"A stale cast id cannot redirect a different spell.");
        }

        [TestMethod]
        public void CloseRangeMeleeToleratesBoundedRenderLagWithoutInventingReach()
        {
            using var f=new Fixture(); var sword=f.Equip(350,EquipMask.MeleeWeapon);f.Mode(CombatMode.Melee);
            f.Player.CurrentMotionState=new Motion(MotionStance.SwordCombat,MotionCommand.Ready);
            f.Target.Location.Pos=f.Player.Location.Pos+new Vector3(0,1,0);
            f.Target.PhysicsObj.Position.Frame.Origin=f.Target.Location.Pos;
            f.Player.HandleVRCombat(new VRCombatRequest());
            var hit=Swing(f,sword,1,new Vector3(-.3f,.3f,1),new Vector3(.3f,.3f,1));
            hit.ObservedBody=new Vector3(0,.3f,0);
            var health=f.Target.Health.Current;f.Player.HandleVRCombat(hit);
            Assert.IsTrue(f.Target.Health.Current<health,"A deliberate point-blank swing reaches the displayed body within bounded interpolation lag.");
            health=f.Target.Health.Current;typeof(Player).GetField("vrNextAttack",PrivateInstance).SetValue(f.Player,DateTime.MinValue);
            var forged=Swing(f,sword,2,new Vector3(-.3f,-1,1),new Vector3(.3f,-1,1));
            forged.ObservedBody=new Vector3(0,-1,0);f.Player.HandleVRCombat(forged);
            Assert.AreEqual(health,f.Target.Health.Current,"An arbitrary reported body position cannot move the server target.");
        }
    }
}
