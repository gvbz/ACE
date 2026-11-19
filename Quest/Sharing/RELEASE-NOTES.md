# AC:Unreal / AC:VR release 161

Windows desktop, PC VR, native Linux, standalone Quest, and the Android phone and tablet preview share version **2026.11.18.161**.

## Waypoint maps and atlas imports

- Crowded map markers group into clickable counts. Select a result from the list instead of reading overlapping portal and dungeon labels.
- Drag the minimap to pan and use the mouse wheel to zoom, including while the interface is locked. Use the crosshair button (**center player**) to resume following your character. Overworld maps support closer zoom levels.
- **Browse locations** opens an XML file picker on Windows and Linux; **Import locations** loads the selected file. Expanded GoArrow and Crossroads of Dereth XML support covers the supplied atlas variants. Import buttons retain single-line labels and consistent padding.
- Opening a plugin again brings its existing interface forward instead of creating another copy.
- Third Eye directions correctly recognize targets in the same dungeon.

## Third Eye audio alerts

- Enable an optional chime when a new matching entity is found. Alerts are off by default and can be used independently of chat notifications.
- The Audio tab includes a bundled chime, custom WAV selection, volume, sound preview, and restore-default controls. Settings save per character and server.

## Sky, fog, and distant scenery

- Imported AC2 skies use authored fog colors separately from cloud tint. Fog coverage accounts for the terrain actually displayed, including camera offsets and delayed terrain loading.
- Smooth terrain detail transitions and fade distant scenery near its range limit to reduce abrupt popping and foliage speckles while preserving cutout shapes.
- Ground cloud shadows disappear when imported sky clouds are absent.
- Correct the imported primary moon's orientation and apparent size relative to AC1. Rendered checks verify both moons remain behind opaque clouds and fog.
- Thanks to **gvbz** for the far-distance rendering contribution.

## Inventory and verification

- Restore visibility of NPC-returned stack remainders in GDLE-compatible inventory update sequences, and add bounded turn-in diagnostics. This does not establish that the reported Coldeve trinket exchange consumes or returns the correct quantities on the server.
- Automated regression and package checks cover map input, atlas imports, plugin window reuse, audio settings, inventory updates, sky behavior, fog coverage, and runtime materials. Desktop packages are exercised on Windows and Linux; Android and Quest packages receive signing, content, and version checks.
- Fog blending still differs from retail in some color transitions, and terrain popping is reduced rather than eliminated. Physical headset testing remains important for fine foliage and distant scenery. The additional terrain detail and retained scenery can use more memory.

Players supply their own AC1 data and, for the optional imported sky, their own Legions data. No retail game data, saved profiles, or server credentials are included.
