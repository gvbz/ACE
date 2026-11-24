# AC:Unreal / AC:VR release 166

Windows desktop, PC VR, native Linux, standalone Quest, and the Android phone and tablet preview share version **2026.11.24.166**.

## Plugin Manager

- Open **Plugin Manager** on the account screen to browse official and community plugins, view the developer and repository, and install the ones you want. Update one plugin or use **Update all**.
- Fresh installs start with an empty plugin list. Existing installations with a recognized previous profile can restore their previously bundled plugins. Uninstalling a managed plugin keeps its saved profiles.
- Developers can register a GitHub repository once in [ACUnrealPlugins](https://github.com/Thwargle/ACUnrealPlugins). The catalog checks for new published packages hourly.
- Pack-management plugins can move items between the main pack, side packs, and open external storage, including its accessible side packs. Plugins can choose a placement slot.

## Display and combat

- Resize the Waypoint direction arrow by dragging its corner with the interface unlocked.
- Desktop display options support 32:9 ultrawide screens.
- Enable **Stand still to shoot** to require a stop before firing a missile weapon. Turning in place still works. ACE server operators can enforce this with the optional `missile_block_while_moving` property in the updated server.
- Improve selection and combat sight for small creatures at the waterline, including Bog Moars. Movement releases during an interaction also reach the server when movement resumes.
