using System.IO;
using ACE.Entity.Enum;
using ACE.Server.Network.Structure;
using ACE.Server.Network.Enum;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace ACE.Server.Tests
{
    [TestClass]
    public class RawMotionStateTests
    {
        [TestMethod]
        public void MissileTravelUsesTranslationChannelsAndTheirWireSpeedDefaults()
        {
            foreach (var forward in new[] { MotionCommand.WalkForward, MotionCommand.WalkBackwards, MotionCommand.RunForward })
            {
                var motion = new RawMotionState { Flags = RawMotionFlags.ForwardCommand, ForwardCommand = forward };
                Assert.IsTrue(motion.HasTranslation());
                motion.Flags |= RawMotionFlags.ForwardSpeed;
                Assert.IsFalse(motion.HasTranslation());
                motion.ForwardSpeed = .25f;
                Assert.IsTrue(motion.HasTranslation());
            }
            foreach (var side in new[] { MotionCommand.SideStepLeft, MotionCommand.SideStepRight })
            {
                var motion = new RawMotionState { Flags = RawMotionFlags.SideStepCommand, SidestepCommand = side };
                Assert.IsTrue(motion.HasTranslation());
                motion.Flags |= RawMotionFlags.SideStepSpeed;
                Assert.IsFalse(motion.HasTranslation());
                motion.SidestepSpeed = 1;
                Assert.IsTrue(motion.HasTranslation());
            }
            foreach (var forward in new[] { MotionCommand.Invalid, MotionCommand.Ready, MotionCommand.Reload })
            {
                var turning = new RawMotionState { ForwardCommand = forward, TurnCommand = MotionCommand.TurnLeft,
                    Flags = RawMotionFlags.TurnCommand | RawMotionFlags.ForwardCommand };
                Assert.IsFalse(turning.HasTranslation());
            }
        }

        [TestMethod]
        public void RejectedActionsDoNotLeaveAnInvalidCommandCount()
        {
            foreach (var accepted in new[] { false, true })
            {
                using var stream = new MemoryStream();
                using var writer = new BinaryWriter(stream, System.Text.Encoding.UTF8, true);
                writer.Write((uint)(accepted ? 2 : 1) << 11);
                writer.Write(unchecked((ushort)MotionCommand.Point)); // not a client soul emote
                writer.Write((ushort)0x8001); writer.Write(1.0f);
                if (accepted)
                {
                    writer.Write(unchecked((ushort)MotionCommand.PointState));
                    writer.Write((ushort)0x8002); writer.Write(1.0f);
                }
                writer.Write(0x12345678u); // following position field stays aligned
                stream.Position = 0;
                using var reader = new BinaryReader(stream);
                var state = new RawMotionState(new MoveToState(), reader);
                Assert.AreEqual(accepted ? 1 : 0, state.Commands.Count);
                Assert.AreEqual((ushort)state.Commands.Count, state.CommandListLength);
                Assert.AreEqual((uint)state.Commands.Count, state.PackedFlags >> 11);
                Assert.AreEqual(accepted, state.HasSoulEmote(false));
                if (accepted) Assert.AreEqual(MotionCommand.PointState, state.Commands[0].MotionCommand);
                Assert.AreEqual(0x12345678u, reader.ReadUInt32());
                Assert.AreEqual(stream.Length, stream.Position);
            }
        }
    }
}
