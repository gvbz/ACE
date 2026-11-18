using System.Collections.Concurrent;
using System.Net;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using ACE.DatLoader;
using ACE.Entity.Enum;
using ACE.Entity.Enum.Properties;
using ACE.Server.Command;
using ACE.Server.Entity;
using ACE.Server.Mods;
using ACE.Server.Network;
using ACE.Server.Network.Managers;
using ACE.Server.WorldObjects;

static void Require(bool test,string label)
{
    if (!test) throw new Exception(label);
}
if (args.Length != 2 || !Directory.Exists(args[0]) || !Directory.Exists(args[1]))
{
    Console.Error.WriteLine("Usage: LocalSkyPreview.Tests <mod directory> <retail DAT directory>");
    return 2;
}
Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
Environment.SetEnvironmentVariable("ACE_NONINTERACTIVE_CONSOLE","True");
string modDir=Path.GetFullPath(args[0]);
ACE.Common.ConfigManager.Initialize(new ACE.Common.MasterConfiguration());
DatManager.Initialize(Path.GetFullPath(args[1]), false, false);
CommandManager.Initialize();
var module = new ModContainer {FolderPath=modDir, Meta=System.Text.Json.JsonSerializer.Deserialize<ModMetadata>(File.ReadAllText(Path.Combine(modDir,"Meta.json")))};
module.Initialize();
module.Enable();
Require(module.Status == ModStatus.Active,"ACE mod loader");
Require(CommandManager.GetCommandByName("weather").Count()==1,"command registration");
Require(CommandManager.GetCommandByName("timelapse").Count()==1,"timelapse registration");
var stockTime = CommandManager.GetCommandByName("time").Single();
const BindingFlags Private = BindingFlags.NonPublic|BindingFlags.Instance;
typeof(SocketManager).GetField("listeners", BindingFlags.NonPublic|BindingFlags.Static)!.SetValue(null,Array.Empty<ConnectionListener>());
Session NewSession(AccessLevel characterLevel=AccessLevel.Player, AccessLevel accountLevel=AccessLevel.Player)
{
    var session=new Session(new ConnectionListener(IPAddress.Loopback,0),new IPEndPoint(IPAddress.Loopback,12345),1,1);
    // Exercise the real IsAdmin getter and dispatcher without loading or
    // modifying any persisted player/account data.
    var character=(Player)RuntimeHelpers.GetUninitializedObject(typeof(Player));
    var flags=new Dictionary<PropertyBool,bool?> {
        [PropertyBool.IsAdmin]=characterLevel==AccessLevel.Admin,
        [PropertyBool.IsArch]=characterLevel==AccessLevel.Developer,
        [PropertyBool.IsEnvoy]=characterLevel==AccessLevel.Envoy,
        [PropertyBool.IsSentinel]=characterLevel==AccessLevel.Sentinel,
        [PropertyBool.IsAdvocate]=characterLevel==AccessLevel.Advocate };
    typeof(WorldObject).GetProperty("ephemeralPropertyBools",Private)!.SetValue(character,flags);
    typeof(WorldObject).GetProperty(nameof(WorldObject.CurrentLandblock))!.SetValue(character,RuntimeHelpers.GetUninitializedObject(typeof(Landblock)));
    typeof(Session).GetProperty(nameof(Session.Player))!.SetValue(session,character);
    typeof(Session).GetProperty(nameof(Session.AccessLevel))!.SetValue(session,accountLevel);
    return session;
}
var player=NewSession(AccessLevel.Admin,AccessLevel.Admin);
var second=NewSession();
var packets=(ConcurrentQueue<ServerPacket>)typeof(NetworkSession).GetField("packetQueue",Private)!.GetValue(player.Network)!;
var guardedCommands=new[]{"/weather","/weather clear","/weather status","/weather reset","/weather set day",
    "/time","/time set day","/TiMe SeT night","/time reset","@TIME status",
    "/timelapse","/timelapse 100","/timelapse status","/timelapse stop","/timelapse reset"};
