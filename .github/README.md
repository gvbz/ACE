# AC:Unreal client, gvbz fork

A fork of Thwargle's AC:Unreal client (https://github.com/Thwargle/ace)
with a few added options. The original README is in the repository root.

## Branches

- `master` matches Thwargle's repository.
- `gvbz` has the changes below, on top of client v161 (2026.11.18.161). Since v159 they are also part of Thwargle's client (pull request #1), with his later changes.

## Changes

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

How each change was built: [Plan_History_git.md](Plan_History_git.md)

Screenshots of each change, on and off: [Comparisons.md](Comparisons.md)

Added code is in `Personal` folders and files named `Personal*`. Edits
in existing files are marked `// personal:`.

## License

AGPL-3.0, the same as the original. See LICENSE.
