# Waypoint

Waypoint is a built-in navigation plugin with no game-action permissions. It is
enabled by default and can be disabled in Plugins. It does not move the player.

## Navigation

Use **NAV** on the desktop Mods bar for settings and the **globe icon** for the independent
map window. In VR, select Waypoint in plugin management; Open map switches that
panel to its map view and Waypoint settings returns to the settings view.

Coordinates such as `42.0N, 33.6E` become blue, underlined links in desktop and VR
chat. Each coordinate pair is independent. Desktop drag-to-select/copy still
works, and clicking ordinary sender text retains its existing tell behavior.
Coordinates are N/S first, E/W second, case insensitive. The manual Go field
accepts the same format; malformed or out-of-range coordinates are rejected.

With no destination selected, the floating arrow points north, including in dungeons.
The shaded arrow rotates relative to the player's facing: up is ahead, right is
right, left is left, and down is behind. The text above it is the **current player
position**, with the destination below. Distance is horizontal metres, with an
Arrived indication within three metres; rounded chat coordinates are approximate.
The position is locally predicted, so it follows movement without waiting for a
server position packet. Map units follow the ACE coordinate constructor / common
Decal coordinate conversion: global metres / 240 - 101.95.

Turn off **Show floating arrow** to hide the coordinate arrow on the HUD and in
the Waypoint panel, including the north arrow shown when you have no destination.
This choice persists across logins and Waypoint profiles. Chat links, map clicks,
atlas routes and other plugins can update the destination while the arrow is hidden.
Turn the checkbox back on to see the current heading or destination.
**Show distance to destination** is independent. To move the
arrow, unlock the game UI with the radar lock icon, or hold **Quick-show pinned
windows** (Left Control by default), and drag the arrow area. The
arrow has no background, frame or title bar, and does not intercept clicks while
locked. Its
position is stored through the same plugin window persistence as other plugins.
Window positions, the destination and display preferences survive login/restart.
The desktop overlay hides with Alt+Z and is removed on logout.

## Map

- **Pin map to interface** keeps a thin gold-bordered desktop overlay after closing the
  normal map window. It follows the player's predicted position and automatically
  switches between overworld terrain and dungeon/building outlines on a black backdrop.
  Both views share the saved position, size and visibility; existing dungeon pins
  carry over. Each view saves its own zoom across closing the map and relogging. Clear Pin
  to hide both views; reopen the globe window to change these options.
- Use the pinned map's **+ / −** buttons to zoom in or out around the player,
  including while the UI is locked. They work in both overworld and interior views.
- Use the separate **Map − / Map +** button to minimize or restore it without
  hiding the direction arrow. Its expanded size and position are retained.
  **Map background opacity** in the globe window adjusts the black backdrop and
  terrain while keeping outlines, labels and markers readable. It shares the
  Waypoint Map background setting in Plugin Manager, including the global opacity
  override. Older pinned-map opacity preferences are retained; this preference
  and minimized state are saved.
- **Player facing up** rotates both maps with the character; off keeps north up.
- Use the game UI's **lock/unlock icon** to lock the pinned map (click-through,
  except for its zoom and minimize/restore buttons)
  or unlock it to drag, resize from its bottom-right corner, or zoom with the
  wheel. Position, size, pinned mode and orientation survive logout and restart.
  Saved arrow and map positions are retained across temporary viewport changes.
- Left drag pans. Mouse wheel zooms around the cursor; + and - zoom at the center.
- Center on me centers without discarding the zoom. Reset view restores the overview.
- The full map also saves separate indoor and outdoor zoom levels, independently
  of the pinned map. Reset view saves the default zoom for the current view only.
- Click a marker or map location to set a destination.
- Towns, Portals and POIs control marker categories. The name filter restricts markers.
- Portal names stay visible at every zoom level in both map views, even when
  labels overlap. Town and other POI labels appear when zoomed in or hovered;
  detailed non-portal atlas markers appear at 3× zoom or while searching.
  Close markers remain selectable. Labels omit a leading "Portal to", while
  hovering shows the full name and search still matches the original name.
- Dungeon toggles the current landblock's interior layout, including buildings.
  Floor outlines around the player's elevation are highlighted; other floors are dim.
  A destination clicked in a dungeon is tied to that landblock, so the arrow does
  not direct the player toward it in a different dungeon.
- **Fill walkable areas** adds blue floor shading inside the dungeon outlines in
  both map views. It uses the actual floor polygons, preserving pits and gaps;
  floors near your elevation are brighter. The option is saved with your profile.
- **Show active route** draws the selected UCM route in gold on both map views,
  with the current waypoint and its incoming leg in green. Runtime routes loaded
  by metas take precedence. Portal/recall transitions and unrelated dungeons do
  not get connecting lines. This saved option is independent of the 3D route
  preview and is enabled by default. These lines show recorded waypoints, not
  collision-aware pathfinding around obstacles.

