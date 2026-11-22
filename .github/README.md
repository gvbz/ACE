# AC:Unreal client, gvbz fork

A fork of Thwargle's AC:Unreal client (https://github.com/Thwargle/ace)
with a few added options. The original README is in the repository root.

## Branches

- `master` matches Thwargle's repository.
- `gvbz` has the changes below, on top of client v163 (2026.11.20.163). Since v159 they are also part of Thwargle's client (pull request #1), with his later changes.

## Changes in v163

- Fog Colors by Weather: with `Use imported AC2 sky` on, the fog at
  sunrise and sunset is recolored from atmospheric color tables based
  on real-world sky colors, with a different table for each server
  weather: Clear uses the clear sky table, Sunny the hazy table,
  Cloudy the foggy table, and Rainy the overcast and rainy table.
  Each table gives colors for each twilight phase (on clear days
  golden hour, civil and nautical twilight, night, blue hour and
  dawn), at the horizon, mid-sky and zenith. The fog follows the
  phases through sunrise and sunset in the horizon colors, and the
  horizon band (the sky just above the land) in the mid-sky colors.
  The tables change the hue and saturation of the AC2 fog, but its
  brightness stays the AC2 sky's (adjusted per weather), so the base
  feel of the AC2 sky is kept. Rain weakens the colors, and each day
  shifts them slightly. `Fog Colors by Weather` toggle (off by
  default) and `Fog Color Saturation` (0 to 1.5, default 0.35). Test
  controls are in the Development Dashboard's Fog tab. This truly
  creates a 1 in 1000 sunrise.
- Smooth light direction: with `Use imported AC2 sky` on, the light
  turns gradually when the sun or the large moon rises or sets and
  at midnight, instead of turning in one frame. Test controls are in
  the Development Dashboard's Lighting tab.

## Changes in v161

- Max Fog Distance: the fog never reaches past the set distance, in
  any weather (`Max Fog Distance`, 5 to 43 landblocks, default 11; 43
  is no limit). Below 43 the fog starts at 3 landblocks in clear
  weather by day and lets 27 % of the distant land show through, and
  the far terrain textures stop at the limit. It applies with the far
  terrain on.
- Far Terrain Horizon: with it on, the horizon band is drawn at 73 %
  opacity, so the horizon line shows through the haze.
- Development Dashboard: a bundled Lua plugin (off until enabled in
  Manage Plugins) with test controls for the fork's `personal.` console
  variables (Far Terrain, Far Scenery and Far Rooms tabs; values are not
  saved), and an Admin tab with server admin commands, saved locations,
  place suggestions and map coordinates, for a server where the
  character is an admin. It uses a new plugin service `dev`.

How each change was built: [Plan_History_git.md](Plan_History_git.md)

Screenshots of each change, on and off: [Comparisons.md](Comparisons.md)

Added code is in `Personal` folders and files named `Personal*`. Edits
in existing files are marked `// personal:`.

## License

AGPL-3.0, the same as the original. See LICENSE.

## v158

Sent to Thwargle in pull request #1 and part of his client since v159, with his later changes.

- Camera: a height slider for the third person camera (1.5, the client's
  height, to 5). `Save Camera Position` keeps the distance and pitch
  between sessions, and `Free Camera Restriction` gives one zoom limit at
  every angle (both off by default).
- Far terrain: one low-detail terrain mesh past the loaded landblocks,
  out to the `Far Terrain Radius` setting (43 to 96 landblocks, default
  43). The `Far Terrain` toggle turns it on and off (off by default).
- Far terrain objects: trees, rocks, buildings and other objects are
  drawn past the landblocks where the client draws its own, out to the
  `Far Terrain Objects Radius` setting (3 to 24 landblocks, default 6),
  with simpler models further out (`Far Terrain Objects` toggle, off by
  default). `Hide Far Terrain with Fog` removes the ones past the fog
  end (off by default). It applies with the far terrain on.
- Far terrain textures: the far terrain within about 2.7 km is drawn
  with the same terrain textures as the loaded terrain (`Far Terrain
  Textures` toggle, off by default). It applies with the far terrain on.
- Sky horizon: the night sky continues below eye level, so no bright
  strip shows between the far terrain and the sky from sunset to dawn
  (`Far Terrain Horizon` toggle, off by default). It applies with the
  far terrain on.
- Far terrain rooms: far buildings show the rooms behind their windows
  and other openings, with the rooms' own lights at night, and their
  doors get dark panels until the door itself is there (`Far Terrain
  Rooms` toggle, off by default). It applies with the far terrain
  objects on.
- The far terrain options stay on inside buildings, for the view out of
  doors and windows, and turn off in dungeons.
