# AC:Unreal / AC:VR release 158

Windows desktop, PC VR, native Linux, standalone Quest, and the Android phone and tablet preview share version **2026.11.15.158**.

## Waypoint

- Import a GoArrow `locations.xml` file from **NAV → Import locations**. Waypoint opens the searchable location list and reports how many entries it loaded. If an import fails, it shows the error and keeps the previous atlas.
- Search by name or type, then **Point arrow** at a destination or choose **Find route** for walking and portal directions. Locations with the same name show their coordinates. Reimport atlases saved in v157 or earlier to include their portal exits.
- The floating arrow respects its saved on/off setting across sessions and destination updates.

## Dungeon lighting

- Torches and other light fixtures illuminate nearby dungeon walls, floors, scenery, and characters. Turning in place keeps torch brightness stable, including when a torch behind the camera lights the floor ahead.
- With **Dynamic lights** set to **Full** on desktop, dungeon fixtures have their own lights. **Low** and **Medium** shadows use two nearby shadow casters. **High** and **Cinematic** allow every fixture to cast shadows.
- Enable **Uniform dungeon lighting** directly below **Custom object highlighting** for evenly lit interiors. The option applies immediately and saves across sessions. Torch lighting is the default.

## Server source

- Merged ACEmulator upstream through `aadedb3d6`, including .NET package and server build metadata updates.
