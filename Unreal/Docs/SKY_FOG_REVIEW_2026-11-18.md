# Sky, fog and extended-distance review — 18 November 2026

This review compares AC:Unreal v160 with the reconstructed AC1 client, reconstructed AC2 Legions client, the AC2 Unreal implementation, and the locally installed retail DAT records. The requested AC2 Unreal riverside screenshot is a visual reference; it is not a matched camera/time/region capture of AC1. Work from this pass remains local.

## Findings

### 1. Cloud tint and world fog color were incorrectly coupled

**Corrected; high confidence.** Both native clients store world fog independently of sky-object colors. The imported renderer had substituted dome/horizon artwork hues into relatively neutral world fog, and applied an AC1 night-scatter tint to imported storm fog. Neither operation is supported by the native AC2 fog path. They could tint the entire landscape with a color intended for the sky artwork.

The imported renderer now uses the AC2 fog schedule independently of cloud and celestial object tints. It retains the existing AC1 cloud/rain-envelope and storm-brightness adaptation, so an AC1 shower outside the native AC2 episode still follows the local clock; this is not exact native storm scheduling. World fog has opaque endpoint alpha, while the separate sky overlay uses the adapted `fog_sky` opacity. Older caches without rain-day records retain their AC1 weather fallback. The existing night storm cloud readability adjustment remains confined to cloud artwork.

This does **not** mean every native fog color is gray. The installed AC2 clear-day record has pink clouds `(255,209,230)` at phase `.90`, but warm brown-orange world fog `(196,103,56)`. At noon its fog is neutral `(160,160,160)`. Replacing every fog key with gray would also depart from the source.

Primary evidence:

- AC1 `SkyTimeOfDay.cpp:13`, `SkyDesc.cpp:141` (`GetWorldFog`), and `LScape.cpp:905–936`, under `C:/dev/retail_client_source`.
- Legions 7160 `CDayDesc::GetWorldFog`, address `0x0050EE40`; `CDayDesc::GetSky`; `LScapeSky::UpdateTimeOfDay`, address `0x005553E0`.
- AC2 Unreal's source snapshot rechecked at **2026-11-18 08:59 UTC** independently assigns sky-object tint (`C:/dev/ac2e/Unreal/Source/AC2Client/AC2Sky.cpp:454–462`), sky-fog overlay (`:409–417`) and world fog (`:479–481`). Its regional fog is now applied in `AC2Sky.h:78`; this wiring was added during the review.
- Exact local AC2 day records `1A000001`, `1A000002`, `1A000003`: cache CRC checked, complete records decoded, and all three compared byte-for-byte with installed Legions `portal.dat`.

### 2. AC2 Unreal's fog implementation changed during this review

**Confirmed; high confidence.** Native AC1 uses linear range fog, including full 3D camera-to-point distance. AC2's native outside environment also submits linear near/far fog, although the reviewed AC2 excerpts do not establish its distance metric on every renderer path.

The **earlier AC2 Unreal source snapshot** used exponential height fog, clamped negative starts to zero, approximated density from the fog interval, and limited maximum opacity to `0.7`. That approximation could leave scenery more visible; copying its opacity cap would expose the finite terrain shell in AC:Unreal. It is no longer the current implementation.

The **current source, rechecked at 08:59 UTC**, instead composites linear radial fog after opaque lighting and before translucency. It blends in display RGB, preserves negative starts, and reaches full extinction at the authored end. Translucent and additive materials use their own surface distance; additives fade toward black. Regional fog is now loaded and applied before interior/underwater overrides. This is a substantially closer source reference for the remaining color-composition difference discussed in finding 6.

The new AC2 renderer is not a verified drop-in change for ACE: its opaque pass is SM5-only, its material exemptions are binary rather than ACE's fractional `FogAmount`, and its supplied capture tests do not establish mobile ES3.1, stereo or MSAA behavior. Indoor/outdoor portal materials, preview captures and the after-DOF VR panels require their existing exclusions to survive. No passing execution of the new `AC2.Rendering.NativeFog` test was found in AC2's `Saved/Logs` at 08:56 UTC; this review did not run or modify that checkout.

