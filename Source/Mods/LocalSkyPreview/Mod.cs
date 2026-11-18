using System.Buffers.Binary;
using System.Globalization;
using System.Runtime.CompilerServices;
using ACE.DatLoader;
using ACE.DatLoader.FileTypes;
using ACE.Entity.Enum;
using ACE.Server.Command;
using ACE.Server.Network.GameMessages.Messages;
using ACE.Server.Entity;
using ACE.Server.Mods;
using ACE.Server.Network;
using HarmonyLib;

namespace LocalSkyPreview;

public sealed class Mod : IHarmonyMod
{
    private readonly Harmony patches = new("local.ace.sky-preview");
    private sealed record Preview(double ServerTicks, double SkyTicks, double Rate = 1)
    {
        public double At(double now)
        {
            // Keep even enormous user rates representable by the client's
            // int32 day index, without an arbitrary multiplier ceiling.
            double span = (DatManager.PortalDat?.RegionDesc.GameTime.DayLength ?? 7620) * (double)(1L << 30);
            double elapsed = now - ServerTicks;
            double advance = elapsed * Rate;
            if (!double.IsFinite(advance) || Math.Abs(advance) >= span)
                advance = (elapsed % (span / Rate)) * Rate;
            double ticks = (SkyTicks + advance) % span;
            return ticks < 0 ? ticks + span : ticks;
        }
    }
    private static readonly ConditionalWeakTable<NetworkSession, Preview> previews = new();
    private const string TimelapseHelp = "/timelapse [multiplier | status | stop | reset] (any positive number; default: 4x).";
    private const string Help = "/weather clear | sunny | cloudy | rainy | passing | reset. /time set dawn | day | dusk | night. " + TimelapseHelp;

    public void Initialize() => patches.PatchAll(typeof(Mod).Assembly);

    public void Dispose()
    {
        patches.UnpatchAll(patches.Id);
        previews.Clear();
    }

    // Keep the native /time display; the shared access guard also covers it.
    [HarmonyPatch(typeof(CommandManager), nameof(CommandManager.ParseCommand))]
    private static class TimeAlias
    {
        static void Postfix(ref string command, string[] parameters)
        {
            if (string.Equals(command, "time", StringComparison.OrdinalIgnoreCase)
                && parameters.Length > 0
                && (parameters[0].Equals("set", StringComparison.OrdinalIgnoreCase)
                    || parameters[0].Equals("reset", StringComparison.OrdinalIgnoreCase)))
                command = "weather";
        }
    }

    // Require the character's flag, not merely an admin account. ACE's normal
    // sudo path can bypass character permissions, so reject that path too.
    [HarmonyPatch(typeof(CommandManager), nameof(CommandManager.GetCommandHandler))]
    private static class AdminCharacterOnly
    {
        static bool Prefix(Session session, string command, string[] parameters,
            ref CommandHandlerInfo commandInfo, ref CommandHandlerResponse __result)
        {
            string target = command;
            if (string.Equals(target, "sudo", StringComparison.OrdinalIgnoreCase) && parameters?.Length > 0)
                target = parameters[0];
            if (!(string.Equals(target, "weather", StringComparison.OrdinalIgnoreCase)
                || string.Equals(target, "time", StringComparison.OrdinalIgnoreCase)
                || string.Equals(target, "timelapse", StringComparison.OrdinalIgnoreCase))
                || session?.Player?.IsAdmin == true)
                return true;
            commandInfo = null!;
            __result = CommandHandlerResponse.NotAuthorized;
            return false;
        }
    }

    private static bool RequireAdminCharacter(Session session)
    {
        if (session?.Player?.IsAdmin == true) return true;
        if (session != null)
            session.Network.EnqueueSend(new GameMessageSystemChat("Sky preview commands require an admin character.", ChatMessageType.Broadcast));
        return false;
    }

