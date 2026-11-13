# AC:Unreal client, gvbz fork

A fork of Thwargle's AC:Unreal client (https://github.com/Thwargle/ace)
with a few added options. The original README is in the repository root.

## Branches

- `master` matches Thwargle's repository.
- `gvbz` has the changes below, on top of client v155 (2026.11.11.155).

## Changes

- Camera: a height slider for the third person camera, one zoom limit
  at every angle, and saved distance and pitch.
- Far terrain: one low-detail terrain mesh past the loaded landblocks,
  out to the `Far Terrain Radius` setting (0 = off, default 48, max 96
  landblocks).

Added code is in `Personal` folders and files named `Personal*`. Edits
in existing files are marked `// personal:`.

## License

AGPL-3.0, the same as the original. See LICENSE.