AC:Unreal retains linear 3D range fog and full extinction at the end of the visible world. The sky artwork is excluded from world distance fog. No horizontal-only distance or global gray tint has been introduced.

Primary evidence:

- AC1 `RenderDeviceD3D.cpp:3256,3654–3679,3760–3761`: start/end states, range fog enabled, linear vertex fog mode.
- AC1 `GameSky.cpp:100–153`: ordinary sky drawn with world fog disabled and depth writes disabled; environmental overrides are a separate exception.
- Legions `WorldRender::SetEnvironment` and `SceneTool::SetFogProperties`; corroborating Admin 7148 `RenderDeviceD3D::SetFogProperties`, address `0x005C68E0`.
- Earlier AC2 Unreal snapshot: `AC2Sky.cpp:346–347,479–485`; `Docs/RETAIL_VIDEO_COMPARISON.md:287–291` describes that earlier approximation.
- Current AC2 Unreal snapshot: `AC2Fog.cpp:249–279` (opaque pass), `AC2Fog.ush:9–41` (display-RGB composition and negative-start-preserving range), `AC2Sky.cpp:306–309` / `AC2Sky.h:78` (regional loading/application), and `AC2MaterialTemplates.cpp:703–728` (transparent surface paths). The SM5 guards are `AC2Fog.cpp:46,255`.

### 3. The far-terrain fog limit used requested radius instead of actual coverage

**Corrected; high confidence.** While a new shell is built asynchronously, the displayed shell can still be centered on the previous landblock. A radius measured from the new player/camera center can extend fog beyond that old shell, exposing its edge. The camera boom adds another offset.

Fog now uses the actual applied mesh's outer cell rectangle and world scale, measured from the current camera. It reaches full extinction at least one terrain cell before the nearest edge. Camera altitude does not shorten this geometric coverage bound; the fog shader itself still uses 3D range. The clear-weather distance extension and short storm visibility are retained.

Primary ACE code: `PersonalFarTerrainMesh::CoverageDistanceCm`, `UPersonalFarTerrainSubsystem::ApplyMesh` / `FogCoverageDistanceCm`, and `AACESkyDomeActor::UpdateFog`.

### 4. Far scenery had hard removal thresholds despite configured fade distances

**Confirmed; high confidence.** Instance start/end cull distances were set, but the materials did not consume a fade. Buildings also had a hard cull distance. In addition, clamping the retention ring to the selected visible radius removed hysteresis at the outer edge.

The scenery limit is distinct from fog distance: objects may stop at 8–24 landblocks while terrain reaches 96 landblocks. Pulling world fog inward to hide the object limit would erase distant ridges. The correction must fade object silhouettes locally while retaining the terrain and its lighting.

Five dedicated far-scenery material parents now fade opaque objects, masked foliage, building shells and their room/window fills through the final 20% of their size/radius limits. Original leaf and doorway cutouts are evaluated first, and these materials still write depth and preserve their lighting/fog. Dark window fills retain their special lighting instead of entering the outdoor-lighting update list. Desktop forward MSAA uses sample coverage; non-MSAA uses temporal coverage. Mobile uses binary coverage when its depth prepass cannot represent fractional coverage, keeping depth and color consistent. Ordinary foreground materials retain their existing blend modes.

A hidden preparation ring and a retained ring cover normal one-landblock recentering. Small radius settings retain useful scenery and the near-loading fallback. At the maximum supported radius, the visible fade ends conservatively inside the footprint. Hardware culling waits until the entire object bounds, including an offset mesh origin, have crossed the already-invisible limit.

