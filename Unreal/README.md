# AC:Unreal / AC:VR — Unreal project

**AC:Unreal** is the desktop client; **AC:VR** is the PC VR and native Quest client.
Both use the **ACEClient** plugin and the shared `ACUnreal` project/module.
Existing profiles migrate to the renamed save folder without replacing newer settings.
Launcher artwork and reproducible icon exports are in [Build/Branding](Build/Branding/README.md).

## Quick start

**VR build 2026.09.12.3:** see [Rift / SteamVR setup and controls](Docs/VR_SETUP.md)
and the [implementation review](Docs/VR_PORT_REVIEW.md). `Launch-VR.ps1` builds
and launches the OpenXR client. A Development client package and matching
extended server are available under `PackagedVR`. Headset and live combat
acceptance testing is still required.

1. Install **Unreal Engine 5.8** (Epic Games Launcher).
2. Install **Visual Studio 2022** with workload **Game development with C++**, including:
   - MSVC v143 toolset
   - **Windows 10/11 SDK (10.0.19041 or newer)** — required or Unreal cannot compile
3. Put retail DAT files in `C:\Turbine\Asheron's Call\` (`client_portal.dat`, `client_cell_1.dat`, optional `client_highres.dat`).
4. Close any existing editor instance for this project, then run **`C:\dev\ACE\Unreal\Launch.bat`**.
5. The launcher builds **ACUnrealEditor Win64 Development** with UE 5.8 before opening the project. If compilation fails, it stops instead of opening old binaries. Unchanged builds only need an incremental check.
6. Press **Play** → login UI → account / Enter World → WASD.

If compile fails with **“No available Windows SDKs”** / **Sdk: not found**, install SDK 10.0.19041 via Visual Studio Installer → Modify → Individual components → *Windows 10 SDK (10.0.19041.0)*, or:

```bat
winget install --id Microsoft.WindowsSDK.10.0.19041 --accept-package-agreements
```

### Optional AC2 sky

In **Graphics Options**, click **Browse…** and choose your **Asheron's Call 2: Legions** `portal.dat` (Windows and Linux). You can also paste the folder or file path. Click **Import**, then enable **Use imported AC2 sky**. If `highres.dat` is in the same folder, the importer uses its higher-resolution textures where available.

The client keeps a local copy in `Saved/Skies/Legions.sky`. Outdoors, the AC1 server clock and your AC1 DAT determine the weather and the positions of the sun and both moons. The Legions moon follows the larger AC1 moon's track. The smaller moon keeps its AC1 artwork. The Legions palette follows the AC1 dawn and dusk transitions. AC1 controls when cloud banks, rain and lightning arrive. The imported Legions dawn and daytime rain records supply the storm haze, cloud art and lighting. The asteroid belt fades behind cloud cover and disappears in fully opaque storm haze. Reimport your Legions DAT once to add storm skies to an older import. Turn the option off to return to the AC1 sky and lighting.

For controlled comparisons, install the optional [admin sky preview server mod](../Source/Mods/LocalSkyPreview/README.md).
It provides `/weather`, `/time set`, and `/timelapse [multiplier]` for admin
characters. Classic and imported clouds follow the preview clock; imported
clouds use the classic sky's travel direction, and sun/moon tracks stay aligned.

## Layout

```
Unreal/
  ACUnreal.uproject          ← open this
  Launch.bat                 ← build and open the current editor client
  OpenACUnreal.bat           ← UE 5.8 build/launch implementation
  GenerateProjectFiles.bat    ← optional .sln generation
  Config/
  Source/ACUnreal/           ← game module (GameMode, Pawn)
  Plugins/ACEClient/          ← ACE network + DAT client
  Content/                    ← add a map here if you want