    // ACE synchronizes the client every 20 seconds. An ACK, if present,
    // precedes the double in the optional headers. Echo data follows it.
    [HarmonyPatch(typeof(NetworkSession), "WriteOptionalHeaders")]
    private static class PeriodicClock
    {
        static void Postfix(NetworkSession __instance, ServerPacket packet)
        {
            if (!packet.Header.HasFlag(PacketHeaderFlags.TimeSync) || !previews.TryGetValue(__instance, out var preview))
                return;
            int offset = packet.Header.HasFlag(PacketHeaderFlags.AckSequence) ? sizeof(uint) : 0;
            var timestamp = packet.Data.GetBuffer().AsSpan(offset, sizeof(double));
            BinaryPrimitives.WriteDoubleLittleEndian(timestamp, preview.At(BinaryPrimitives.ReadDoubleLittleEndian(timestamp)));
        }
    }

    // Clients extrapolate clock samples at 1x. Update accelerated previews every
    // 100ms instead of making large clock jumps every 20 seconds.
    // Reuse ACE's authenticated, sequenced sync packets; gameplay/echo clocks
    // and sessions without timelapse retain their normal timing.
    [HarmonyPatch(typeof(NetworkSession), nameof(NetworkSession.Update))]
    private static class TimelapseSync
    {
        static void Prefix(NetworkSession __instance, ref DateTime ___nextResync)
        {
            if (!__instance.sendResync || !previews.TryGetValue(__instance, out var preview) || preview.Rate == 1)
                return;
            var deadline = DateTime.UtcNow.AddMilliseconds(100);
            if (___nextResync > deadline) ___nextResync = deadline;
        }
    }

    private static int ForecastIndex(RegionDesc region, long day)
    {
        uint seed = unchecked((uint)day + region.GameTime.DaysPerYear * region.GameTime.ZeroYear);
        uint hash = unchecked(0x6A42FDB2u * seed - 0x7541E9AEu);
        return (int)(((ulong)hash * (uint)region.SkyInfo.DayGroups.Count) >> 32);
    }

    private static void SyncNow(NetworkSession network, double ticks)
    {
        var packet = new ServerPacket();
        packet.Header.Flags = PacketHeaderFlags.TimeSync | PacketHeaderFlags.EncryptedChecksum;
        packet.InitializeDataWriter();
        packet.DataWriter.Write(ticks);
        network.EnqueueSend(packet);
    }

    [CommandHandler("timelapse", AccessLevel.Admin, CommandHandlerFlag.RequiresWorld, 0,
        "Accelerate this session's sky clock for testing.", TimelapseHelp)]
    public static void Timelapse(Session session, params string[] args)
    {
        if (!RequireAdminCharacter(session)) return;
        if (args.Length == 1 && args[0].Equals("reset", StringComparison.OrdinalIgnoreCase))
        {
            Weather(session, "reset");
            return;
        }
        if (args.Length == 1 && args[0].Equals("status", StringComparison.OrdinalIgnoreCase))
        {
            Weather(session, "status");
            return;
        }
        bool stop = args.Length == 1 && args[0].Equals("stop", StringComparison.OrdinalIgnoreCase);
        double rate = 4;
        if (args.Length != 0 && !stop && (args.Length != 1
            || !double.TryParse(args[0], NumberStyles.Float, CultureInfo.InvariantCulture, out rate)
            || !double.IsFinite(rate) || rate <= 0))
        {
            session.Network.EnqueueSend(new GameMessageSystemChat(TimelapseHelp, ChatMessageType.Broadcast));
            return;
        }
        var net = session.Network;
        var now = Timers.PortalYearTicks;
        previews.TryGetValue(net, out var current);
        double ticks = current?.At(now) ?? now;
        if (stop) rate = 1;
        previews.AddOrUpdate(net, new Preview(now, ticks, rate));
        SyncNow(net, ticks);
        double dayLength = DatManager.PortalDat?.RegionDesc.GameTime.DayLength ?? 0;
        double cycle = dayLength / rate;
        string duration = dayLength > 0 ? FormattableString.Invariant($" A full day/night cycle takes {cycle:G5} seconds.") : "";
        session.Network.EnqueueSend(new GameMessageSystemChat(stop
            ? "Timelapse stopped. Sky time continues at 1x from here. /weather reset restores live time."
            : FormattableString.Invariant($"Timelapse enabled: sky time runs at {rate:G}x.{duration} /timelapse status shows progress; /timelapse stop resumes 1x; /weather reset restores live time."), ChatMessageType.Broadcast));
    }