Radius coverage follows the player's exact streamed position, while size fading and atmospheric fog use the actual camera. A 450m camera boom therefore cannot move the radius boundary outside the prepared footprint. Room and window components explicitly initialize their radius-only fade state. Preparing the extra ring increases residency: the radius-eight fixture starts with 352 blocks instead of the previous 280, and retains more briefly across movement. This is a memory/streaming tradeoff; no unchanged-FPS claim is made.

The ES3.1 preview caught and corrected an additional implementation fault: continuous alpha coverage alone remained fully visible through the fade band under the mobile full-depth prepass. UE's mobile base pass bypasses its alpha-to-coverage state in that configuration, while its depth-only shader clips the fractional mask. The shader now selects binary coverage under `EARLY_Z_PASS_ONLY_MATERIAL_MASKING` on mobile, in both passes. Evidence: UE5.8 `MobileBasePass.cpp:471,913–923`, `DepthOnlyPixelShader.usf:93–96`, `RenderUtils.cpp:621–628,666–675`, and the global shader define in `ShaderCompiler.cpp:3987–3995`.

### 5. Terrain detail changes moved real ridge geometry

**Mitigated; high confidence in the measured geometry.** The old shell switched directly between separately filtered height levels when its center moved by a landblock. At the supplied D599 summit, an independent model using the actual DAT height pyramid measured up to 5.62 pixels of skyline displacement at 1440p/60-degree vertical field of view, within the 9.6km clear-weather fog range. This is projected geometry evidence, not a claim about the exact visibility of every pixel in the user's screenshot.

The builder now blends toward the next coarser triangle across a transition band outside the protected native terrain/scenery area. The same numerical skyline comparison drops to 0.545 pixels. The outer coverage and watertight shared edges are preserved. This increases the radius-96 test mesh from approximately 62k to 96k vertices; the transition is built asynchronously and adds no per-frame vertex update. It reduces, rather than mathematically eliminates, temporal LOD movement.

Primary ACE code: `FFarTerrainShellBuilder::Height`, `PersonalFarTerrainMesh::Build`; tests `ACE.Personal.FarTerrain.Shell` and `ACE.Personal.FarTerrain.LODMovement`. Local independent analysis: `far-lod-review.md`, `lod-recenter-measurements.json`, `measure-lod-recenter.py`.

### 6. Linear-light fog mixing is a remaining retail color difference

AC1 explicitly disables sRGB writes and texture sampling in its fixed-function render state. Its fog interpolation therefore operates on display-valued RGB. AC:Unreal samples sRGB textures into linear light and blends fog in that linear material space. For black toward white, a half-fog blend would encode to roughly `.735`, rather than the legacy display value `.5`.

The focused GPU probe confirmed this without exposure, tone mapping, or scene lighting: the 50% sample captured exactly `.5` linear, encoding to `.735357` display RGB. Maximum display deviation in the black/white probe was `.287099`. The endpoint and monotonicity checks passed. This diagnostic intentionally reports interior differences; it does not claim retail color parity.

The probe explicitly selects equal-precision 16-bit scene-color channels and reads raw HDR values, then restores the caller's scene-color setting. This isolates fog mixing from the unequal channel precision of the 11/11/10-bit buffer selected by some effects-quality settings; it does not change the game's rendering configuration.

This does not establish that all of the photographed haze is caused by gamma. Globally changing texture sRGB would alter every material; changing only unlit terrain would disagree with lit buildings and actors. Exact consistent legacy blending needs fog composition after lighting. This pass does not silently apply a mixed-model or arbitrary brightness workaround.

Primary evidence: AC1 `RenderDeviceD3D.cpp:3722,3756`; ACE `WireRetailDistanceFog`; test `ACE.RetailParity.FogColorProbe`.

### 7. The sky dome's nominal size does not explain the distant cutoff

The existing sky shader expands camera-relative geometry by 1024, with hardware depth testing and no scene-depth discard. The nominal 2.7km classic dome therefore does not intersect an 18.432km terrain shell. An independent triangle-distance check put even the nearest imported face at 80.760km before that expansion. These are existing behaviors, not fixes made in this pass. The identified popping mechanisms were terrain detail/residency transitions and hard object removal.