The world overview uses the retail DAT map image. As you zoom in, 256-pixel
terrain tiles replace it, generated from the installed cell and portal DATs with
retail TexMerge road/terrain blends and height shading. Three detail levels cover
32, 8 or 2 landblocks per tile. Only visible tiles are requested, nearest the view
center first. A serial background worker owns independent DAT readers; at most 64
GPU tiles and 256 terrain blend images are retained. First use may take a few
seconds to index the DATs; the overview stays visible while tiles load. These
are terrain maps, not overhead renders of buildings, trees or server objects.
Town landmarks come from the existing retail map data. Nearby portals come
from live server objects, so custom portals use their server-provided placement.

### GoArrow destinations and directions

Open **NAV**, paste the full path to GoArrow's **locations.xml**, and click
**Import locations**. Windows “Copy as path” quotes are accepted. A successful
import opens the searchable list and shows how many locations were loaded. An
error appears beside the import controls and leaves the previous atlas available.
Use **Browse locations** to reopen it after restarting the client. If you imported
with v157 or earlier, import the XML once more to add its portal exits.

Search by name or type and select a row. Each row shows its coordinates so you
can distinguish places that share a name.

- **Point arrow** aims the floating arrow directly at the selected location.
- **Find route** uses your current outdoor position and looks for the least walking
  distance through eligible one-way atlas transports. The arrow points to the
  first step. After taking a portal, select the next step in the directions.
  The current itinerary is available when you reopen the browser in the same session.

Walking legs use straight-line distances within GoArrow's map regions and must
be under 50 map units each. Directions use the imported exits. Personal recalls,
terrain obstacles, and character-specific access requirements are outside the
planner. You move your character and use each portal yourself.
Region boundaries come from [GoArrow's MapRegions.xml](https://github.com/Xanius/GoArrow-VVSEdition/blob/fb7e49f0f9f4d7bd1401c5310dfcfa9fd05ed84d/GoArrow/RouteFinding/MapRegions.xml).

The importer accepts GoArrow's
`<locations><loc name="..." type="..." NS="..." EW="..."/></locations>` format.
It does not accept the older raw CoD `<atlas>` format. Invalid input preserves the
previous atlas. The import reads data only and does not run Decal DLLs. The
copyrighted GoArrow/CoD database is not bundled with this client. Atlas locations
can be stale or differ on custom servers; nearby live portals remain authoritative.

The imported atlas is stored at `Saved/ClientPlugins/Waypoint/locations.json`.
On Quest, use its app-private Saved location; copying a file there requires ADB.
Waypoint saves the floating arrow choice in `Saved/ClientPlugins/Waypoint/floating-arrow.txt`.
The destination and other display options are under `Saved/ClientPlugins/Profiles/waypoint`.
Desktop placement is in `Saved/ClientPlugins/settings.json`.

Dungeon outlines are built from cell/portal DAT geometry, incrementally while the
map is visible. They do not spawn meshes, actors, textures or collision for unseen
rooms. Reads are limited to two cells per tick with a time budget between cells,
4,096 cells per landblock and 50,000 floor outlines. An individual file read is
synchronous. Outlines are a visual aid, not a server pathfinder or an indication
that a locked door or quest barrier is traversable.

## UtilityBelt compatibility

UtilityBelt cannot be copied into Installed and run unchanged. Source reviewed at
commit `5fe9825a82f38047737768fd92c61dd47d88e467` of
https://gitlab.com/utilitybelt/utilitybelt.gitlab.io/:

- `UtilityBelt/UtilityBelt.csproj` targets **net48 / x86**, permits unsafe code, and
  references Decal.Adapter, Decal COM services, VirindiViewService, VirindiHUDs,
  VirindiHotkeySystem and Microsoft.DirectX dependencies.
- `UtilityBelt/Lib/PhysicsObject.cs` reads retail physics objects through pointers
  and fixed memory offsets obtained from Decal. Unreal objects do not have that ABI.
- `UtilityBelt/installer.nsi` registers a Decal network filter in the Windows registry.

Our API 1 plugin host runs bounded Lua with explicit host actions and native
cross-platform panels. Installing .NET or loading a DLL would not supply those
retail memory layouts, renderer, Decal events, actions and UI services. A useful
port would retain applicable data and isolated algorithms while replacing those
integrations with our host APIs and Slate/VR views. A full compatibility runtime
would be a substantial separate project, especially for Linux and Android/Quest.
No UtilityBelt executable code is loaded or bundled by Waypoint.

Reference behavior: https://www.virindi.net/wiki/index.php/GoArrow_(VVS_Edition)

## Validation for this implementation

- Windows Editor, Linux Development and Android/Quest Development builds passed.
- All 11 `ACE.Plugins` suites passed, including coordinate parsing/heading,
  persistence and import rejection checks; native VR chat passed too.
- Desktop chat input, multiline selection and copy/paste passed on an isolated
  rerun after the combined run encountered Windows `OpenClipboard` error 5.
- The installed GoArrow atlas imported **5,066** locations without executing any
  plugin binaries. That atlas was used only in temporary test storage.
- World map, arrow and dungeon `0x0143` rendered against real DAT files. Settings
  and map panels were rendered and inspected at 450px and 780px widths.
- This is offline validation, not a live-server or headset playtest. No release
  was published or installed on a headset as part of this change.