CommandHandlerResponse Authorize(Session? session,string text,out CommandHandlerInfo handler)
{
    CommandManager.ParseCommand(text,out var name,out var parameters);
    return CommandManager.GetCommandHandler(session!,name,parameters,out handler);
}
foreach(var role in new[]{AccessLevel.Player,AccessLevel.Advocate,AccessLevel.Sentinel,AccessLevel.Envoy,AccessLevel.Developer})
{
    foreach(var account in new[]{AccessLevel.Player,AccessLevel.Admin})
    {
        var denied=NewSession(role,account);
        foreach(var command in guardedCommands)
            Require(Authorize(denied,command,out _)==CommandHandlerResponse.NotAuthorized,$"denied {role}/{account}: {command}");
        foreach(var command in new[]{"/sudo weather clear","/sudo time","/sudo time set day","/sudo timelapse 100","/SuDo TiMeLaPsE stop"})
            Require(Authorize(denied,command,out _)==CommandHandlerResponse.NotAuthorized,$"no account-level sudo bypass: {command}");
    }
}
foreach(var command in guardedCommands)
{
    Require(Authorize(player,command,out _)==CommandHandlerResponse.Ok,"admin authorized: "+command);
    Require(Authorize(null,command,out _)==CommandHandlerResponse.NotAuthorized,"console is not an admin character: "+command);
}
var withoutCharacter=new Session(new ConnectionListener(IPAddress.Loopback,0),new IPEndPoint(IPAddress.Loopback,12345),1,1);
Require(Authorize(withoutCharacter,"/timelapse",out _)==CommandHandlerResponse.NotAuthorized,"character selection rejected");
foreach(var name in new[]{"weather","timelapse"})
{
    var handler=CommandManager.GetCommandByName(name).Single();
    ((CommandHandler)handler.Handler)(second,new[]{name=="weather"?"rainy":"100"});
    var queue=(ConcurrentQueue<ServerPacket>)typeof(NetworkSession).GetField("packetQueue",Private)!.GetValue(second.Network)!;
    Require(queue.IsEmpty,"direct invocation cannot set a non-admin clock");
}
void Command(string text)
{
    CommandManager.ParseCommand(text,out var name,out var parameters);
    Require(CommandManager.GetCommandHandler(player,name,parameters,out var handler)==CommandHandlerResponse.Ok,"admin dispatch: "+text);
    Require(handler.Attribute.Access==AccessLevel.Admin && handler.Attribute.Flags.HasFlag(CommandHandlerFlag.RequiresWorld),text);
    ((CommandHandler)handler.Handler)(player,parameters);
}
double Pop()
{
    Require(packets.TryDequeue(out var p),"Immediate update missing");
    Require(p!.Header.Flags==(PacketHeaderFlags.EncryptedChecksum|PacketHeaderFlags.TimeSync) && p.Data.Length==8,"invalid immediate packet");
    return BitConverter.ToDouble(p.Data.ToArray());
}
var region=DatManager.PortalDat.RegionDesc;
(double phase,string weather,int group) Read(double ticks)
{
    double date=(ticks+region.GameTime.ZeroTimeOfYear)/region.GameTime.DayLength;
    long day=(long)Math.Floor(date);
    uint hash=unchecked(((uint)day+region.GameTime.ZeroYear*region.GameTime.DaysPerYear)*0x6A42FDB2u-0x7541E9AEu);
    int group=(int)(((ulong)hash*(uint)region.SkyInfo.DayGroups.Count)>>32);
    return (date-day,region.SkyInfo.DayGroups[group].DayName,group);
}
ServerPacket Periodic(Session who,bool ack,bool sync)
{
    var bundleType=typeof(NetworkSession).Assembly.GetType("ACE.Server.Network.NetworkBundle",true)!;
    object bundle=Activator.CreateInstance(bundleType)!;
    bundleType.GetProperty("TimeSync")!.SetValue(bundle,sync);
    bundleType.GetProperty("SendAck")!.SetValue(bundle,ack);
    bundleType.GetProperty("ClientTime")!.SetValue(bundle,120f);
    var packet=new ServerPacket();
    typeof(NetworkSession).GetMethod("WriteOptionalHeaders",Private)!.Invoke(who.Network,new object[]{bundle,packet});
    return packet;
}
double now=Timers.PortalYearTicks;
foreach (string preset in new[]{"clear","sunny","cloudy","rainy"})
{
    Command("/weather "+preset); var forecast=Read(Pop()); Require(forecast.weather.Equals(preset,StringComparison.OrdinalIgnoreCase),preset);
    foreach (var (label,phase) in new[]{("day",.5),("night",.1),("dawn",.21),("dusk",.90)})
    {
        Command("/time set "+label); double target=Pop(); var sky=Read(target);
        Require(sky.group==forecast.group && Math.Abs(sky.phase-phase)<1e-8,preset+label);
        if (preset=="rainy")
        {
            var group=region.SkyInfo.DayGroups[sky.group];
            int curtain=group.SkyObjects.FindIndex(s=>s.DefaultGFXObjectId==0x01004C44);
            var before=group.SkyTime.Last(k=>k.Begin<=phase);
            var after=group.SkyTime.First(k=>k.Begin>phase);
            Require(before.SkyObjReplace.Single(r=>r.ObjectIndex==curtain).Transparent<1 && after.SkyObjReplace.Single(r=>r.ObjectIndex==curtain).Transparent<1,"rain curtain inactive");
        }
        foreach(bool ack in new[]{true,false})
        {
            var body=Periodic(player,ack,true).Data.ToArray();
            var other=Periodic(second,ack,true).Data.ToArray();
            int offset=ack?4:0;
            Require(BitConverter.ToDouble(body,offset)==target && BitConverter.ToDouble(other,offset)==now,"resync or second session");
            Require(body[..offset].SequenceEqual(other[..offset]) && body[(offset+8)..].SequenceEqual(other[(offset+8)..]),"ACK/echo fields");
            Require(Periodic(player,ack,false).Data.ToArray().SequenceEqual(Periodic(second,ack,false).Data.ToArray()),"non-time packet changed");
        }
    }
}
Command("/weather passing"); var passing=Read(Pop()); Require(passing.weather=="Rainy" && Math.Abs(passing.phase-.008)<1e-8,"passing rain");
foreach(var cmd in new[]{"@weather snow","/time set invalid","/weather","/weather status","/weather clear extra"}) Command(cmd);
Require(packets.IsEmpty,"invalid or status command changed time");
foreach(var cmd in new[]{"/time reset","/weather reset"})
{
    Command("/WeAtHeR RAIN"); Pop(); Command(cmd);
    Require(Pop()==now && BitConverter.ToDouble(Periodic(player,false,true).Data.ToArray())==now,"reset");
}
CommandManager.ParseCommand("/time",out string bare,out _);
Require(bare=="time" && CommandManager.GetCommandByName(bare).Single()==stockTime,"native time handler");
Require(Timers.PortalYearTicks==now,"simulation clock modified");