```

## What you get on Play

| Piece | Class |
|-------|--------|
| Game Mode | `ACUnrealGameMode` (world + terrain presenters) |
| Controller | `ACEPlayerController` (login UI, WASD, local appearance) |
| Pawn | `ACUnrealPawn` (capsule + `ACECharacterAppearanceComponent`) |

On **Play**, a separate PIE window opens with the **AC:Unreal** login panel (host/port/account/password). DAT files load when you enter the world, not at Play, so startup is immediate.

The Play window title includes the build version. `Saved/Logs/ACUnreal.log`
also records `ACEClient BUILD STAMP` from the compiled client module. The
version in `Plugins/ACEClient/Source/ACEClient/Public/ACEClientBuild.h` and
`ProjectVersion` / `ProjectDisplayedTitle` in `Config/DefaultGame.ini` must agree.
Run `Launch.bat --build-only` to compile the same target without opening Unreal.

For diagnosing a Windows DX12 shader-conversion crash, `Launch.bat --dx11`
opens the editor with DirectX 11 for that launch only. It does not change the
project renderer or the standalone Quest build. This is a compatibility
workaround; a successful offline render test does not verify a particular server
login. The flag can be combined with `--build-only`.

## Windows packaging

Use **Platforms → Windows → Package Project** in UE 5.8. Packaging builds both
the editor/cooker and the game; close any running game or editor before rebuilding
its DLLs from the command line.

Commandlets suppress automatic startup of the editor's MCP HTTP listener in
memory. This lets cooking run while the interactive editor owns port 8000 and
preserves the saved MCP auto-start preference. No engine plugin changes or port
changes are required.

The cook automatically generates shader parents under
`Content/ACE/RuntimeMaterials` using the same factories as the editor. These assets
are always cooked. Game builds load the compiled parents and bind DAT textures
through dynamic material instances. The cook also refreshes its asset registry,
so newly generated parents are included on the first cook.
Cooked output is written to disk before staging, so assembling the package does
not depend on the local Zen server remaining alive after the cooker exits.

The map cook list contains the default `Template_Default` startup map. It does
not include the optional offline Dereth exports: the normal client streams DAT
terrain and objects at runtime. If using a custom or baked map, add that map to
**Project Settings → Packaging → List of maps to include in a packaged build**.

The package still needs the retail DAT installation configured in
`[/Script/ACEClient.ACEDatSubsystem] DatDirectory`. It does not contain those DAT
files or saved login credentials. Run `Windows/ACUnreal.exe` from the output
folder, keeping the adjacent `ACUnreal` and `Engine` folders with it.

If login succeeds but character selection stays black, first verify the retail
DAT installation on that computer. By default it must contain
`C:/Turbine/Asheron's Call/client_portal.dat` and `client_cell_1.dat` (and
`client_highres.dat` when installed). The packaged client does not include them.
Keep the complete extracted package, including
`ACUnreal/Plugins/ACEClient/Docs/UI/Resolved/*.json`.

Build 2026.09.11.6 keeps the login card visible until character selection is
ready, displays missing DAT/layout errors, and stops retrying failed indexing
each frame. Click Login again after correcting a missing DAT installation.
For a non-default DAT folder, set the following in the game's saved
`Config/Windows/Engine.ini`, then restart (preserve other settings):

```ini
[/Script/ACEClient.ACEDatSubsystem]
DatDirectory=D:/Games/Asheron's Call
```

Collect `ACUnreal.log` from the package's `ACUnreal/Saved/Logs`, or
`%LOCALAPPDATA%/ACUnreal/Saved/Logs`. The `background index starting` line
records the DAT folder actually used; `background index failed` and
`UILayoutResolver: missing resolved layout` identify the two asset failures.

For shader preparation without a complete package, run `UnrealEditor-Cmd.exe`
with the project path and `-run=ACEPrepareRuntimeMaterials -unattended`.
Development packages expose `ACE.Packaging.Materials`,
`ACE.RetailParity.PortalMaskMaterial`, and `ACE.RetailParity.MeshApplication`
automation tests. The latter uses the installed retail DAT files to verify
runtime mesh construction and actual collision traces.

## First-time editor setup (one-time)

The repo ships **no `.umap` files** (binary assets). Config points at the engine **Open World** template, which loads automatically. For a project-owned map:

1. **File → New Level → Empty Open World** (or Basic).
2. **Edit → Project Settings → Maps & Modes** → set **Editor Startup Map** and **Game Default Map** to your saved map (e.g. `/Game/Maps/ACUnreal`).
3. **Window → World Settings → GameMode Override → ACUnrealGameMode** (project default is already set in `Config/DefaultEngine.ini`).
4. Save the level under `Content/Maps/` (e.g. `ACUnreal`).

## Custom map (optional)

1. Create **File → New Level → Empty Open World** (or Basic).
2. **World Settings → GameMode Override → ACUnrealGameMode**.
3. Save as `Content/Maps/Main`, set it as Editor Startup / Game Default Map in Project Settings.

## Plugin docs

- [Plugins/ACEClient/README.md](Plugins/ACEClient/README.md)
- [Plugins/ACEClient/Docs/DAT.md](Plugins/ACEClient/Docs/DAT.md)