The imported cloud-motion capture also verifies its UV motion projects along the AC1 wind direction. The comparison retains the existing AC1 celestial direction/visibility mapping and the separate imported color schedule. The source review does not justify rotating the dome or extending its nominal radius again. Supporting evidence is in `sky-depth-review.md` and `ac2-review.md`.

## Deliberate adaptation to AC1's extended landscape

AC2 has geographic region fog maps and daylight/region combine modes, followed by bilinear interpolation at the camera. Those maps do not correspond to AC1 landblocks. The imported sky therefore uses AC1 terrain visibility as its baseline and AC2 authored fog color/artwork. **In imported mode, the far endpoint is now at least the imported palette's own authored endpoint**, before applying the finite-coverage cap. This is an explicit adaptation, not complete regional AC2 parity.

The rendered dusk comparison exposed why that floor is necessary: at phase `.90`, the active AC1 clear record extinguishes geometry at 800m, while the AC2 warm sunset color is authored for 1500m. Applying it at the shorter interval produced an orange wall over the valley. Taking the longer endpoint gives the sunset its intended reach while also keeping AC1 rain from being shortened to the imported 250m storm range. Near distance stays on the AC1 baseline; AC2 supplies it only when both bracketing AC1 far endpoints are nonpositive. The final near distance is still clamped to zero or above, including that fallback. Classic sky with far terrain disabled retains its stock interval.

With far terrain enabled, Clear/Sunny forecasts retain the existing four-times visibility extension through dusk and night. Previously, the extension inferred clear weather from a long instantaneous fog endpoint and collapsed when a clear forecast reached its shorter dusk/night keys. The imported clear `.90` endpoint is now 6000m before the actual-coverage cap, while the checked daytime rain endpoint remains 1500m. This intentionally supports the extended landscape rather than reproducing a native client's short drawing distance. Other forecasts retain their weather-limited envelope. The terrain shell uses the shared land material, fog parameters, ambient lighting and cloud mask. Scenery quality limits remain independent of atmospheric visibility.

Native AC2 also has negative fog starts: clear noon `-50/1500` and daytime storm `-150/250`. Those are real authored values, not corrupt data. The latter implies 37.5% fog at zero range in a linear model. The earlier AC2 Unreal exponential implementation clamped that negative start; the current display-RGB linear implementation preserves it. ACE normally uses the AC1 interval instead. Importing the 250m storm endpoint directly into the AC1 summit would hide terrain below the camera as well as terrain far away.

The live AC1 `13000000` region record has all 232 weather keys' world-fog flags enabled, so the currently ignored enable flag does not explain this reported outdoor case. One Sunny key at phase `.999` has a 4000m far endpoint between much shorter night values; rapid clock tests can expose that authored short visibility expansion. It was not rewritten as part of the rendering corrections.

## Verification and evidence

Local evidence directory: `C:/dev/ACE/TestOutput/FogReview-Nov18`.

