# AC:Unreal client, gvbz fork

A fork of Thwargle's AC:Unreal client (https://github.com/Thwargle/ace)
with a few added options. The original README is in the repository root.

## Branches

- `master` matches Thwargle's repository.
- `gvbz` has the changes below, on top of client v157 (2026.11.13.157).

## Changes

- Camera: a height slider for the third person camera, one zoom limit
  at every angle, and saved distance and pitch.
- Far terrain: one low-detail terrain mesh past the loaded landblocks,
  out to the `Far Terrain Radius` setting (43 to 96 landblocks, default
  43). The `Far Terrain` toggle turns it on and off (off by default).
- Far terrain objects: trees, rocks, buildings and other objects are
  drawn past the landblocks where the client draws its own, out to the
  `Far Terrain Objects Radius` setting (3 to 8 landblocks, default 6),
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
- The far terrain options stay on inside buildings, for the view out of
  doors and windows, and turn off in dungeons.

Added code is in `Personal` folders and files named `Personal*`. Edits
in existing files are marked `// personal:`.

## License

AGPL-3.0, the same as the original. See LICENSE.