    [CommandHandler("weather", AccessLevel.Admin, CommandHandlerFlag.RequiresWorld, 0,
        "Preview the outdoor sky for this session.", Help)]
    public static void Weather(Session session, params string[] args)
    {
        if (!RequireAdminCharacter(session)) return;
        void Reply(string message) => session.Network.EnqueueSend(new GameMessageSystemChat(message, ChatMessageType.Broadcast));
        var net = session.Network;
        var now = Timers.PortalYearTicks;
        if (args.Length == 1 && args[0].Equals("reset", StringComparison.OrdinalIgnoreCase))
        {
            previews.Remove(net);
            SyncNow(net, now);
            Reply("Sky preview ended. Using the live server clock and forecast.");
            return;
        }

        var region = DatManager.PortalDat?.RegionDesc;
        if (region == null || region.SkyInfo.DayGroups.Count == 0 || region.GameTime.DayLength <= 0)
        {
            Reply("The server DAT has no sky calendar available.");
            return;
        }
        bool active = previews.TryGetValue(net, out var current);
        double length = region.GameTime.DayLength;
        double rate = current?.Rate ?? 1;
        double date = ((current?.At(now) ?? now) + region.GameTime.ZeroTimeOfYear) / length;
        long day = (long)Math.Floor(date);
        double phase = date - day;
        var groups = region.SkyInfo.DayGroups;
        if (args.Length == 0 || (args.Length == 1 && args[0].Equals("status", StringComparison.OrdinalIgnoreCase)))
        {
            Reply(FormattableString.Invariant($"{(active ? "Preview" : "Live")} sky: {groups[ForecastIndex(region, day)].DayName}, {phase:P2} through the day, {rate:G}x speed. {Help}"));
            return;
        }

        if (args.Length == 2 && args[0].Equals("set", StringComparison.OrdinalIgnoreCase))
        {
            phase = args[1].ToLowerInvariant() switch
            {
                "dawn" => 0.21,
                "day" or "noon" => 0.50,
                "dusk" => 0.90,
                "night" => 0.10,
                _ => -1
            };
            if (phase < 0) { Reply(Help); return; }
        }
        else if (args.Length == 1)
        {
            var mode = args[0].ToLowerInvariant();
            string? wanted = mode switch
            {
                "rain" or "rainy" or "storm" or "passing" => "Rainy",
                "clear" => "Clear",
                "sunny" => "Sunny",
                "cloudy" => "Cloudy",
                _ => null
            };
            if (wanted == null) { Reply(Help); return; }
            // In the retail DAT the first Rainy profile has a storm from
            // 0.02 to 0.96. Later Rainy profiles can be completely dry at noon.
            int wantedGroup = groups.FindIndex(g => string.Equals(g.DayName, wanted, StringComparison.OrdinalIgnoreCase));
            long lastDay = day + 512;
            while (wantedGroup >= 0 && day < lastDay && ForecastIndex(region, day) != wantedGroup) ++day;
            if (wantedGroup < 0 || day == lastDay) { Reply($"No {wanted} forecast found in the server DAT."); return; }
            if (mode == "passing") phase = 0.008; // rising edge of the storm, for comparing the belt
            else if (wanted == "Rainy") phase = Math.Clamp(phase, 0.03, 0.95);
        }
        else { Reply(Help); return; }

        double ticks = (day + phase) * length - region.GameTime.ZeroTimeOfYear;
        previews.AddOrUpdate(net, new Preview(now, ticks, rate));
        SyncNow(net, ticks);
        Reply(FormattableString.Invariant($"Sky preview: {groups[ForecastIndex(region, day)].DayName}, {phase:P2} through the day, {rate:G}x speed. /weather reset restores live time."));
    }
}
