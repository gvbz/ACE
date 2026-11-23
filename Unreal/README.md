# AC:Unreal and AC:VR

This is the shared Unreal Engine project for the desktop, PC VR, Quest and
Android clients. Start here for a Windows editor build. See the [source build guide](../SOURCE_CHECKOUT.md),
[VR setup](Docs/VR_SETUP.md), [Quest guide](../Quest/README.md) or
[Android guide](../Mobile/README.md) for other targets.

## Quick start

1. Install Unreal Engine 5.8 and Visual Studio with **Game development with C++**,
   an Unreal-supported MSVC toolset and Windows SDK 10.0.19041 or newer.
2. Put your retail `client_portal.dat`, `client_cell_1.dat` and optional
   `client_highres.dat` in `C:\Turbine\Asheron's Call`. Use **Game files** on
   the [login screen](Docs/LOGIN.md) to select a different folder.
3. Run `Launch.bat` from this directory. It builds and opens the editor. Use
   `--build-only` to compile without opening it, or `--dx11` to launch the
   editor with DirectX 11 for troubleshooting.
4. Press **Play**, connect to a server, and enter the world. For PC VR, use
   `Launch-VR.ps1`.

## Optional AC2 sky

In **Graphics Options**, browse to your Legions `portal.dat` or paste its path.
Select **Import**, then **Use imported AC2 sky**. The import uses adjacent
`highres.dat` textures when available and saves to `Saved/Skies/Legions.sky`.
The AC1 clock and weather determine timing for both sky options.

For controlled comparisons, the [admin sky preview mod](../Source/Mods/LocalSkyPreview/README.md)
provides `/weather`, `/time set` and `/timelapse [multiplier]`.

## Windows packaging

Close running game and editor instances. In UE 5.8, use **Platforms → Windows →
Package Project**, or follow the [command-line build guide](../SOURCE_CHECKOUT.md#windows--pc-vr).
The cooker generates the required runtime materials automatically.

Keep the full output directory together and run `Windows/ACUnreal.exe`. The
retail DAT files come from your local installation. For a custom DAT path, set
`DatDirectory` in the game's saved `Config/Windows/Engine.ini` and restart.

```ini
[/Script/ACEClient.ACEDatSubsystem]
DatDirectory=D:/Games/Asheron's Call
```

For login or character-selection problems, check the DAT path and keep
`ACUnreal/Plugins/ACEClient/Docs/UI/Resolved` with the package. Find
`ACUnreal.log` under `ACUnreal/Saved/Logs` or `%LOCALAPPDATA%/ACUnreal/Saved/Logs`.
`ACEClient BUILD STAMP` identifies the compiled client and
`background index starting` identifies the DAT folder in use.

## Custom map

The project uses the engine's startup map. To add your own, create an Empty
Open World or Basic level, set `ACUnrealGameMode` as its Game Mode, and save it
under `Content/Maps`. Set it in **Project Settings → Maps & Modes** and add it
to the packaging map list if you plan to ship it.

## Plugin docs

- [ACEClient integration](Plugins/ACEClient/README.md)
- [Client plugins and UCM](Plugins/ACEClient/Docs/Plugins/README.md)
- [Public Lua API](Plugins/ACEClient/Docs/Plugins/PublicAPI.md)
- [DAT files](Plugins/ACEClient/Docs/DAT.md)
