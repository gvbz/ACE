# AC:VR — native Quest client

This folder contains the Quest configuration and reproducible build/install tools.
Shared client code lives in `../Unreal`. `Build-Quest.ps1` calls
`Sync-ClientSource.ps1` to populate the ignored `Project/Source`, `Project/Plugins`,
and `Project/Content` folders from the canonical source. Do not edit or commit those
generated copies. Runtime UI JSON and test fixtures are included in synchronization.

See [source checkout setup](../SOURCE_CHECKOUT.md) for .NET, Unreal, SDK/NDK, JDK,
server setup, and Windows builds. Build Windows first if runtime materials changed.

In PowerShell 7:

```powershell
.\Build-Quest.ps1
.\Sync-ClientSource.ps1 -CheckOnly
```

The APK is written to `Packaged/Android_ASTC/ACUnreal-arm64.apk`. The application ID
is `com.acecommunity.questtest`, displayed as **AC:VR**. Updates preserve existing installations and encrypted login data. The build checks
the APK version against `Project/Config/DefaultEngine.ini` before reporting success.

Put the Android toolchain in the ignored `AndroidSDK` directory, or pass
`-AndroidSdkRoot <path>` to the build script. Installation/test/sharing helpers
expect `AndroidSDK/platform-tools/adb.exe` and `AndroidSDK/build-tools/35.0.1/aapt.exe`;
none of these external tools are checked in.

With Developer Mode enabled and USB debugging accepted in the headset:

```powershell
.\Install-Quest.ps1 -DatDirectory "C:\Turbine\Asheron's Call"
# Later updates, preserving installed game data and settings:
.\Install-Quest.ps1 -SkipData
```

Keep the headset awake when launching. Open **Apps > Unknown Sources > AC:VR**. Enter an ACE server address reachable from the headset; the server runs on
the PC, not inside the Quest. Configure your own local server credentials separately.
The guarded `Start-Server.ps1` helper expects a configured published server at
`../Unreal/PackagedVR/Server` and avoids starting a second server on occupied UDP ports.

`Build-SharePackage.ps1` creates a versioned installer ZIP from the current APK and
the scripts in `Sharing`. Each recipient supplies retail DAT files and Android
platform-tools. It preserves earlier archives and refuses to overwrite a release.

`Test-Quest.ps1` runs headset startup checks with the app visible. Installer tests
live in `Tests`; native client automation lives in `../Unreal/Plugins/ACEClient/Source`.
Successful builds and automation do not replace an in-headset gameplay check.

For a black screen/ANR before login, run `Sharing/Diagnose-Quest.ps1` with the
headset connected immediately after the failure. It works with existing v152
installs and needs no APK/manifest. The generated ZIP contains app-scoped logs,
retained Android exit/ANR evidence, version/storage/wake information and DAT
sizes, without modifying the app or copying credentials/configs. ADB reads have
bounded timeouts; missing/unsupported diagnostic commands are recorded without
discarding the rest of the report. `Tests/Test-QuestDiagnostics.ps1` exercises
the collector under Windows PowerShell, including stalled ADB and absent data.

The November 10 startup review verified the released v152 APK's `AC:VR` label,
launcher activity and ARM64 ABI. DAT indexing now rejects cyclic/shared directory
pointers and excessive tree depth, clears partial indexes on failure, and rejects
impossible resource lengths before allocation. Header/sector reads also check
physical file bounds before seeking, including packaged Windows' buffered reader.
This closes corrupt-data crash
path; it does not establish the cause of the reported Quest 3 v152 black screen.
Win64 Editor compilation and `ACE.DAT.DirectoryIntegrity`,
`ACE.RetailParity.MissingDatLogin`, and `ACE.VR.RigAndMenus` passed, as did nine
Windows PowerShell diagnostic-helper cases. The affected headset's startup/ANR
capture and native hardware retest remain necessary.

