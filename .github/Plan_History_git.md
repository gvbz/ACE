# Plan history (gvbz at bb8d321d3, client v161, 2026.11.18.161)

How each change in this fork was built: files, settings, how it works, tests, measurements, what it depends on in Thwargle's client, and what is still open. One section per change, oldest first, then an idea that was tried and dropped. Screenshots of each change, on and off, are in [Comparisons.md](Comparisons.md).

Code paths are under `Unreal/Plugins/ACEClient/Source/ACEClient` unless stated. T = a file from Thwargle's repository; every edit in one is marked `// personal: <feature>`. Line numbers are for gvbz at 156bdf587. Files the client writes are in its `Saved` folder (`Unreal/Saved` in the editor; the packaged client has its own). Measurements are from a packaged client on an RTX 5080 at 3440x1440.

## 1. Camera

| | |
|---|---|
| Merged | Client v155, commit 0feafd03e |
| Changed | 2026-11-16, commit 47a2e4015, client v158: the lowest height is retail's 1.5 (was 0.5) |
| Changed | 2026-11-16, commit 156bdf587, client v158: the saved position and the 450 zoom limit became the options `Save Camera Position` and `Free Camera Restriction`, both off by default |
| Settings | `Camera Height` (`PersonalCameraHeight`), 1.5 to 5 AC units, default 1.5 (retail's height, `ACECameraRetail::DefaultPivotZAc`). `Save Camera Position` (`PersonalCameraSave`), check box, off. `Free Camera Restriction` (`PersonalCameraFree`), check box, off |

### What it does

- Camera height: the point the third-person camera orbits around can be raised above the character's feet, from retail's height up. Until 47a2e4015 it could also be lowered, to 0.5.
- `Free Camera Restriction` on: zooming out stops at 450 AC units at every camera angle. Off (default): Thwargle's limit, `ACECameraRetail::MaximumArmLength`, which bounds each horizontal axis to 10 units and the height to 450 (so 450 only from straight above, about 10.5 at pitch -17). Before 156bdf587 the 450 limit was always on.
- `Save Camera Position` on: camera distance and pitch are kept between sessions. Off (default): retail defaults at login, nothing written; values saved earlier stay in the file. Before 156bdf587 it was always on.
- With both off and the height at 1.5, the camera is Thwargle's.

### Files

| File | Role |
|---|---|
| `Private/Personal/PersonalCamera.h/.cpp` | Namespace `PersonalCamera`: constants, option keys `SaveKey`/`FreeKey`, `SavePosition`, `FreeZoom`, `PivotHeightAc`, `MaxArmLength`, `Restore`, `Update` |
| `Private/Tests/Personal/PersonalCameraTests.cpp` | Tests `ACE.Personal.Camera` (own settings file `Saved/Automation/PersonalCameraFixture.ini`) |
| T `Private/ACEPlayerController.cpp` line 93 | Include |
| T `ACEPlayerController.cpp` line 3862, `ApplyRetailCameraPivot` | Pivot height from `PersonalCamera::PivotHeightAc(bCameraInHead)` in place of the retail constant |
| T `ACEPlayerController.cpp` line 4263, `UpdateOrbitCamera` | `PersonalCamera::Restore(...)` right after `ResetCameraToRetailDefaults` |
| T `ACEPlayerController.cpp` line 4317, `UpdateOrbitCamera` | `PersonalCamera::Update(...)` at the end of the function, every frame |
| T `ACEPlayerController.cpp` line 4403, `StepCameraZoom` | Zoom-out stops at `PersonalCamera::MaxArmLength(Boom->GetRelativeRotation(),ScaleCm)` in place of `ACECameraRetail::MaximumArmLength(Boom->GetRelativeRotation(),ScaleCm)` |
| T `Public/ACERuntimeOptions.h` lines 14 to 16 | Rows `{PersonalCameraHeight, "Camera Height", 1.5, 1.5, 5}`, `{PersonalCameraSave, "Save Camera Position", 0, 0, 1}`, `{PersonalCameraFree, "Free Camera Restriction", 0, 0, 1}`. `ACERuntimeOptions::Get` and `Set` clamp to the row's range, so an older saved height below 1.5 reads as 1.5; the slider takes its range from the row |
| T `Private/UI/ACEVideoSettingsWidget.cpp` lines 117 to 119 | `PersonalCameraHeight` in the camera slider list (between `CameraAdjustment` and `FieldOfViewDegrees`); then the two check boxes (`UPersonalOptionCheckBox::Make`, 272 x 20), before `Align camera to slope` |

All edits are one line, marked `// personal: camera`. `Private/Tests/ACECameraEdgeTests.cpp` had one added line until 156bdf587; it is unchanged from Thwargle's again.

### How it works

- `SavePosition()`, `FreeZoom()`: the two option rows, read every call (`ACERuntimeOptions::Get > 0.5`), so the check boxes apply without a relog.
- `PivotHeightAc(bInHead)`: first person returns retail's `ACECameraRetail::DefaultPivotZAc` (1.5); third person returns the `PersonalCameraHeight` option. The caller multiplies by `GetCameraScaleCm()`.
- Limits: `MaxDistanceAc` 450, `MinDistanceAc` 0.5, pitch -89 to 20 degrees. `MaxArmLength(Rotation, ScaleCm)` = 450 x scale with `FreeZoom()`, else `ACECameraRetail::MaximumArmLength(Rotation, ScaleCm)`.
- `Restore(Boom, UserArmLength, ScaleCm)`: with `SavePosition()` on, reads `Distance` and `Pitch` from section `personal.Camera` of `GameUserSettings.ini`, sets the pitch (clamped), then the arm length clamped to `MaxArmLength` at that pitch (and to 450). Runs after the client resets the camera to retail defaults at login. With no saved values, or the option off, the retail defaults stay. Then it records the current position as seen, and as saved only with the option on.
- `Update(...)`: called every frame. With `SavePosition()` off it writes nothing and marks the position as not saved, so turning the option on writes the current position after 1 second, moved or not. With it on: when distance and pitch have been unchanged for 1 second and differ from the saved values, writes both and flushes the file. Not saved in first person, look-down or map view. A login alone does not write the file.
- Turning `Free Camera Restriction` off while zoomed out past the angle limit: the camera stays; zooming in works; zooming out is refused until inside the limit (the zoom loop stops at a step out of bounds). Nothing else limits zooming out: `SyncUserCameraArmLength` only keeps the minimum, the 10-unit cap at `ACEPlayerController.cpp` lines 4224 to 4228 is look-down only, and `ApplyCameraOffset`, map mode and the reset set fixed lengths (checked in v158).
- Distance is stored in AC units, so it does not depend on the world scale.
- State is in file-level variables (one camera, game thread only).

### Tests (`ACE.Personal.Camera`)

Height range; both rows exist and are off by default (an unknown key reads as 1); `MaxArmLength` equals `MaximumArmLength` at pitch -89, -45, -20, 0 with Free off and 45000 (scale 100) with Free on; Save off: a saved position is not applied and nothing is written after the delay; turning Save on writes the unmoved position; Save on with Free off: a saved 450 at pitch -17 comes back limited to the angle limit; Save and Free on: the delay, special views, restore and clamps. `ACE.RetailParity.CameraAndEdges` passes unchanged: it runs on its own empty settings file (`CameraFixture.ini`), so Free Camera Restriction is off there whatever a player saved.

### Baseline

Not measured: nothing drawn changes, so no frame-time or memory cost is expected (47a2e4015 and 156bdf587, v158).

### Depends on (Thwargle's code)

`ApplyRetailCameraPivot`, `UpdateOrbitCamera`, `StepCameraZoom`, `ResetCameraToRetailDefaults`, `UserCameraArmLength`, `GetCameraScaleCm`, `bCameraInHead`, `bCameraLookDown`, `bCameraMapMode`; `ACECameraRetail::DefaultPivotZAc`, `MaximumArmLength`, `CloserMinLengthAc`, `ZoomMultiplier`; `ACERuntimeOptions::Get`; the settings window's `Section`, `Fixed`, `Label`, `RoundToggle`, `Box`. If `ACECameraEdgeTests.cpp` stops using its own settings file, Free Camera Restriction saved on would break its zoom-limit step.

### For later features

- A crosshair or aimed-combat view has to account for the pivot height: the camera may not orbit a fixed 1.5 units above the feet.
- Any new camera mode that should not overwrite the saved position must be added to the `bSpecialView` argument of `Update`.
- The zoom limit applies to mouse-wheel and key steps in `StepCameraZoom` only; other code that sets the arm length is not limited by it.
- The settings window's Defaults button does not reset `UPersonalOptionCheckBox` check boxes (as for the far options).

## 2. Far terrain

| | |
|---|---|
| Merged | 2026-11-11, client v155, commit 4f171d272 |
| Changed | 2026-11-12 by far terrain textures (section 3), commit d20621031 |
| Changed | 2026-11-13, commit f2c7e6e20, client v157: check box `Far Terrain`, radius 43 to 96. See "Settings" below |
| Changed | 2026-11-14 by far scenery (section 5), commit bd40e8370: off by default; shown inside buildings too (`NeedsExteriorTerrain` in place of "indoor cell"), hidden in dungeons, portal space and at login; a number under the slider |
| Settings | Check box `Far Terrain` (`PersonalFarTerrain`), off by default since bd40e8370 (on before), saves and applies at once. Slider `Far Terrain Radius` (`PersonalFarTerrainRadius`), 43 to 96 landblocks, default 43. Until f2c7e6e20: 0 = off, default 48, max 96 |

### What it does

One mesh (the shell) continues the terrain from the edge of the loaded landblocks out to the radius setting. No collision, no shadows.

### Files

| File | Role |
|---|---|
| `Private/Personal/PersonalFarTerrainBake.h/.cpp` | World data: height and terrain type of every 24 m cell corner, read from the DAT files and cached |
| `Private/Personal/PersonalFarTerrainMesh.h/.cpp` | Builds the shell's vertex, triangle, normal and UV arrays |
| `Private/Personal/PersonalFarTerrainSubsystem.h/.cpp` | World subsystem: follows the player, owns the actor, mesh, material and texture |
| `Private/Personal/PersonalFarTerrain.h/.cpp` | Namespace `PersonalFarTerrain`: `OptionKey`, `RadiusKey`, `IsOn()`, `Radius()` (f2c7e6e20) |
| `Private/Tests/Personal/PersonalFarTerrainBakeTests.cpp`, `PersonalFarTerrainShellTests.cpp` | Tests `ACE.Personal.FarTerrain.Bake` and `ACE.Personal.FarTerrain.Shell` |
| T `Public/ACERuntimeOptions.h` lines 32 and 33 | Rows `{PersonalFarTerrain, "Far Terrain", 0, 0, 1}` and `{PersonalFarTerrainRadius, "Far Terrain Radius", 43, 43, 96}` |
| T `Private/UI/ACEVideoSettingsWidget.cpp` lines 146 to 148 | The `Far Terrain` check box, the slider (step 1) and the number under it |
| T `Public/ACEDatSubsystem.h` line 309 | Added getter `GetWorldEmissiveScale()` |

The edits are one line each, marked `// personal: far-terrain` (the number under the slider is marked `far-scenery`).

### World bake (`FPersonalFarTerrainBake`)

- Grid: 2041 x 2041 cell corners (255 landblocks x 8 cells + 1). Index `X * Dim + Y`, X and Y in cells from the world's low corner. Lookups outside the world return the edge value.
- Per corner: `HeightIndex` (uint8 into `HeightTable[256]`, AC units, from the region file in the portal DAT), `TerrainRaw` (low 7 bits of the DAT terrain word: road in bits 0-1, terrain type in bits 2-6) and `TerrainCode` (terrain type 0 to 31, or 32 = road; derived from `TerrainRaw` with `CodeOfRaw`). Since bd40e8370 also `SceneType`.
- `Palette[33]`: one average color per terrain code, sampled from the client's own baked terrain textures (`FACELandblockMeshBuilder::BakePCode`, every eighth pixel). The bytes are averaged as they are, the way the mips of loaded terrain are made, so both look the same from far away.
- Height levels: level 0 is the real heights. Levels 1 to 4 have one sample every 2, 4, 8, 16 cells, each a 1-2-1 average of the level below. `HeightAt(Level, X, Y)` takes world cell coordinates at every level.
- Color texture pixels: 2048 x 2048, one texel per cell; freed after the texture is created.
- Reading: opens its own handles to `client_portal.dat` and `client_cell_1.dat` (the game's cell DAT handle is not public and shares a lock with terrain streaming). Reads all 65,025 landblocks `0xXXYYFFFF` with `ACEDatUnpack::UnpackCellLandblock`. Fails if more than half are missing.
- Cache: `Unreal/Saved/Personal/FarTerrain_<key>.bin`, magic `PAFT`, version 5 since bd40e8370 (12.5 MB). Key = hash of path, size and modified time of both DAT files, plus the version. Levels and color pixels are rebuilt at each load, not cached.
- Timing: about 5 s for the first build; later starts load the cache. Log lines start with `PersonalFarTerrain:`.
- Threading: `LoadOrBuildAsync` runs on the thread pool and returns a shared pointer (`FBakePtr`). After that the bake is read-only, except that the game thread empties `ColorPixels` once.
- Failure: one warning, the pointer stays null, the shell stays off for the session.

### Shell mesh (`PersonalFarTerrainMesh::Build`)

- Square rings of quads around the center landblock, built from the inside out.
- Quad size per level K: 24, 48, 96, 192, 384 m (1, 2, 4, 8, 16 cells).
- Reach: level 0 reaches `FineLimit = max(8, HoleRadius + 1)` landblocks; each next level reaches twice as far; the last level ends at the radius. At Landscape Radius 5 the levels end at 8, 16, 32, 64 and the radius.
- Holes: landblocks the client has spawned are left out, per landblock, so the shell fits any loaded shape. Only level 0 borders loaded terrain.
- Join with loaded terrain: level 0 uses the real heights and the same diagonal per cell (`FACELandblockMeshBuilder::GetSplitDir`), so there is no gap or step.
- Between levels: a border vertex takes the coarser level's height, or the midpoint of its two coarser neighbors, so there are no cracks. Border vertices are shared.
- Normals: central difference of `HeightAt(K, ...)` over one step each side (48 m at level 0, up to 768 m at level 4). AC normal is (-SlopeX, -SlopeY, 1); `AceVectorToUnreal` flips X, so the code stores (SlopeX, -SlopeY, 1).
- UVs: `(cell + 0.5) / 2048` into the color texture.
- Vertices are relative to the low corner of the center landblock; the actor sits at `PersonalFarTerrainMesh::Origin(Center, WorldScale)`.
- Size: about 100,000 triangles.
- Two triangle lists over the same vertices: `Triangles` with `UVs` (color map) and `DetailTriangles` with `DetailUVs` (detail texture, section 3).

### Subsystem (`UPersonalFarTerrainSubsystem`)

- World subsystem, game worlds only. A timer calls `Update()` every 0.1 s; nothing runs per frame.
- Finds the `UACETerrainPresenterComponent` in the world and reads `LoadRadius`, `WorldScale` and `IsLandblockSpawned`.
- Player landblock from `UACEClientSubsystem::GetPlayerPosition().CellId`.
- Hidden when `PersonalFarTerrain::IsOn()` is false, at login, in portal space (`IsInPortalSpace`), in a cell where `NeedsExteriorTerrain` is false (a dungeon), or while the player's landblock is not yet spawned.
- Rebuild when the center landblock, the radius or the set of spawned landblocks (within `LoadRadius + 2`) changes. The arrays are built on a worker and applied on the game thread with `CreateMeshSection_LinearColor`. One build at a time.
- Material: a dynamic instance of the land material (`UACEDatSubsystem::EnsureAceLandMaterialBase`), so fog and day and night lighting match the terrain. Parameters set: `ACETexture` = color texture, `LandDepthBias` = 100 cm (loaded terrain wins where both cover the same ground), `EmissiveStrength` = `GetWorldEmissiveScale()` when it changes.
- Texture: `FACEDatTextureResolver::CreateTransientRgbaWithMips`, clamped, trilinear. Outside the world it repeats the edge color.
- Console: `personal.FarTerrain.Freeze` (stop following the player), `personal.FarTerrain.Wireframe` (editor builds).

### Settings (f2c7e6e20)

With the sky horizon feature (section 4), a small radius leaves a band of black with stars under the cloud rim. At Mizuda (90.25S 8.65E, hilltop about 172 m above the sea) it starts below radius 32 or 33. The slider starts at 43 (45% of 96), which covers ground up to about 230 m above the far terrain's edge (`radius = height / 5.36 m`). From higher ground a thin band can still show at 43. The 43 is in one place, the option row (as default and as minimum).

- `PersonalFarTerrain::IsOn()`: the check box is on and the saved radius does not round to 0. Before the check box, radius 0 turned the far terrain off, so a saved 0 still means off.
- `ACERuntimeOptions::Get` and `Set` limit a value to the row's range, so a saved 0 comes back as 43. `IsOn()` therefore reads the saved radius itself: `GConfig->GetFloat("ACE.Presentation", "PersonalFarTerrainRadius", ..., GGameUserSettingsIni)`.
- `PersonalFarTerrain::Radius()`: `Get` rounded to a whole number. A saved 1 to 42 is used as 43; a saved 48 stays 48.
- The subsystem's `Update()` uses `IsOn() ? Radius() : 0`.
- The slider takes its range from the option row (`AddValue`).
- The check box works without a relog: the subsystem reads the settings every 0.1 s, and `UPersonalOptionCheckBox::Changed` builds the sky again for `PersonalFarTerrain` (section 4).
- Tests: the settings part of `ACE.Personal.FarTerrain.Shell` (range, nothing saved, saved 0, 1, 42, 43, 48, 96 and others, slider at its lowest, check box). It uses a fixture settings file.
- A failed world bake is not part of `IsOn()`: that state is inside the subsystem object.
- Known gap: the settings window's Apply saves every slider, so with an old saved 0 any Apply writes 43 and the far terrain comes on. Nothing builds the sky again then, so the stars extension is missing until the next sky build (next game day, a portal, or a toggle of either check box).

### Checked

- Build; `ACE.Personal`, `ACE.RetailParity.CameraAndEdges`, `ACE.RetailParity.Weather`, `ACE.RetailParity.NetworkWeather` pass with `-RenderOffscreen` (2026-11-13). In game at Mizuda 90.25S 8.65E (v157): the slider starts at 43 with no cut-off, Far Terrain off and on again works, no strip at the horizon.
- Clear weather (Mizuda 90.25S 8.67E, v157): the outer edge of the shell does not show past the fog end.
- Brightness between loaded terrain and the far terrain: no fix needed. `FACELandblockMeshBuilder::BuildLandblock` produces down-facing normals, but `AcePrepareLandSectionLighting` (`ACEDatSubsystem.cpp`) flips them up before every `CreateMeshSection_LinearColor` call in `ApplyLandblockToProceduralMesh`. The shell's normals point up too, and both use the land material with the same parameters except `LandDepthBias`. In game there is no significant step; the far terrain may be slightly lighter.

### Baseline

FPS overlay, Landscape Radius 5.

| Spot | Client and commit | Before | After | Memory | Notes |
|---|---|---|---|---|---|
| Mizuda 90.4S 8.8E (before), 90.25S 8.65E (after), radius 96 | v155, 4f171d272 | 218 FPS, 4.6 ms, no far terrain | 225 FPS, 4.4 ms | not measured | Before in Rainy, after in hazy daylight from another camera angle; not the same session, so it shows no cost, not an exact one |
| Mizuda 90.24S 9.17E, camera high above the land, radius 48 | v155, 4f171d272 | not measured | 199 FPS, 5.0 ms | not measured | Much more land in view |
| Mizuda 90.25S 8.67E, clear weather | v157 | not measured | 221 FPS, 4.5 ms | not measured | The shell's edge does not show past the fog end |
| Mizuda 90.25S 8.65E, looking over the sea, radius 96, with far terrain textures and the horizon on | v157, f2c7e6e20 | not measured | 215 FPS, 4.6 ms | 1,983 MB system, 3,114 MB video (no before) | Daylight, haze; settings window open |
| Hoshino 81.02N 43.56W, fresh sessions minutes apart, radius 78, with textures and horizon on | v157, bd40e8370 | every far option off: 1,711 MB system, 2,827 MB video | 1,878 MB system, 2,957 MB video | +167 MB system, +130 MB video | Memory only; far terrain, textures and horizon together |

### What differs from the first design

- 24 m quads next to loaded terrain (design: 192 m), five levels up to 384 m (design: three, up to 768 m).
- Land material with a color texture (design: vertex colors on `M_ACEVertexColor_v1`).
- Holes follow the spawned landblocks (design: one square hole).
- Bake keeps every cell corner (design: landblock corners only).
- Maximum radius 96.

### Depends on (Thwargle's code)

`EnsureAceLandMaterialBase` and material parameters `ACETexture`, `EmissiveStrength`, `LandDepthBias`; `IsLandblockSpawned`, `LoadRadius`, `WorldScale`, `NeedsExteriorTerrain`; `GetSplitDir`, `BakePCode`, `GetPalCode`, `EnsureTexMerge`, `VertexDim`, `RegionFileId`; `UnpackCellLandblock`, `UnpackRegionLandHeightTable`; `IsInPortalSpace`, `GetDatDirectory`; the settings section name `ACE.Presentation` and the clamp in `ACERuntimeOptions::Get`. A rename shows as a build error. The join relies on the client drawing loaded landblocks at full detail (v155); if coarse rings come back, the join needs rework.

### For later features

- The bake is a world height source. `HeightAt(X, Y)` and `HeightAt(Level, X, Y)` work for any cell in the world, loaded or not.
- The bake is owned by the subsystem and only loads when the shell is first needed. A feature that needs it earlier, or with the far terrain off, has to request it itself or move ownership. `UPersonalFarTerrainSubsystem::GetBake()` shares it.
- Changing what the cache file holds needs a new `FarTerrainCacheVersion`.
- The shell uses the land material, so any change to that material or to land lighting applies to it too.
- Past the textured part (section 3), colors are one average per terrain type and roads are one color. No water surface on the shell.

### Open

None.

## 3. Far terrain textures

| | |
|---|---|
| Merged | 2026-11-12, client v155, commit d20621031 |
| Setting | Check box `Far Terrain Textures` (`PersonalFarTerrainTextures`), off by default since bd40e8370 (on before), saves at once. Has an effect only with the far terrain on |

### What it does

The far terrain within 14 landblocks of the player (about 2.7 km) is drawn with the same terrain textures as loaded terrain, roads included. Farther out it keeps the flat color map. About 85 MB of video memory and about the same system memory (from the texture size).

### Files

| File | Role |
|---|---|
| `Private/Personal/PersonalFarTerrainDetail.h/.cpp` | The detail texture: content, tile cache, what to upload after a move |
| `Private/Personal/PersonalOptionCheckBox.h/.cpp` | Reusable check box for any on/off row of `ACERuntimeOptions` |
| `Private/Personal/PersonalFarTerrainBake.h/.cpp` | Changed: keeps road flags (`TerrainRaw`), `PalCodeAt`, byte averaging |
| `Private/Personal/PersonalFarTerrainMesh.h/.cpp` | Changed: second triangle list and UV set for the textured part |
| `Private/Personal/PersonalFarTerrainSubsystem.h/.cpp` | Changed: owns the detail texture and material, two mesh sections |
| `Private/Tests/Personal/PersonalFarTerrainDetailTests.cpp` | Test `ACE.Personal.FarTerrain.Detail` |
| T `Public/ACERuntimeOptions.h` line 36 | Row `{PersonalFarTerrainTextures, "Far Terrain Textures", 0, 0, 1}` |
| T `Private/UI/ACEVideoSettingsWidget.cpp` line 30 | Include |
| T `Private/UI/ACEVideoSettingsWidget.cpp` line 154 | The check box, below Far Terrain Rooms |

The edits are marked `// personal: far-textures`.

### Detail texture (`FPersonalFarTerrainDetail`, namespace `PersonalFarTerrainDetail`)

- One 4096 x 4096 texture, 16 pixels per 24 m cell, so 256 cells or 32 landblocks across. 13 mips.
- It wraps: world cell (X, Y) is always at pixel (X x 16, Y x 16) modulo 4096. Mesh UVs (`CornerUV`: cell / 256, not wrapped) do not depend on where the player is, and a move of one landblock rewrites only the landblocks that came into range.
- Holds landblocks center - 16 to center + 15 (`TextureRings` 16). The mesh uses center - 14 to center + 14 (`MeshRings` 14), two rings fewer, so a texture and a mesh made one landblock apart still fit. `Covers` is true when texture and mesh centers are at most one landblock apart.
- Tiles: per terrain palette code (`Bake.PalCodeAt`, the same code loaded terrain uses for the cell), the texture from `BakePCode` (1024 px source) reduced with `FACEDatTextureResolver::BuildOpaqueMip` to 16, 8, 4, 2 and 1 pixels (341 pixels per tile). Stored already turned to match loaded terrain's mapping (U = 1 - x, V = 1 - y in `BuildLandblock`, `CornerU` and `CornerV`).
- A code with no texture falls back to the flat color.
- Mips smaller than one pixel per cell mix cells, so they are rebuilt from the 1-pixel-per-cell mip (`LastCellMip`) after every update.
- `Update(Bake, Center, BakeTile, Out)` returns the rectangles to upload. When more than a quarter of the landblocks changed it sends whole mips.
- Tile cache: `Unreal/Saved/Personal/FarTerrainTiles_<key>.bin`, magic `PAFD`, version 1, same key as the world bake. Loaded on first use, saved after an update that made new tiles.
- DAT access: own handles and an own `FACELandblockMeshBuilder` (`MakeDatTileSource`), opened on first use, used from one thread at a time.
- Measured in the test (Mizuda): first texture 10.0 s for 349 tiles with no cache; one landblock step 5.5 ms.

### In the subsystem

- `UpdateDetail(Center, DatDirectory)` runs from `Update()`. Work is done on a worker; `ApplyDetail` copies the uploads into the texture on the game thread with `UpdateTextureRegions`. One update at a time (`bDetailBusy`).
- The texture is a transient `PF_B8G8R8A8` sRGB `UTexture2D`. `DetailMaterial` is a second dynamic instance of the land material with `ACETexture` = detail texture and the same `LandDepthBias` and `EmissiveStrength` as the shell material.
- Mesh section 0 is the flat-color part, section 1 the textured part. The mesh is rebuilt when the detail center it was built for changes. With the option off, or before the first texture is ready, everything is in section 0.
- `PersonalFarTerrainMesh::Build` takes an optional `DetailCells` rectangle (`MeshCells(TextureCenter)`); quads fully inside it go to `DetailTriangles`.

### Check box (`UPersonalOptionCheckBox`)

`Make(WidgetTree, Style, Label, Key, Tooltip)` creates a check box for an on/off option row. It shows the saved value when the window is built and writes a change at once (`ACERuntimeOptions::Set`, then a flush of `GameUserSettings.ini`), so it does not use the Apply button. The key is also the widget name, so it cannot contain a dot. The Defaults button does not reset it.

### Baseline

Not measured: about 220 FPS with it on was reported in game (v155, d20621031); spot, weather and frame time were not recorded. Memory together with the far terrain and horizon is in section 2.

### Depends on (Thwargle's code)

Everything in section 2, plus `FACEDatTextureResolver::BuildOpaqueMip`, `FACELandblockMeshBuilder::GetPalCode`, `BakePCode`, `EnsureTexMerge`, `UACEHoverTooltipWidget::SetWidgetTooltip`, `ACERuntimeOptions::Set`. It relies on the cell texture mapping in `BuildLandblock` (`CornerU`, `CornerV`); if that changes, the textures turn at the join.

### For later features

- New on/off settings can use `UPersonalOptionCheckBox` with one line in the settings widget.
- `Bake.PalCodeAt(X, Y)` gives the terrain palette code of any cell in the world.
- The textured part and loaded terrain use the same material and textures, so a remaining difference between them is geometry, normals or lighting, not texture.

## 4. Sky horizon

| | |
|---|---|
| Merged | 2026-11-13, client v157, commit 15da55025 |
| Changed | 2026-11-13, commit f2c7e6e20: the feature is off when the far terrain is off |
| Changed | 2026-11-14, commit bd40e8370: label `Far Terrain Horizon` (was `Night Sky To Horizon`), off by default (was on) |
| Setting | Check box `Far Terrain Horizon` (`PersonalSkyHorizon`), off by default, saves and applies at once. Has an effect only with the far terrain on (`PersonalFarTerrain::IsOn()`, section 2) |

### What it does

The stars layer of the sky (`0x010015EF`) is the upper half of a cube, open at the bottom, so it ends at eye level. Below eye level the day dome is not covered at night and showed as a bright strip between the far terrain and the sky. The walls of the stars layer now continue straight down by the cube's half-width (to -45 degrees at the wall centers, -35.3 at the corners), below the dome's lowest edge (-25.5). Nothing above eye level changes. With the option off the sky is as before.

### Files

| File | Role |
|---|---|
| `Private/Personal/PersonalSkyHorizon.h/.cpp` | Namespace `PersonalSkyHorizon`: `StarsGfxId`, `OptionKey`, `ExtendRim`, `ExtendSection`, `RebuildSky` |
| `Private/Personal/PersonalOptionCheckBox.cpp` | Changed: after saving `PersonalSkyHorizon` or `PersonalFarTerrain` it calls `RebuildSky` |
| `Private/Tests/Personal/PersonalSkyHorizonTests.cpp` | Test `ACE.Personal.SkyHorizon` |
| T `Private/ACESkyDomeActor.cpp` line 27 | Include |
| T `Private/ACESkyDomeActor.cpp` line 474, `RebuildSlotMesh` | `PersonalSkyHorizon::ExtendSection(GfxId, Verts, Triangles, Norms, UVs, Colors)` right after the loop that applies the bind transform, before the textured and the vertex-color path split |
| T `Public/ACERuntimeOptions.h` line 37 | Row `{PersonalSkyHorizon, "Far Terrain Horizon", 0, 0, 1}` |
| T `Private/UI/ACEVideoSettingsWidget.cpp` line 155 | The check box, the last of the far rows |
| T `Private/Tests/ACERetailWeatherTests.cpp` lines 31 and 41 | Include, and the option set to 0 for the length of `ACE.RetailParity.Weather` (restored at the end) |

All edits are marked `// personal: sky-horizon`.

### How it works

- `ExtendSection` runs for every section of every sky mesh. It does nothing unless the gfx id is `StarsGfxId`, the option is on and `PersonalFarTerrain::IsOn()` is true: with the far terrain off the sky is the same as in Thwargle's client, whatever the check box says. It logs `PersonalSkyHorizon: stars layer extended below eye level by N triangles` at each sky build.
- `ExtendRim(Vertices, Triangles, Normals, UVs, Colors)` is a pure function on the section's arrays, in component space (Z up, after the bind transform):
  - Corners are matched by position (tolerance 0.0001 of the largest coordinate), because the mesh builder gives every polygon its own vertices.
  - A face is counted once per set of corners, so a back face (same corners, other winding) is the same face.
  - A rim edge has both ends at height 0, belongs to one face, and that face's third corner is above it.
  - Per rim edge and per triangle on it (front and back), it adds two vertices straight below the ends, lower by the half-width (largest |X| or |Y| of the rim), and two triangles `To, From, below From` and `To, below From, below To`, which face the same way as the triangle above.
  - UVs: the change per unit of height is taken from the triangle above and continued, so the texture keeps its scale and direction. Normals and colors are copied from the rim vertex.
  - Existing entries are not changed and stay first. Returns the triangles added; 0 for a closed mesh, no edge at 0, an index out of range, or empty input.
- `RebuildSky` calls `AACESkyDomeActor::InvalidateAndRebuild` (public; the call the client makes after a portal) on every sky actor in a game world. The sky is gone for a moment and weather effects restart.
- The sky materials (`bRetailSky` in `CreateAceOverlayMaterial`) move every vertex out along the eye ray by a factor of 1024 and test depth, so the added quads draw behind terrain like the rest of the sky.

### Sky facts (DAT, checked 2026-11-13, v157)

- Stars section: surface `0x0800004D`, 60 vertices, 20 triangles (10 polygons, each with a back face). 16 triangles and 16 vertices are added. Half-width 2627 m at sky scale 250 (`WorldScale` 100 x `SkyDistanceScale` 2.5). Rim at exactly 0.
- Stars UVs: 0.398 to 4.602 across a wall, 1.449 (top) to 3.551 (rim) down; the texture tiles about 4.2 x 2.1 times per wall. Below the rim it continues to 5.652. Material: `M_ACESkyTranslucentWrap`, one-sided.
- The stars are obj[1] in all 20 day groups, not timed, angle 0: never tilted. Heading changes turn the layer about the vertical axis only.
- Stars transparency by day fraction (same in every group): 0 from 0.96 to 0.16, 70 at 0.21, 100 from 0.27 to 0.84, 30 at 0.90.
- Day groups: 0, 1, 2, 4, 5, 6 Sunny; 8, 10, 11 Clear; 12, 13, 14 Cloudy; 3, 7, 9, 15 to 19 Rainy. Sunny, Clear and Cloudy have 7 objects; Rainy has 17 or 19 (rain cylinders `0x01004C44`, `0x01004C42` and effect scripts).
- Domes (obj[0]): `0x010015EE`, `0x010015F0`, `0x010015F1`, `0x010015F2`, all 84 vertices from 45.0 down to -25.5 degrees.
- Cloud layer: every group has one (props 0x2, scrolling): `0x01004C36`, `0x01004C37`, `0x01004C38`, `0x010015B6` (Sunny), `0x01004C39`, `0x01004C3A` (Clear), `0x01004C35` (Cloudy, Rainy). 72 vertices from 90.0 down to -1.6 degrees; only `0x010015B6` (group 6) ends at 0.0. Opaque from day fraction 0.02 to 0.96, luminosity 15 at night; fully transparent only at midnight.
- A Dereth day is 7620 s. The weather changes at day fraction 0.

### What differs from the first design

- The call is after the bind transform loop, not before the textured `CreateMeshSection_LinearColor`, so one line covers both paths.
- The check box applies at once through `InvalidateAndRebuild`, not at the next sky rebuild.
- A 4 degree limit on the extension was considered and dropped: the depth of the extension stays as built, and the band under the clouds is avoided by the far terrain radius starting at 43 (section 2, "Settings").
- `ACERetailWeatherTests.cpp`: with the option on, `ACE.RetailParity.Weather` failed in two places: "Sky retains authored vertices and winding" (it compares the drawn stars mesh with the DAT mesh) and "Rainy night horizon retains its authored purple tint in every direction" (it expects the purple dome below eye level). The test now runs with the option off, which also checks that the sky is unchanged then.
- Every weather has a cloud layer, and its rim at -1.6 degrees is where the picture now changes below eye level (see Open).

### Checked

- Build; `ACE.Personal`, `ACE.RetailParity.Weather` and `ACE.RetailParity.NetworkWeather` pass with `-RenderOffscreen` (2026-11-13). Full `ACE` suite not run.
- Captures of the weather test (Rainy, day fraction 0.1, no terrain, option on against off): from 0 to -1.6 degrees the color matches the sky above (88, 54, 80 against 87, 54, 79; it was 113, 72, 119). Below -1.6 it is black with stars (8, 5, 5) where it was the dome (85, 50, 95).
- In game, editor, Rainy, day fraction 0.88 to 0.95 (dusk into night): good.
- Packaged client (f2c7e6e20, v157), Mizuda 90.25S 8.65E: no strip at the horizon at dusk or night; with Far Terrain off the check box changes nothing; with it on again the far terrain and sky come back without a relog.

### Baseline

Not measured: no frame-time or memory compare with the option on and off was made (v157, 15da55025). Readings with far terrain, textures and horizon on together are in section 2.

### Depends on (Thwargle's code)

`AACESkyDomeActor::RebuildSlotMesh` (the arrays `Verts`, `Triangles`, `Norms`, `UVs`, `Colors` and the place after the bind transform loop), `InvalidateAndRebuild`, `ACERuntimeOptions::Get`, `UACEHoverTooltipWidget::SetWidgetTooltip`. It relies on the stars mesh staying a half cube with its rim at height 0 and on the stars slot not being tilted; `ACE.Personal.SkyHorizon` checks both against the DAT files. A change to `RebuildSlotMesh` shows as a merge conflict or a build error. If `ACE.RetailParity.Weather` changes, keep the option off for it.

### For later features

- `ExtendRim` works on any section with an open edge at height 0, not only the stars.
- A mesh change for another sky object can use the same place in `RebuildSlotMesh`: `ExtendSection` already gets every section with its gfx id.
- Another setting that needs the sky built again can call `PersonalSkyHorizon::RebuildSky`.
- The sky facts above save reading the DAT again for sky work.

### Open

- Below the cloud rim (-1.6 degrees) the night sky is black with stars instead of the dim dome, with an edge at the rim. It shows where the far terrain ends lower than that. With `Far Terrain Radius` at 43 or more this is left only from ground more than about 230 m above the far terrain's edge. Seen in game at radius 0, 6, 14 and 32, before the slider started at 43.
- Not checked in game: Sunny, Clear, Cloudy, dawn, FPS against the baseline.

## 5. Far scenery (far terrain objects)

| | |
|---|---|
| Merged | 2026-11-14, client v157, commit bd40e8370 |
| Settings | Check box `Far Terrain Objects` (`PersonalFarScenery`), off by default. Slider `Far Terrain Objects Radius` (`PersonalFarSceneryRadius`), 3 to 8 landblocks, default 6, saved with Apply. Check box `Hide Far Terrain with Fog` (`PersonalFarSceneryFog`), off by default |
| Also changed | Defaults of `Far Terrain`, `Far Terrain Textures` and the horizon check box from on to off; the horizon label to `Far Terrain Horizon`; far terrain and far scenery stay on inside buildings |

### What it does

Trees, rocks, stabs and buildings of the landblocks past the client's own scenery (2 landblocks) are drawn out to the radius setting, at the same place and size as when the player gets close. Instances only: no collision, no shadows, no animation, light or sound. Models get simpler with distance. Needs the far terrain on. With the check box off nothing is loaded or drawn.

### Files

| File | Role |
|---|---|
| `Private/Personal/PersonalFarScenery.h/.cpp` | The rules, no state: rings, size rule, fog ring, wanted set, handoff; the three settings (`IsOn`, `Radius`, `HideWithFog`) and their console variables |
| `Private/Personal/PersonalFarSceneryPlacement.h/.cpp` | Where a landblock's objects go, from the world bake and the DAT files: RegionDesc scenery (an own copy of `UACEDatSubsystem::CollectRegionScenery`), stabs and buildings of the info file. `TypeKey` |
| `Private/Personal/PersonalFarSceneryMeshes.h/.cpp` | The DAT's lower-detail models as own meshes (`Build`, `MakeStaticMesh`); panel meshes for building openings, used by section 6 |
| `Private/Personal/PersonalFarScenerySubsystem.h/.cpp` | `UPersonalFarScenerySubsystem`: streaming, units, components, handoff, measuring tools |
| `Private/Personal/PersonalSliderLabel.h/.cpp` | `UPersonalSliderLabel`: the number under a slider |
| `Private/Personal/PersonalFarTerrainBake.h/.cpp` | Changed: `SceneType` per corner, `TerrainWordAt`, cache version 5 |
| `Private/Personal/PersonalFarTerrainSubsystem.h/.cpp` | Changed: `GetBake()`; shown inside buildings (`NeedsExteriorTerrain`) |
| `Private/Tests/Personal/PersonalFarSceneryTests.cpp` | `ACE.Personal.FarScenery.Placement`, `.Rules`, `.Levels`, `.Draw` |
| T `Public/ACEDatSubsystem.h` line 310 | `float GetWorldFogEndCm() const` |
| T `Public/ACELandblockActor.h` line 95 | `bool IsNearSceneryShown() const { return bFullResDetail && bSceneryQueued && IsSceneryComplete(); }` |
| T `Public/ACERuntimeOptions.h` lines 34, 35 and 38 | The three rows |
| T `Private/UI/ACEVideoSettingsWidget.cpp` lines 31 and 146 to 155 | Include; the far rows in the order Far Terrain, its slider and number, Far Terrain Objects, its slider and number, Hide Far Terrain with Fog, Far Terrain Rooms (section 6), Far Terrain Textures, Far Terrain Horizon |

New edits are marked `// personal: far-scenery`; rows of the other far features keep their own markers.

### Placement (`PersonalFarSceneryPlacement`)

- `Open(DatDirectory)`: own DAT handles, the region scene tables (`0x13000000`) and every scene file, read once on a worker.
- `Place(Bake, Source, LandblockId, OutItems)`: fills a `FACEBuiltLandblockMesh` from the bake and runs `CollectRegionScenery` line for line, with heights from the client's `FACELandblockMeshBuilder::SampleHeightAc`. Cells with a building get no scenery. Then the stabs (`Objects`, placement 101) and buildings of the info file `0xXXYYFFFE`, as `AACELandblockActor::QueueScenery` takes them. Safe on any thread; about 0.1 ms per landblock.
- `TypeKey(Id, Kind)`: an object type is its DAT id plus kind (Region, Stab, Building) in the top two bits; a RegionDesc key is the id. Every map "by Setup id" in the subsystem is by this key. `TypeId`, `TypeKind`, `TypePlacement` (101 for stabs) give the parts.

### Rules (`PersonalFarScenery`)

- Ring: landblocks from the player's, the larger of the two directions. `NearRing` 2, `LastRing` 8 (past it the far terrain is smoothed and objects would not stand on it).
- Wanted set: rings 2 to the radius. Ring 2 is prepared and hidden for the handoff.
- Size rule: an object is drawn out to height x scale x factor (`personal.FarScenery.SizeFactor`, 400, about 4 pixels tall at 3440x1440). A type that cannot be seen from anywhere in its landblock's ring is not created.
- Fog rule (only with `Hide Far Terrain with Fog` on): nothing is created past `FogRing = 1 + floor(fog end / 192 m)`; what exists is kept one ring further.
- Handoff: a ring 2 landblock's far objects are hidden while the client's own scenery there is complete and shown (`IsNearSceneryShown`). No fade.
- Animated types (a default animation) are left out; the client draws at most 3 per landblock as live actors. Buildings are never left out. Scripted stabs (torches) are drawn as the plain object.

### Subsystem

- World subsystem, a timer every 0.1 s, nothing per frame. `Update()` checks the settings and where the player is, then calls `Stream`. Off when `IsOn()` is false; hidden at login, in portal space, and in a cell where `UACETerrainPresenterComponent::NeedsExteriorTerrain` is false (a dungeon). Inside a building it stays on.
- Landblocks are placed on a worker, 64 at a time, nearest first. They are drawn in units: one actor with one instance component per object type. A unit is one landblock in rings up to 2 and a square of `personal.FarScenery.Group` landblocks further out (default 1).
- Meshes: level 0 is the client's own static mesh, asked for with the client's cache key for that kind (scenery and stabs: outdoor lit; buildings: `bStencilHoleClip`), so nothing is built twice. Collision is off at the component. Budget `personal.FarScenery.BudgetMs` (1 ms per update); a new mesh is built one per update.
- Detail levels (`personal.FarScenery.Detail`, 1): full below `MiddleRing` (4), middle from there, simplest from `SimplestRing` (5) and for any landblock past the fog end. The simpler models are the DAT's own (GfxObj degrade tables), built on a worker and used only when they have fewer triangles than the level before. Own meshes are in `LevelMeshes` with the client's outdoor lit materials.
- Buildings: one plain `UStaticMeshComponent` each with the client's shell material, full detail at every ring (the shell material's base is used on plain components only).
- Hide and show past the client's rings is spread over updates (about 40 components each).
- Building openings were filled with grey panels when this was merged; since 9157eec40 they are open unless Far Terrain Rooms is on (section 6).

### Console

`personal.FarScenery.Draw` (-1 follows the check box, 0 off, 1 on; the far terrain is needed either way), `.Radius` (0 follows the slider, else 2 to 8), `.FogRule` (-1 follows the check box), `.SizeFactor`, `.BudgetMs`, `.Group`, `.Detail`, `.MiddleRing`, `.SimplestRing`, `.TestSetup` (measuring: one mesh for every type), commands `.LogFrameRate` and `.ScreenPercentage N`. The packaged client has no console key: it runs lines starting with `personal.` from `Unreal/Saved/Personal/FarSceneryCommands.txt` when the file changes after the client started, and a file `Unreal/Saved/Personal/FarSceneryOff` turns the feature off while it exists.

### Where the cost is

- The cost is GPU geometry: about 0.08 ms per million triangles at Sawato. Not the game thread (2.0 to 2.4 ms throughout), not pixels (half resolution: 0.83 against 0.90 ms), not the number of components (6 times fewer: 0.88 against 0.94 ms), not instances (an 8-triangle mesh for all: 0.07 ms).
- Full detail at radius 8 was 0.97 ms at Sawato (11.7 M triangles) and 0.96 ms at Hoshino (5.75 M); three detail steps 0.62 and 0.59 ms.
- Per border the far objects take 5 to 15 ms of game thread in all, spread over about 1 s, with a longest single update of 1.0 to 2.9 ms. The first fill after login takes 6 to 13 s in the background.
- Placement equals the client's own placement on 78 landblocks (9,978 objects, difference 0).

### Baseline

Landscape Radius 5, Far Terrain on, 2026-11-14. Frame times are 10 s averages from the client's frame counter, off and on in one session at the same view, unless noted. Measured on packaged builds of the feature branch with the same code as bd40e8370.

| Spot | Client and commit | Before | After | Memory | Notes |
|---|---|---|---|---|---|
| Sawato 23.53S 52.70E (`0xC162`), swamp forest, radius 6 (default) | v157, bd40e8370 | 5.89 ms (about 170 FPS) | +0.44 ms (about 158 FPS) | not measured | Clear, by day. Radius 5: +0.31, 7: +0.58, 8: +0.67 ms |
| Hoshino 81.02N 43.56W (`0x48E4`), forest ridge, radius 6 | v157, bd40e8370 | 5.94 ms (about 168 FPS) | +0.42 ms (about 157 FPS) | +98 MB system, +20 MB video (fresh sessions, 1,878 to 1,976 MB and 2,957 to 2,977 MB) | Radius 5: +0.35, 7: +0.50, 8: +0.57 ms |
| Qalabar, town and open land, radius 6 | v157, bd40e8370 | 5.79 ms | +0.10 ms | not measured | About 0.10 ms at every radius |
| Sawato, same view, feature off against the morning baseline | f2c7e6e20 before, bd40e8370 after | 169 FPS, 5.9 ms (FPS overlay) | 176 FPS, 5.7 ms | 1,833 to 1,744 MB system, 2,815 to 2,774 MB video | Off is the client as before; other time of day |
| Sawato, running north across four landblock borders (`0xC162` to `0xC166`) | f2c7e6e20 before; an early build at radius 8 after | 190 to 208 FPS running; lowest 177, 171, 182, 197 FPS | 176 to 197 FPS running; lowest 179, 181, 183, 186 FPS | not measured | FPS overlay read from video. The early build had no size rule or detail steps |
| Sawato and Hoshino, second session, radius 6 | v157, bd40e8370 | off | +0.29 ms Sawato, +0.40 ms Hoshino | not measured | Rainy, dusk |
| Sawato at night, radius 6 | v157, bd40e8370 | off | +0.23 ms with Hide Far Terrain with Fog off, about +0.13 ms with it on | not measured | Rainy; the fog moved during the run, so the second figure is rough |

Login and portal time: 5.1 to 5.4 s with it, 5.2 to 5.9 s without.

### What differs from the first design

- Settings: labels, defaults (off, radius 6) and the fog check box; the first design had `Far Scenery`, on, radius 8, and no fog setting.
- Detail levels, units, the fog switch and the inside-building rule were not in the first design.
- Own meshes for the lower levels, so more than "nothing is built twice" at level 0.
- Animated types are left out; the first design drew them in their rest pose.
- The fog rule is off by default (far objects stay as a skyline in fog and at night).

### Checked

- `ACE.Personal`, `ACE.RetailParity.CameraAndEdges`, `.Weather`, `.NetworkWeather` pass (2026-11-14).
- In game, packaged, 2026-11-14, several hours across Dereth: the handoff looks good; no significant hitching; middle and simplest models look good; far buildings stand on the ground and match close up; the night skyline with the fog rule off looks good; all check boxes and both sliders work without a relog; far features stay on inside buildings and are off in a dungeon.

### Depends on (Thwargle's code)

`CollectRegionScenery` (the placement is a copy: `ACE.Personal.FarScenery.Placement` compares the two and fails when the client's rules change), `SampleHeightAc`, `UnpackRegionSceneTables`, `UnpackScene`, `UnpackLandblockInfo`; `RequestSetupMesh`, `FindSetupMesh`, `GetOrCreateSetupStaticMesh` and its cache key, `BindSetupStaticMeshMaterials`, `GetOrCreateOutdoorLitMaterial`, `TryGetSetupPhysics`; `FACESetupMeshBuilder::BuildSetupWithAppearance` with `bDistanceLevels`, `FACEBuiltSetupPart::Portals` and `DistanceLevels`; `AACELandblockActor` (`bFullResDetail`, `bSceneryQueued`, `IsSceneryComplete`; that it hides building shells when a landblock leaves full detail); `NeedsExteriorTerrain`, `IsLandblockSpawned`, `LoadRadius`, `WorldScale`; `WorldFogEndCm`; the shared fog parameter collection; `ACERuntimeOptions`. The handoff relies on the client drawing its own scenery within 2 landblocks (`SetFullResDetail` in `ACETerrainPresenterComponent.cpp`).

### For later features

- `PersonalFarSceneryPlacement::Place` gives every object of any landblock in the world without loading it.
- `UPersonalSliderLabel::Make` puts a number under any slider with one line in the settings widget.
- `UPersonalFarTerrainSubsystem::GetBake()` shares the world bake.
- A command file and an off file let scripts drive the packaged client; the frame rate line gives frame, game, render and GPU times.

### Open

- First use of an object type costs 5 to 54 ms once (seen as a short hitch when a new type first appears). The time is not the mesh: `0x02000322` takes 39 ms at full detail and at its simplest model with 83 triangles. Most likely the first creation of its materials and textures. Not profiled.
- System memory for far objects is near the 100 MB target (98 MB at Hoshino, radius 6). Where it goes was not looked at.
- Within 2 landblocks, a building's openings show through until the client draws its rooms (within 50 m, or when its doorway check admits the building; `AddLookInStabCells` in `ACETerrainPresenterComponent.cpp`). Left for separate work.
- A cellar or dungeon cell in a landblock that also has buildings keeps the far features on (the `NeedsExteriorTerrain` rule).
- No fade at the cull distance or at the handoff; it would need the client's material builder.
- Single-model stabs (GfxObj ids): not checked whether they get simpler models.
- `personal.FarScenery.Group` stays at 1; groups of 4 x 4 measured about the same.
- Radius 7 and 8 are over the 0.5 ms target in forest (0.58 and 0.67 ms at Sawato).

## 6. Far terrain rooms

| | |
|---|---|
| Merged | 2026-11-15, client v157, commit 9157eec40. Built in stages from 2026-11-14 to 2026-11-15 |
| Setting | Check box `Far Terrain Rooms` (option row `PersonalFarWindows`), off by default, under `Hide Far Terrain with Fog`, saves and applies at once. Needs Far Terrain Objects |
| Also changed | Far scenery no longer draws grey panels in building openings: with the check box off, far buildings have open openings, as in Thwargle's client |

### What it does

Far buildings, out to the ring where far scenery draws buildings (`PersonalFarWindows::OuterRing`: the Far Terrain Objects radius, or the fog ring with Hide Far Terrain with Fog), show the rooms behind every opening that is not a door: the client's own room geometry (walls, floors, ceilings) with its baked light, so lit rooms are seen from far away at night in their own colors. Door openings get a brown panel (55,31,16, 25% at night) until the player is within 96 m and the client draws the room behind (the server sends doors by distance). Close up the client's own rooms take over with no visible change.

### Files

All own, in `Private/Personal` unless noted:

- `PersonalFarWindows.h/.cpp`: kinds of opening (`EKind`, `FOpening`, `KindOf`, `LowestOpening`), the door rule (`FDoorRule`, `IsDoorSize`, `IsDoorStart`), the panel show rule (`ShowPanel`, `DoorInReach`), `OuterRing`, `IsPrepared`, `NightAmount`, `DimLevel`, `TestColor`, `IsOn` (cvar `personal.FarWindows.Draw`, -1 follows the check box). Cvars `.Kinds`, `.LogRooms`, `.DoorDistance`, `.DoorColor`.
- `PersonalFarRooms.h/.cpp`: far-room rules: `GetsRooms` (every kind but Door), `AddRooms` (room behind plus the portal's stab list), `IsWanted`, `ShowRooms`, `RoomFog` and `FogStep`, `LiftOffset`. Cvars `personal.FarWindows.Rooms` (-1 follows Draw), `.RoomsBudgetMs`, `.RoomFogNear` (200 m), `.RoomFogFar` (600 m), `.RoomLift` (3 cm), `.RoomsAlways`, `.LogFlicker`.
- `PersonalFarRoomsMeshes.h/.cpp`: own DAT handles and room builder for workers (`Open`), `Build` (a building's far rooms merged, one section per surface, sharing key), `MakeStaticMesh` (the client's room materials, baked light as vertex colors), `ApplyDoorShading` (the client's per-room doorway darkening).
- `PersonalFarRoomComponent.h/.cpp`: `UPersonalFarRoomComponent`, a static mesh component whose scene proxy cannot be occluded (UE 5.8 has no such switch on a static mesh component).
- `PersonalFarWindowsOverrides.h/.cpp`: overrides per building type and opening order (`door`, `open`, `rule`), one-building lines win; the shipped list is read first, the file `Unreal/Saved/Personal/FarWindowsOverrides.txt` adds and wins.
- `PersonalFarWindowsSubsystem.h/.cpp`: `UPersonalFarWindowsSubsystem` (world subsystem, 0.1 s timer, per-frame tick after the terrain presenter): door and test panels (`Stream`, `BuildBlock`, `ApplyShown`, `ApplyLook`), far rooms (`StreamRooms`, `ApplyRoomsShown`, `UpdateRoomFog`, `UpdateRoomLift`, `PruneRoomMeshes`), `personal.FarWindows.Mark door|open|clear`.
- `PersonalFarSceneryMeshes.h/.cpp` (changed): `OpeningKinds`, `MakePanelMesh` (one mesh per opening, sections per kind), `MakePanelMaterial`, `SetPanelColor`. `PersonalFarScenerySubsystem` (changed): panels removed.
- `Unreal/Plugins/ACEClient/Data/Personal/FarWindowsOverrides.txt`: the shipped overrides (21 lines, marked in game).
- Tests `Private/Tests/Personal/PersonalFarWindowsTests.cpp`: `ACE.Personal.FarWindows.Survey` (writes `Unreal/Saved/Personal/FarWindowsSurvey.txt`), `.Rules`, `.Draw`, `.Rooms`, `.RoomsMatch` (`-RenderOffscreen`), `.RoomsRules`, `.RoomsDraw`.
- T edits, one line each, marked `// personal: far-windows`: `Public/ACERuntimeOptions.h` line 39 (option row), `Private/UI/ACEVideoSettingsWidget.cpp` line 153 (check box), `ACEClient.Build.cs` line 57 (stages `Unreal/Plugins/ACEClient/Data/Personal`).

### How it works

- Kinds: an opening polygon of the building's setup (`Part.Portals`) in its building's own space. Door: in a wall, 2.2 to 2.8 m tall, 1.1 to 2.3 m wide or a double door 3.0 or 4.0 m (sizes to 0.1 m, 6 cm slack), starting within 0.5 m of the building's lowest opening or at least 2.5 m above it. Everything else (windows, open entrances, floor and roof openings, over 12 m2) gets far rooms. Overrides change a kind per type and opening order.
- Far rooms: per landblock, the rooms are found on the game thread, built on a worker one landblock at a time, and made into a static mesh on the game thread within `RoomsBudgetMs`. One `UPersonalFarRoomComponent` per building, lighting channel 1 only (the sun is on channel 0, as for the client's rooms), no collision, no shadow, the far building's cull distance. Meshes shared by `FBuilt::Key`, pruned when no building draws them.
- Show rule: shown in rings 2 and up; in rings 0 and 1 while the client does not draw one of the rooms directly behind the building's openings (`IsWorldCellVisible`); hidden for the building the player is in.
- Day fog: the client's room material has `FogAmount` 0; a far room takes `FogAmount` = (1 - night) x smoothstep(200 m, 600 m, distance), in steps of 0.04, on own copies of the client's instances, so it blends with its fogged walls by day; none at night.
- Lift: far rooms stand 3 cm higher than the client's rooms, so the floor does not depth-fight the ground seen through a doorway from far away (1 cm already stopped it in game).
- Test colors (`Kinds 1`): a panel in every opening, doors red, the rest green, always shown; made while on.

### Checked

- Offscreen (`RoomsMatch`, Holtburg): far rooms equal the client's rooms in place (0.000 cm) and in every pixel (none differs by more than 0.01, day and night, with the doorway shading).
- Tests 19 of 19, and 11 of 11 with `-RenderOffscreen` (2026-11-15).
- In game (2026-11-15): Holtburg, Shoushi (night and day), Yaraq, Asheron's Castle, the Marketplace and other towns; day fog blends in; roof openings show their rooms and light; no flicker zoomed out, by day, at night and in fog; no line of color at doors; the check box switches far rooms and door panels live.

### Baseline

Night, Rainy, 2026-11-15. Frame times from `personal.FarScenery.LogFrameRate`, off and on in one session. Measured on a packaged build of the feature branch with the same code as 9157eec40.

| Spot | Client and commit | Before | After | Memory | Notes |
|---|---|---|---|---|---|
| Holtburg A8B6, defaults (Far Terrain 43, objects 6), standing still | v157, 9157eec40 | 4.92 ms (203.1 FPS) | 4.92 ms (203.3 FPS) | Two fresh starts at A8B7: off 2,640 MB system, 3,743 MB video; on 2,600 MB, 3,606 MB. Cost below the difference between sessions | 39 buildings, 299 rooms, 5,517 triangles, 56 door panels |
| Holtburg A8B6, objects radius 8 | v157, 9157eec40 | 5.06 ms | 5.08 ms | not measured | 53 buildings, 403 rooms, 7,542 triangles, 75 door panels |
| Shoushi D953, defaults, standing still | v157, 9157eec40 | 5.86 ms (170.6 FPS) | 5.86 ms | not measured | 34 buildings, 376 rooms, 7,727 triangles, 72 door panels |

### What differs from the first design

- The first design put lit panels in the openings, with near and far fades. In game the panels never matched the client's rooms from far to near, so they were replaced by far rooms. Panels are kept only for doors.
- Doors by size needed overrides: the rule cannot tell an open doorway from one with a door (21 overrides marked in game).
- The flicker (zoomed out, at one angle) was depth fighting of the far room's floor with the ground, found from a recording read frame by frame; the occlusion fix (`UPersonalFarRoomComponent`) was built first and kept.
- A visibility cut and light cards were not needed.

### Depends on (Thwargle's code)

`FACEEnvCellMeshBuilder::BuildEnvCell`, `FACEBuiltEnvCellMesh`, `GetOrCreateEnvCellMaterial`, `FACEDatLandblockBuilding::Portals` (`OtherCellId`, stab list), `FACEBuiltSetupPart::Portals`, `IsWorldCellVisible`, `FindBuildingInfoIndexForIndoorCell`, `GetLandblockInfo`, `FindSetupMesh`/`RequestSetupMesh`, `GetWorldFogEndCm`, the room material `M_ACEEnvCell_v3` (`FogAmount`, `Door0/Door1/Normal0/Normal1`), lighting channel 1 for rooms (`ACEEnvCellActor.cpp`), and everything far scenery depends on.

### For later features

- `personal.FarWindows.Mark` writes overrides for the opening under the center of the screen and groups the parts of a split doorway.
- The internal names still say "far windows" (`personal.FarWindows.*`, `PersonalFarWindows*`).

### Open

- Objects inside rooms (a lamp on a sill) appear only at the handoff.
- A door type with a door in one town and none in another needs a one-building override line (supported in the file, not by Mark).
- Far rooms stop where far scenery stops drawing buildings (the Far Terrain Objects radius).
- Not checked in game: Sunny and Clear weather by day.

## Abandoned

Kept so the same idea is not tried again without new information.

### Fog Distance slider

Built and dropped on 2026-11-12, never merged.

What was built: option `PersonalFogDistance` (1 to 8). The fog end was multiplied by the slider and limited to the edge of the drawn terrain, with a later cap that kept 35% fog where loaded terrain ends (`personal.FarFog.SeamFog`). Code in `Private/Personal/PersonalFarFog.h/.cpp`, one line in `AACESkyDomeActor::UpdateFog`.

Why it was dropped: it made the picture worse.

1. Less fog exposed the flat-colored far terrain and the join with loaded terrain.
2. The fog is linear, so the sea got almost no fog and then a thin bright band at the horizon.
3. Night is dark mostly because of the dark fog; thinning it made loaded and far terrain too bright at night.
4. The seam cap left the slider with no effect in clear weather.

Retail fog depends on weather, time of day and color at once, so one multiplier cannot work. Replacing the fog with Unreal's was rejected: the client disables the exponential height fog because it never reached the emissive DAT materials.

## Since v159

Thwargle's release v159 (82391a7e, 2026.11.17.159) contains the changes above (pull request #1) and changes some of them. Release v160 (7e9348e5, 2026.11.18.160) changes none of them. Both were merged into `gvbz` as b5abfd700; under `Unreal/` the branch now matches Thwargle's `master`. Line numbers in the sections above are for 156bdf587 and may now be off.

- Options window: the far terrain check boxes (`Far Terrain`, `Far Terrain Objects`, `Hide Far Terrain with Fog`, `Far Terrain Rooms`, `Far Terrain Textures`, `Far Terrain Horizon`) are plain check boxes applied with Apply, and Defaults and Reset include them. Apply rebuilds the sky when `Far Terrain Horizon` turns on or off (this was in `UPersonalOptionCheckBox`). Labels, keys and defaults are unchanged.
- Far scenery: `Far Terrain Objects Radius` runs from 3 to 24 (was 3 to 8), default 6. Past ring 8, units are at least 4 x 4 landblocks. Never built past the far terrain radius.
- Far terrain rooms and door panels: limited to ring 8 (`PersonalFarScenery::InteriorRings`).
- Far terrain: the shell is at full 24 m detail out to the objects radius (`SceneryRadius` in `PersonalFarTerrainMesh::Build`). The 100 cm `LandDepthBias` is gone; the shell is placed 1 cm lower. Its materials are registered with `UACEDatSubsystem::RegisterLandSky` (brightness, and with the AC2 sky, cloud shadows and a tint). New `VisibleRadius()`.
- Sky horizon: new `PersonalSkyHorizon::IsEnabled()`. Thwargle's new horizon band (fog color below eye level) is drawn under the extended stars when the option is on and no cloud layer shows.
- Camera options: unchanged.
- Fog, for all players: the fog end follows the drawn area. With the far terrain off, full fog at the edge of the loaded terrain; with it on, clear-weather fog (2,400 m and more in the DAT) reaches up to 4 times as far, up to the edge of the far terrain.

### v161

Thwargle's release v161 (670bc29d, 2026.11.18.161), merged into `gvbz` as bb8d321d3 with no conflicts. Under `Unreal/` the branch still matches Thwargle's `master`. Changes to the features and the fog:

- Far scenery: objects fade out over the last 20% of the objects radius and of each object type's size limit, with a dithered (temporal) fade, in five new far scenery materials (`GetFarSceneryMaterial`, `MPC_ACEFarScenery_v1`). The radius edge follows the player's position, not the camera. Console variable `personal.FarScenery.Fade` (1; 0 turns the fade off for comparison). One extra ring of landblocks is prepared hidden past the radius and one more is kept (at radius 8: 352 landblocks instead of 280). Cull distances cover the whole object bounds.
- Far terrain rooms and door panels: use the far scenery materials with the radius fade only.
- Far terrain: the shell keeps full detail out to the objects radius plus one ring, then blends heights toward the next coarser level over a band of 4 landblocks (levels 1 and 2) or 8 landblocks (coarser levels), so ridges no longer jump when the shell is rebuilt. More vertices (about 62,000 to 96,000 at radius 96). New `FogCoverageDistanceCm`: the fog limit is measured from the camera to the edge of the shell actually drawn, one cell inside it.
- Fog, for all players (`UpdateFog`): with the far terrain on, Clear and Sunny forecasts keep the 4 times longer fog through dusk and night; other weather keeps the earlier rule. With the imported AC2 sky, the fog end is at least the AC2 palette's own value (1,500 m at dusk instead of AC1's 800 m). With the far terrain off: unchanged.
- Imported AC2 sky: the fog color is the AC2 fog color only (no longer tinted by the sky dome, and no night rain tint); cloud shadows on the land follow the opacity of the drawn cloud layer (`M_ACELandLit_v42`); the AC2 moon is turned and sized to match the AC1 moon.
- Unchanged: the horizon band, the shell's 1 cm drop, the objects radius range (3 to 24), the options window rows, the camera.
- New tests: `ACE.Personal.FarScenery.Fade`, `ACE.Personal.FarTerrain.LODMovement`, `ACE.Personal.FarTerrain.Coverage`.
