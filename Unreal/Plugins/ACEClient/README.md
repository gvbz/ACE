# ACEClient

ACEClient is an Unreal Engine plugin for the retail Asheron's Call 1802
protocol. It handles login, gameplay networking, DAT assets and world
presentation. See the [retail parity review](Docs/RETAIL_PARITY.md) and
[test guide](Tests/README.md) for coverage and remaining gaps.

## Integration

For this repository's client, start with the [Unreal project guide](../../README.md).
To integrate ACEClient in another Unreal project:

1. Copy the plugin into your project's `Plugins/ACEClient`, enable it, and
   generate project files. It uses Sockets, Networking, UMG and
   ProceduralMeshComponent.
2. Point `ACEDatSubsystem.DatDirectory` at the retail installation containing
   `client_portal.dat` and `client_cell_1.dat`. The default Windows path is
   `C:/Turbine/Asheron's Call`. See the [DAT guide](Docs/DAT.md).
3. Use `ACEPlayerController` with a capsule pawn. Add
   `ACEWorldPresenterComponent` and `ACETerrainPresenterComponent` to the
   Game Mode or a persistent level actor.
4. Connect with a valid server account. The default ACE ports are 9000 for C2S
   and 9001 for S2C. For local ACE development, prefer
   `DDD.EnableDATPatching = false`.

The controller applies the local character's appearance to the possessed pawn.
The world presenter handles other objects. Customize the login UI with an
`ACELoginWidget` Blueprint that binds `HostBox`, `PortBox`, `AccountBox`,
`PasswordBox`, `LoginButton`, `EnterWorldButton`, `StatusText`,
`CharacterListLabel` and `CharacterListBox`.

## APIs

Blueprints use **ACE Client Subsystem** for login/logout, character selection,
movement and the object registry. Its events include character list, world
entry, object creation/deletion, position updates and chat.

For Lua extensions, see the [plugin guide](Docs/Plugins/README.md) and
[Public API 2](Docs/Plugins/PublicAPI.md).