// Advance the server's frame clock deterministically; the mod must only
// transform outgoing samples, never advance the simulation or another session.
var ticksProperty=typeof(Timers).GetProperty(nameof(Timers.PortalYearTicks))!;
void Advance(double seconds) => ticksProperty.SetValue(null,Timers.PortalYearTicks+seconds);
double Sample(Session who) => BitConverter.ToDouble(Periodic(who,false,true).Data.ToArray());
void Near(double actual,double expected,string label) => Require(Math.Abs(actual-expected)<1e-5,label);
Command("/timelapse"); Near(Pop(),now,"timelapse starts without jump");
Advance(10); Near(Sample(player),now+40,"four times elapsed time");
Near(Sample(second),now+10,"timelapse session isolation");
Near(Timers.PortalYearTicks,now+10,"timelapse leaves simulation alone");
Command("/TiMeLaPsE"); Near(Pop(),now+40,"repeated enable preserves clock");
Advance(5); Near(Sample(player),now+60,"repeated enable remains 4x");
Command("/weather clear"); double clearStart=Pop();
Require(Read(clearStart).weather=="Clear","timelapse weather selection");
Advance(10); Near(Sample(player),clearStart+40,"weather preserves 4x");
Command("/time set dusk"); double duskStart=Pop();
Near(Read(duskStart).phase,.9,"timelapse time selection");
Advance(region.GameTime.DayLength*.2/4);
Near(Sample(player),duskStart+region.GameTime.DayLength*.2,"accelerated midnight rollover");
Near(Read(Sample(player)).phase,.1,"next day night phase");
foreach(bool ack in new[]{false,true})
{
    var body=Periodic(player,ack,true).Data.ToArray();
    var other=Periodic(second,ack,true).Data.ToArray();
    int offset=ack?4:0;
    Require(body[..offset].SequenceEqual(other[..offset]) && body[(offset+8)..].SequenceEqual(other[(offset+8)..]),"timelapse preserves ACK and echo");
}
foreach(var invalid in new[]{"/timelapse fast","/timelapse 0","/timelapse -4","/timelapse NaN","/timelapse Infinity","/timelapse 1e309","/timelapse stop extra","/timelapse reset extra","/timelapse status","/weather status"}) Command(invalid);
Require(packets.IsEmpty,"timelapse invalid or status mutated clock");
double running=Sample(player);
Advance(2); Near(Sample(player),running+8,"invalid command leaves rate intact");