- `ac1-review.md`, `ac2-review.md`, `far-review.md`: detailed source review and confidence notes.
- `ac2-unreal-current-fog-snapshot.md`: dated AC2 source changes, full shader/header hashes and ACE compatibility findings. Rechecked **2026-11-18 08:59 UTC**: `AC2Sky.cpp` SHA256 `177696E61C7ACDA7D7A5D54FFCDBE70FC39AD1166115819AC0951371C6AC9CCC` (modified 08:50:54 UTC); `AC2Fog.cpp` SHA256 `2543356AB46C54072F16C00B0C9A113A5C99F1FB954056A47177E371F059452E` (modified 08:46:32 UTC). These identify inspected source, not a verified build.
- `ac2-authored-fog-keys.json`: decoded original values, hashes, CRC and native DAT equality checks.
- `ac1-authored-fog-keys.json`: direct decode of all 20 live AC1 weather/day groups.
- `fog-probe-tests/index.json` and `FogColorProbe.csv`: measured fog-color transfer and diagnostic results.
- `ac2-decompile/`: small native source extracts used by this review; these are local supporting evidence, not redistributed client assets.
- `baseline-tests/index.json` and `baseline-images/`: successful v160 render baseline before this pass.
- `consolidated-final-tests/index.json`: **all 22 suites passed**, with no failures or unrun tests. Three suites report fixture cleanup, a deliberately invalid Setup ID, or lower-priority quality settings being ignored. This reran the full summit/sky, weather, terrain, scenery, room/window and material-packaging comparison after the fixes.
- `final-images/`: fixed summit captures for classic/imported skies, clear/rain and day/night/dawn/dusk. The clear noon capture retains neutral distant haze and colored clouds/belt. Dusk still has the native palette's warm haze, with visible valley terrain through the corrected 6000m interval.
- `final-probes-tests/index.json`: the extended classic rain/night occlusion probe passed at 1, 5, 10, 18.432 and 20km across four headings. This does not assert the same exhaustive extreme-distance coverage for imported sky.
- `coverage-final-tests/index.json`: room/window rendering and packaged-material checks passed after explicit radius-only primitive initialization.
- `paired-final-deferred-tests/index.json`, `paired-final-forward-tests/index.json`, `paired-final-mobile-tests/index.json`: final far-scenery coverage and all 46 runtime material parents passed in desktop deferred, desktop forward/MSAA4 and ES3.1 mobile LDR/MSAA4 previews. Mobile originally failed partial coverage; the depth-prepass correction in finding 4 made it pass.
- `build-paired-final.log`: successful final local Development Editor build, including the final quantitative GPU probe. `git diff --check` also passed. Existing runtime material assets were preserved; this pass adds five far-scenery parents and their parameter collection.

The final fade probe renders the original and far materials on mirrored sections of the same surface in one capture, reads raw HDR values, and checks their actual material inputs. Full visibility and restored visibility match the ordinary reference at 1.0 in all three renderers; invisible endpoints read zero. Partial size/radius coverage measures approximately `.459/.479` in deferred, `.333/.333` in forward MSAA, and `.461/.482` in mobile. All three 450m orbit checks pass. Earlier sequential captures showed the ordinary reference itself changing between captures; they are not evidence of a stable far-material shading defect, and raw HDR readback alone did not fix that fixture issue. The same-frame comparison retains the original tolerances.

Forward MSAA resolves sample coverage; the mobile full-depth-prepass fallback uses visible binary noise inside the narrow fade band. Its endpoint correctness is verified here, but headset motion and fine-foliage noise need hardware visual acceptance. These PC previews are not headset tests.

The integrated summit run measured 32,747 distant-terrain pixels darkened by cloud shadow, 33 of 230,400 pixels changing by at least six display levels when the terrain shell recentered under a fixed camera, and no lower-view difference from adding fully fogged scenery. Extending scenery from radius 8 to 24 changed 11,636 lower-view pixels. Cloud travel agreed with the AC1 wind direction (dot product 1.0), and 64x sky time changed 17,356 pixels while engine time stayed fixed. These are controlled regression measurements, not native-client pixel equality or a performance benchmark.

The comparison suite uses the supplied AC1 summit `0xD5990036 [144.610855 120.631470 374.005005]`, the supplied facing, real AC1 terrain data, and local Legions sky data. It covers classic/imported clear and rain, day/night/dawn/dusk, far terrain off/on, sky/cloud visibility, shared cloud shadows and fully fogged terrain meeting the horizon. Separate coverage checks exercise camera movement against a retained old shell.

No fresh native-client storm capture or headset hardware comparison is claimed. Existing user references and reconstructed source establish the target behavior; the local preview remains the place for the user's final visual acceptance.
