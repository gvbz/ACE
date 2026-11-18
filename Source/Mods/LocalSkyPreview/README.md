# Admin sky preview commands

Optional ACE server mod for testing classic and imported skies. Commands affect
only the invoking character's client sky clock and forecast. They do not change
the server simulation clock, spell timers, or other players' sessions.

The character must have `IsAdmin` enabled. An admin account using a non-admin
character, including through `sudo`, cannot use these commands. While the mod is
enabled, this restriction also covers the native `/time` display, status queries,
reset/stop commands, and the `@` command prefix.

## Build and install

Requires the same .NET 10 SDK as ACE. From the repository root:

```powershell
dotnet build Source/Mods/LocalSkyPreview/LocalSkyPreview.csproj -c Release
```

The build stages `LocalSkyPreview.dll` and `Meta.json` in
`Source/Mods/LocalSkyPreview/bin/Release/net10.0/LocalSkyPreview/`.
Copy that entire `LocalSkyPreview` directory into the server's configured
`ModsDirectory` (defaults to `Mods` beside the server executable). Restart the
server to load the DLL. When updating, stop the server cleanly before replacing
the files; `/mod find` alone does not reliably replace an already loaded assembly.

To build against an existing server distribution instead of this checkout, add
`-p:ServerBin="C:/path/to/server"`. This directory must contain the ACE assemblies
and their Harmony/log4net dependencies. Do not copy the server dependencies into
the mod folder; the server supplies them.

To disable the commands, set `Enabled` to `false` in the installed `Meta.json` and
restart the server. The native `/time` permissions then return to normal.

## Commands

- `/weather clear`, `/weather sunny`, `/weather cloudy`, `/weather rainy` select a
  matching retail forecast. `rain` and `storm` are aliases for `rainy`.
- `/weather passing` previews the beginning of a rainstorm.
- `/time set dawn`, `/time set day` (or `noon`), `/time set dusk`, and
  `/time set night` select a time while retaining the selected forecast and speed.
- `/timelapse` runs at 4x. `/timelapse 64`, `/timelapse 1000`, or
  `/timelapse 0.5` set any finite positive multiplier, including fractional rates.
  Enter decimal values with a period. Zero, negative, NaN, and infinite rates are
  rejected. Very large rates can skip visible transitions between rendered frames.
- `/weather status` or `/timelapse status` reports the current forecast, day
  fraction, and speed.
- `/timelapse stop` resumes 1x from the current preview time.
- `/weather reset`, `/time reset`, or `/timelapse reset` restores the live server
  clock and forecast. Preview state also ends when the network session ends.

Time continues advancing after a weather selection, so the forecast can change
at the next day boundary. Accelerated sessions receive clock samples every 100ms.
AC Unreal uses that clock for sky colors, sun/moon positions, cloud motion, and
imported cloud shadows; falling precipitation keeps its normal visual speed.

## Regression checks

The checks use the real ACE dispatcher, mod loader, packet writer, and retail
DAT calendar with in-memory characters. They require no server credentials,
database, or running server. From the repository root:

```powershell
dotnet run --project Source/Mods/LocalSkyPreview.Tests/LocalSkyPreview.Tests.csproj -c Release -- Source/Mods/LocalSkyPreview/bin/Release/net10.0/LocalSkyPreview "C:/path/to/retail/DATs"
```

When testing an installed server ABI, pass the same `-p:ServerBin=...` argument
used to build the mod. Checks cover admin-character permissions, lower staff
ranks, account-level `sudo`, direct handler calls, forecast/time combinations,
custom rates, midnight rollover, sync cadence, session isolation, preserved
ACK/echo fields, and restoring normal behavior when the mod unloads.