// Invoke the actual patched Update with sending disabled, so no socket is
// needed. Its scheduling must shorten only a timelapse session's sync deadline.
var resyncField=typeof(NetworkSession).GetField("nextResync",Private)!;
void CheckSyncSchedule(Session who,bool accelerated)
{
    var deadline=DateTime.UtcNow.AddSeconds(20);
    resyncField.SetValue(who.Network,deadline);
    typeof(NetworkSession).GetField("nextSend",Private)!.SetValue(who.Network,DateTime.MaxValue);
    who.Network.sendResync=true;
    who.Network.Update();
    var scheduled=(DateTime)resyncField.GetValue(who.Network)!;
    Require(accelerated ? scheduled<deadline.AddSeconds(-19) : scheduled==deadline,"timelapse sync cadence");
    who.Network.sendResync=false;
}
CheckSyncSchedule(player,true);
CheckSyncSchedule(second,false);
running=Sample(player);
Command("/timelapse stop"); Near(Pop(),running,"stop without jump");
Advance(10); Near(Sample(player),running+10,"stop resumes 1x");
CheckSyncSchedule(player,false);
foreach(double rate in new[]{.125,4.5,64,240,241,1e3,10000,1})
{
    double start=Sample(player);
    Command("/timelapse "+rate.ToString(System.Globalization.CultureInfo.InvariantCulture)); Near(Pop(),start,"speed change without jump");
    Advance(2); Near(Sample(player),start+rate*2,"custom timelapse speed");
    CheckSyncSchedule(player,rate!=1);
    Command("/time set dusk"); start=Pop();
    Advance(1); Near(Sample(player),start+rate,"time selection preserves custom speed");
}
running=Sample(player);
Command("/timelapse"); Near(Pop(),running,"default rate without jump");
Advance(1); Near(Sample(player),running+4,"default stays 4x after custom speed");
Command("/timelapse 1e308"); Pop(); Advance(10);
Require(double.IsFinite(Sample(player)) && Sample(player)>=0 && Sample(player)<region.GameTime.DayLength*(double)(1L<<30),"extreme rate keeps a valid client calendar");
foreach(var reset in new[]{"/weather reset","/time reset","/timelapse reset"})
{
    Command("/timelapse"); Pop(); Advance(2); Command(reset);
    Near(Pop(),Timers.PortalYearTicks,"timelapse reset immediate");
    Advance(3); Near(Sample(player),Timers.PortalYearTicks,"reset leaves live rate");
    CheckSyncSchedule(player,false);
}
ticksProperty.SetValue(null,now);
Command("/weather rainy"); Pop();
Command("/timelapse"); Pop();
module.Disable();
Require(Authorize(NewSession(AccessLevel.Envoy),"/time",out _)==CommandHandlerResponse.Ok,"unload restores native time permissions");
Require(!CommandManager.GetCommandByName("weather").Any() && CommandManager.GetCommandByName("time").Single()==stockTime,"unload");
Require(!CommandManager.GetCommandByName("timelapse").Any(),"timelapse unload");
Require(BitConverter.ToDouble(Periodic(player,false,true).Data.ToArray())==now,"unload clock");
Console.WriteLine("PASS: admin-character-only dispatch, non-admin roles and admin-account sudo rejected, aliases/status/stop/reset and direct-call checks; 16 forecasts/lighting pairs; fractional/custom/extreme rates; continuity, midnight, sync cadence, session isolation, ACK/echo preservation and mod load/unload.");
return 0;