Landscape textures retain the retail 1024-pixel TexMerge output, uncompressed
BGRA8 pixels and full mipmaps. `ace.Texture.MaxWorldSize` applies to object
textures, not landscape, UI or sky. The Android ASTC package label does not change
this runtime DAT texture path. Terrain blends use retail's byte-color arithmetic;
older cached blends are automatically rebuilt. `ace.RenderAudit` reports unique
landscape texture sizes, formats and estimated GPU allocation.

For a live, stationary performance comparison, keep the headset on and the game
visible with menus closed, then run from this directory:

```powershell
.\Tools\Measure-QuestRenderComparison.ps1 -Variable ShowFlag.PointLights -TestValue 0
```

This records baseline, changed, and restored intervals for a live console variable.
It rejects sleep/loading/menu interruptions and changes in refresh rate or
resolution scale, and checkpoints the original setting
and restoration status in `Performance/Comparison-*/comparison.json`. Check that
file if the host exits unexpectedly. Do not use it for settings that require a
restart, such as GPU timestamps or shader permutations. Actor draw caching supports
live comparisons from v76 onward. Keep
screenshots, rendering audits, builds, and other heavy work outside sample windows.
`Tools/Summarize-VrApi.py` summarizes the saved log with the report's process ID
and time interval; use `--min-samples` to reject incomplete captures and
`--expected-hz` / `--expected-scale` to require the reference display settings.
The summarizer also rejects a refresh-rate or scale change within an interval.
Run `python Tests/Test-VrApiSummary.py` to check the capture parser. Runtime App
durations are not GPU-only timings or frame-time percentiles.

### Standalone check, 2026-10-17

Quest 3 was tested with the offline `ace.PerfScene outdoor` fixture: 32 NPCs,
45 seconds of warmup, 30 seconds of sampling, 4128 x 2208 stereo target and
runtime resolution scale 0.80. Initial 90 Hz runs measured 50.39 FPS on v88 and
50.42 on released v121. The old fixture also enabled tracing, so these are not
a precise version-to-version speedup measurement. Neither reached 90 FPS.

At matching 72 Hz/0.80 settings, released v121 measured 48.27 FPS and the local
candidate 50.95 FPS. Temperatures were 51–54 C and CPU clocks varied between
1920 and 2208 MHz; this small FPS difference cannot be attributed solely to the
code changes. A repeat lost OpenXR visibility during sampling and was excluded.
The released indoor fixture measured 55.43 FPS with eight NPCs at 72 Hz.

The CPU traces do verify eliminated work: with controllers untracked, six arm
render proxies were recreated per frame (3312 in 552 frames); the candidate had
none in its subsequent trace. Desktop radar, shortcut, vitals and effect overlays
now defer presentation while only the native VR HUD is displayed. Gameplay
processing continues, and opening a desktop menu or wrist spell bar catches up
before drawing. Animated primitive/light-interaction updates and uniform-buffer
work remain significant. Retesting cached actor draw commands showed no gain,
so that default was retained.

Windows editor and Android packages built successfully. `ACE.VR.RigAndMenus`
and `ACE.Performance.GameplayRefresh` passed, including tracking recovery and
same-frame HUD catch-up; all six VrApi parser tests passed. The local candidate
was installed on the headset without publishing a release. Temporary device
captures were removed after analysis. Local trace payloads were also removed;
summary files and logs remain in `Performance/20261017-Standalone` and
`Performance/Comparison-20261017-041629-858`. Automatic approval review blocked
removing the complete directories. These results do not
establish live combat performance or a sustained 90 FPS target.

## Optional AC2 sky on Quest

After installing the current APK, connect the headset by USB and run `Install-AC2-Sky.cmd` from the extracted Quest bundle. Select your AC2 Legions `portal.dat` or an existing desktop `Saved/Skies/Legions.sky` cache. In AC:VR, open **Options → VR settings → Graphics → Import and use AC2 sky**. The **AC2 sky** toggle remembers your choice. For a scripted installation, pass `-AC2SkyPath` to the installer.
