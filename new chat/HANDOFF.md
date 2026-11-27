# Handoff — procedural cloud renderer

## Scope
Only the SA 1.0 US volumetric cloud effect has been replaced. Native low clouds,
sky polygons and moving fog are deliberately untouched. This remains textured
impostor rendering and is not a physically simulated volume.

## Reference and decisions
- Plugin-SDK: 624a6a49265fd7a6fc63bda1611013ceabeacb8a (2025-10-25).
- gta-reversed: 85bce0be4bd3229eb5ef445d3a7ac2e611a994f4,
  `source/game_sa/Clouds.cpp`, `source/app/app_game.cpp`, `source/game_sa/Weather.cpp`.
- Supplied SA IDB function index corroborates the native function entries
  (image-base/section-relative names). The split IDB archive consists of IDA
  disassembly text; part 06 verifies the instruction at 0x53E1B4
  (bytes E8 C7 81 1D 00), directly calling 0x716380, after MovingFogRender.
  Part 14 confirms CClouds::Init at 0x7138D0 reads cloudhigh/cloudhighm into
  the texture pointer at 0xC6BFC8, then initializes both native cloud and fog
  tables. The stock distance getter at 0x713630 uses Scene's camera far plane
  and a constant cap. These were verified against the actual dump without IDA.
- Renderer call at 0x53E1B4 is checked for E8 rel32 to 0x716380 before patching.
  Plugin-SDK and reversed agree on the native function and texture structure.
  The disassembly also confirms the native FX quality medium multiplier is
  approximately 0.66, rather than the 1/3 in the current reversed source.
  Our configurable density does not depend on this native global.
- Native renderer constructs three orthogonal textured quads, weights alpha by
  the absolute view dot and uses a relatively small distance limit.
- Previous mod used max of old/new bad-weather weights instead of interpolating,
  never retired clouds when target declined, spawned only at distant rings,
  repeatedly pushed apart every pair, reset native cloud tables and used global
  game RNG. It treated timecycle/LOD values as if they could extend the camera
  clip plane and submitted each cloud independently.

## Files
- CloudCore.hpp: portable validated configuration, deterministic simulation and card geometry.
- main.cpp: game integration, texture dictionary lookup, RenderWare pass/state guard,
  once-per-frame update, culling and ordered batched submissions.
- tests/cloud_tests.cpp: executable regression tests (not disabled by NDEBUG).
- CMakeLists.txt / .github/workflows/build.yml: portable checks and Win32 SDK build.

## Validation and next in-game checks
Portable tests cover density increase/decrease, weather transitions, INI input,
spawn spread, wind/timestep, stationary/yaw behavior, clip distance, teleports,
geometry orthogonality, opacity normalization and capacity. No GTA game executable
or assets are available in the Linux workspace. The Windows CI artifact is the
next executable test target, not proof of in-game rendering.

1. Verify log reports hook installed; test unchanged low clouds and fog.
2. Test clear LA to cloudy SF to rainy countryside, then back to clear. Check density
   and fade, native timecycle tint, and sky at sunrise/sunset.
3. Fly through and above the configured absolute altitude band. Look in all yaw
   directions, check rising/descending, cockpit intersections, and long-distance
   cloud recycling without abrupt pops.
4. Test existing INI, missing additional texture names, and modded particle.txd.
5. Test interior return, new game/load, alt-tab, and SkyGfx (including deferred).
   The renderer uses RW state and does not allocate default-pool D3D resources,
   but compatibility and appearance must be tested with real game assets.
6. Compare RenderMode=0 and 1; profile with one texture and several textures.
   Multiple textures may increase draw calls since alpha order must be kept.
   No measured in-game frame-time improvement is claimed.

The offline renderer in `tests/dump_preview.cpp` + `tests/preview.py` projects
the portable model's real cards with a synthetic alpha texture and sky gradient.
Examples in `docs/previews` are diagnostics, not GTA screenshots.

## Lighting correction after user runtime feedback
The original white high clouds and first PR build's dark, brown/gray clouds were
compared in user-supplied GTA screenshots. They are different geographic and camera
positions/times, so not a controlled pixel comparison. Nevertheless the code had
a clear cause: it used unrelated timecycle low-cloud/fluffy-bottom RGB at 100%
strength and darkened it twice (per puff and bottom vertex), plus sky-colour haze.

The IDA text in part 14, 0x716693–0x7166EB, loads six unsigned sky channels,
sums them, divides by six (0x2AAAAAAB multiply), adds 0x40 and clamps to 0xFF.
It uses a neutral value, not the low-cloud palette. The stock blend is SRCALPHA /
INVSRCALPHA with scene fog disabled (gta-reversed PostEffects and native cloud
override). The new default LightingMode=0 reproduces this neutral vertex
colour (64 boost by default), bypassing old tint, shade and haze keys even if
set by an old INI. LightingMode=1 retains custom lighting, with safer fresh
defaults. No changes to opacity, geometry, spawning, or texture selection.

Portable tests exercise old INIs, clear/dawn/night luminance, colour saturation,
uniform top/bottom colours and explicit custom mode. The offline preview now
uses the shared C++ lighting instead of independently approximating it; the
two sunrise scenes illustrate the lighting difference using identical geometry
and synthetic texture/palette. Test the new ASI in the same place, time/weather,
and particle.txd to validate visually.

## Optional shader-free cumulus mode
User requested a cheap approximation of modern dense cumulus clouds that
would run on old hardware. Keep the existing PR #1 and native defaults;
RenderMode=2 opts into the new mode rather than installing a second competing ASI.

Reference: Mark Harris/Anselmo Lastra, Real-Time Cloud Rendering (2001) and
SkyWorks (https://markmark.net/clouds/), plus SimGear's cloudfield.cxx,
cloudfield.hxx and cloud.cxx (FlightGear/simgear, branch next). Reviewed
world-space cloud fields, wind, impostor/LOD organization and CPU-side
particle illumination concepts. All new code and procedural pixels are original;
no GPL source or assets reused.

- Cumulus.hpp: 4 padded, deterministic 128x128 grayscale RGBA cloud textures
  from 22 sub-lobes/variant, soft noisy optical thickness, straight-alpha
  composition. One 256x256 atlas (256 KiB RGBA). 3D puff centers arranged as
  broad bodies and raised towers, camera-facing world-up basis for roll.
- Lighting uses CTimeCycle::m_VectorToSun[m_CurrentStoredValue] from the
  pinned SDK. It uses original neutral sky brightness, diffuse contrast
  and broad backscatter rim approximated on 3x3 or 5x5 CPU-lit lattices.
  It has no dependency on the old custom tint settings or particle TXD.
- Mode 2 uses a different optical-density mapping, with the same native
  field, near/distance/weather fades and solid world geometry occlusion.
  Modes 0 and 1 are unchanged (including old INI defaults).
- main.cpp lazily creates one ordinary RW texture raster, uploads from a
  32-bit RwImage, uses one indexed batch sampler, and releases before RW
  shutdown. Creation failure logs and falls back to clustered mode for the
  remainder of the RW lifetime. No programmable shaders or offscreen targets.
  A 5x5 mesh is 25 vertices and 96 indices/card; 3x3 is 9 and 24.
  Vertex/index batches capped at 480/1920 respectively.
- Tests: cumulus_tests.cpp validates deterministic atlas and padding, coverage,
  UV inset, native-mode compatibility, INI limits, physically intuitive
  sun/backlight ordering, night behavior, bounds and index safety, full
  512-cloud geometry at both details and mode-specific opacity.
  CMake runs both portable regression suites on Linux/Windows.
- Offline software renderer: tests/cumulus_preview.cpp emits the real mesh,
  atlas and lit vertices; cumulus_preview.py rasterizes perspective-correct
  triangles and composites with straight alpha against a synthetic sky.
  It renders front/back light, above cloud, ground, economy mesh and night.
  The software sky glare is NOT a new game effect.
- Actual SA runtime appearance, RW raster creation, alt-tab/resource lifetime,
  compatibility with SkyGfx, sorting near aircraft and GPU fill-rate remain to
  be verified. Billboards are not full volumes; distant cloud detail may alias
  and close intersections may expose planes. Do not claim measured game FPS.
  For a potato setup use CumulusDetail=0, PuffsPerCloud=3; tweak density only if
  fill-rate remains a problem. Preserve the vanilla-like default mode.

## Cumulus follow-up: nighttime ghosting and camera rotation
User's runtime screenshots at 23:42 and 12:36 demonstrated transparent
overlapping puff silhouettes, flattened-looking overhead clouds and
camera-rotation-coupled shapes. The initial cumulus implementation was
using the camera's forward/up basis for every puff and applying native +64
brightness to dense clouds even after the direct sunlight term had vanished.
The screenshots are evidence of the visual symptoms, not a controlled test
with the same camera/weather. Protect modes 0 and 1 and keep PR #1.

- Mode 2 now defaults to CumulusImpostors=1. A 512x512 RGBA atlas has four
  256x256 whole cloud archetypes. Bake 36 noisy rounded lobes into an
  integrated optical-density field, alpha=1-exp(-tau). Smooth weighted
  visible surface gradients and lobe-normal illumination provide neutral
  relief. No runtime raymarching or shaders, only a one-time CPU projection.
- Normal outside representation is a single textured 5x5 or 3x3 grid
  (32 or 8 triangles); project ellipsoid X/Y/Z support radii in the plane
  so overhead views use horizontal cloud extent. Single alpha value avoids
  layer-on-layer pseudo-brightening. Density remapping makes exterior bodies
  nearly opaque but global Opacity, distance, spawn and near fades still apply.
- Near a volume, split optical depth between the body and at most
  PuffsPerCloud-1 near layers, preserving the draw-buffer limit. A ray-specific
  ellipsoid intersection radius prevents wide, shallow clouds overhead from being
  incorrectly treated as near/inside. The blend is an approximation due to
  nonidentical silhouettes; world-space puffs have per-puff proximity fade.
  With PuffsPerCloud=1, use one body card at all distances.
- CumulusImpostors=0 retains the layered representation and original lobe
  atlas algorithm; both paths get updated lighting and world-locked facing.
  The facing basis is determined from the world eye-to-center vector using
  shortest-arc transport of a fixed reference (+Y) frame. Its antipodal
  horizon singularity falls back to world up; no camera yaw/pitch/roll
  or camera forward/right/up axes enter the calculation at any view angle.
  This is not view-independent 3D geometry: moving around an impostor
  may still expose texture/view changes. Further progress could use
  multi-view baked volume projections, with careful blending and LOD.
- Direct sunlight still uses CTimeCycle::m_VectorToSun. Reversed
  TimeCycle.cpp 0x560A67 computes sun direction using sin/cos(game minutes).
  Night blends toward subdued ambient from the sky palette, with small
  diffuse floor rather than vanilla +64 at night. The reversed Clouds.cpp
  lunar sprite offset is (0,-100,15); a weak diffuse moon term is gated by
  an approximate midnight window from CClock and weather rain/fog; not an
  exact phase/visibility reconstruction. CumulusNightLight=1 adjustable.
- Atlas size is now 1 MiB of RGBA, not the initial 256 KiB. Outside, fewer
  meshes and less overlap than five cards; actual GPU overhead is unmeasured.
- Regression tests verify atlas density and padding, night and lunar response,
  optical-depth split, and identical world-space card positions and basis after
  camera orientation changes at the same eye position (including close layers
  and zenith), plus projected overhead extent, finite axes and existing limits.
- Comparison scenes 'zenith'/'night' are from identical synthetic field, camera,
  and palette as 'zenith_previous'/'night_previous' generated from the previous
  commit 94fbeaa15b9d0adf76ab18a43fc9e9d9592a3f3a. The 'layers' scenes use the
  newly corrected layered option; all are software renders, not GTA captures.
- Re-test actual game by rotating camera while stationary, then walking and
  flying underneath/through clouds, checking sunset, night, moon direction,
  circling clouds, and performance on weak GPUs. No actual game run in Linux.

## Cumulus follow-up: altitude rotation and SA art direction
The user's 07:04 screenshot (street level, airbreak) revealed that even the
world-eye-facing impostor from the prior iteration rotated as they climbed.
The dense cream silhouettes and silver fringe also felt out of place in SA.
Target is still old PCs; keep PR #1, modes 0/1, native lighting, the pinned
Plugin-SDK and the original call site unaffected.

- Mode 2 no longer has any facing basis, whole-card impostor, or near-layer
  LOD. It uses three world-fixed orthogonal planes (XZ, YZ, XY in a cloud-local
  stable yaw frame) based on the cloud's actual dimensions. Position,
  orientation, UVs and vertex colours are invariant to camera yaw, roll,
  translation and elevation. Wind still shifts centers in world space.
  The squared normalized eye direction cosines only split alpha via
  1-(1-alpha)^weight. At the exact center, use equal weights; common near fade
  hides the center if enabled. This is optical-depth normalization for full
  masks, not exact volume compositing; intersections remain possible.
- Twelve 128x128 grayscale-alpha projections (four deterministic common
  3D lobe fields, each projected onto the three planes) share the existing
  512x512 RGBA atlas. Softer density taper and procedural erosion, more
  neutral texture, lower mode-2 alpha remap (.48-.80 times CumulusDensity).
  Keep stock daytime grey calculation (+64 by default), with very restrained
  world-fixed directional and height gradient and no additive bright rim.
  Night stays subdued; no camera-aligned lighting or bloom.
- Detail 0 now uses 2x2 grids, up to 12 vertices / 6 triangles per group,
  and is the default for fresh configurations. Detail 1 uses 3x3 grids,
  up to 27 vertices / 24 triangles per group; existing config choices respected.
  Same RGBA atlas size, one fixed-function texture/blend path, bounded
  batches, no shaders or extra passes/targets. Oblique viewpoints can
  require handling up to three planes versus the prior one: triangle cost
  is lower but transparent fill-rate must still be tested on actual hardware.
  CumulusImpostors is legacy/no-op; PuffsPerCloud applies only to mode 1.
  Atlas failure falls back to mode 0, not rotating mode 1.
- Linux regression suite checks projections, UV isolation, full-mask alpha
  weights, repeated deterministic atlas, stable world vertices/UVs/light
  across 175 observer positions (including center, below, above, side), legacy
  INI compatibility, portable mesh capacity, geometry cost and night response.
  Software renders use actual C++ atlas and mesh, synthetic sky only. Height
  sequence at Z=30/210/390 uses the same initialized field and view axes.
  Historical *_previous and *_layers previews are prior versions.
- Test in SA: ascend/descend at fixed X/Y and rotate/roll camera, compare
  sunrise/noon/night, fly around and through the layer, profile at detail 0
  on a weak card before changing count or draw distance. Some crossed-plane
  silhouettes and overdraw remain inherent trade-offs. No actual game capture
  or measured GPU FPS from the Linux environment.

## Follow-up: all modes fixed; period-style density slicing

User reports mode 1 (and suspected 0) still rotates with camera; mode 2
orientation is fine but visual appearance needs convincing 2006–2008-era
clouds for weak GPUs. Same PR #1, pinned SDK, SA 1.0 US call site and native
lighting equation. This section supersedes the previous mode-2 representation.

- Mode 0 was already world-axis crossed cards. Mode 1 previously used
  v.right/v.up, so fixed the actual cause: deterministic fixed crossed core
  plus one tilted world-space card for each of the PuffsPerCloud-1 satellites.
  Core guarantees nonzero facing weight in all directions, including with
  PuffsPerCloud=1. Weights respond only to observer position; normalized
  full-mask transmittance preserves the previous 2x optical opacity rule.
  Modes now each choose their geometry automatically; no new INI switch.
- Separate MaxCards=12 from MaxPuffs=8 to cover both modes 1/2. Draw-list
  and all diagnostic/test arrays sized accordingly. Mode 1 uses puffs+2
  candidates, mode 0 three, mode 2 twelve. Discard negligible alpha before
  per-card bounds/frustum work. Native/custom lighting and texture lookup
  still unchanged in modes 0/1.
- For mode 2, period reference: SIGGRAPH 2004 Course 28 Real-Time Volume
  Graphics §3.2, object-aligned 2D texture slices; source
  https://faculty.cc.gatech.edu/~jarek/lectures/V00Intro/handouts/Course28.pdf
  and the cloud-rendering paper/site by Microsoft Flight Simulator 2004
  developer Niniane Wang https://niniane.org/clouds/. These are conceptual
  references; no copyrighted code/assets from other systems are used.
- CPU-only bake builds four deterministic 64³ or 128³ density volumes with
  overlapping ellipsoidal lobes, noisy erosion, an irregular broad base
  and billows. Integrate optical depth across three thick slabs per axis
  and a matching whole projection; Beer-Lambert alpha, density-weighted
  grayscale lighting from vertical sky occlusion, local density gradient,
  projected thickness relief and slight procedural detail. 48 atlas tiles
  in an 8x8 grid. Economy 512² RGBA / 1 MiB, detail 1 1024² / 4 MiB (slower
  one-time bake; optional only). High allocation failure retries 512²,
  both failures fallback to native crossed mode. Conservative UV inset
  supports either atlas without remapping. High CPU allocation failure
  caught; temporary volume buffers do not persist.
- Three stacks of fixed planes (XZ/YZ/XY), each one whole projection at
  center plus three depth-separated slabs. No camera-derived rotation.
  Select dominant projected area (abs dot / perpendicular extent) with
  smooth normalized weights; suppress near-edge-on axes. Distance LOD
  depends on angular size: economy .16-.32 normalized radius/distance
  blend, quality .10-.23. Both representations exist at fixed positions;
  only alpha is blended. Calibrate stack and LOD alpha against reference
  integrated opacity .95 and its equivalent three slabs. Exact for this
  reference mask; approximate for general rays. Slightly compress slab
  spacing to hide gaps. Mode-2-specific horizontal aspect softening and
  thicker vertical extent prevent needle shapes. Density remap .68-.94
  times CumulusDensity. No additive bright rim or strong timecycle tint.
  Vertex lighting fixed in world axes and includes slab offset.
- Economy two triangles/active card; typical distant cloud one card, near
  three. 12 candidates maximum during simultaneous crossfades, 24 triangles
  economy / 96 quality worst case. Synthetic 512-cloud diagnostic: 912
  economy, 1443 detail active cards before distance/opacity/frustum culls.
  No frame-time or hardware claims; GPU overdraw still depends on views.
- Tests cover all modes across 175 positions and rotated eye axes, mode 2 at
  both details (350 positions); verify alpha unchanged for pure camera
  rotations, geometry/UV/vertex colors stable with translations, projection
  and slab alpha consistency, both atlas resolutions, edge padding,
  bounded buffers, finite colors, reference optical LOD and triangle budgets.
  Software previews include ground, zenith, fixed field altitude sequence,
  close-up, sunrise and night. These use synthetic sky/texture for legacy
  modes only; cumulus uses actual procedural atlas. Actual GTA appearance,
  alpha sorting at extreme oblique/close angles and weak GPU fill rate
  require user testing. Keep detail 0 recommended for potatoes.
- Windows build follow-up: `near` is a legacy Win32 macro. Rename the LOD
  variable to sliceWeight and compile portable cumulus tests with empty
  near/far definitions so this SDK-specific macro collision is caught early.

## Follow-up: replace mode 2 with single-pass procedural cloud canopy (default)

Supersedes all previous mode-2 cumulus implementations. User found prior
world-fixed slice stacks visually poor, wants GTA V-inspired clouds made
with old-console techniques and very low cost. Same PR #1, same SDK pin
2025-10-25, same hook guard. Explicit existing RenderMode=0/1 respected.
Fresh INIs (and INIs without RenderMode) default to 2. No external assets.

- References reviewed directly: Quake III Arena code/renderer/tr_sky.c
  (R_BuildCloudData / cloud-layer intersection), OpenMW
  apps/openmw/mwrender/sky.cpp (UV scroll and blending weather cloud meshes),
  FlightGear SimGear simgear/scene/sky/cloud.cxx (altitude, coverage, transition),
  and Adrian Courreges GTA V frame analysis, section Sky (dome + cloud ring;
  normal/density textures). URLs in README. These are design references;
  no engine code, game textures or copyrighted assets copied.
- Canopy.hpp replaces Cumulus.hpp. Seamless original domain-warped periodic
  value and cellular noise, multiple scales for broad cloud banks and
  billowed edges. Coverage threshold follows target(s, weather); actual
  seed 17321 preview coverage ~41% clear and ~86% rainy for alpha >96
  (texel fraction, not screen coverage). CPU shading uses near/far
  directional density taps, fine relief, edge softness, native sun vector,
  mostly neutral daytime light and subdued night from actual sky palette.
  No additive white rim, no low-cloud oversaturation. Legacy Cumulus*
  settings mapped to related canopy parameters.
- One ordinary managed RW tileable RGBA texture, 512² default / 1024² opt-in,
  1/4 MiB raw plus mipmaps when supported. Format selected after pixels
  filled. Mip flags tried first, then plain texture, detail failure retries
  economy. Keep the CPU scalar map, pixels and one RW image for updates.
  Static noise generated once per RW lifetime; refresh samples a fixed light
  snapshot and shades 16 rows/frame after at least 50 native timestep units
  (~1 second) and meaningful light/coverage change. Upload the complete
  texture only. Paused simulation freezes wind and does not start new jobs;
  an already started CPU bake may finish. Upload failure releases resources
  and falls back to mode 0 for the RW lifetime. No custom GPU shader/targets.
- 257 vertices / 480 triangles in one indexed submission (1440 indices).
  Fixed, shallow curved disk, world XY texture coordinates and absolute
  altitude; circular extent follows camera XY but UVs are world anchored,
  so horizontal travel can see slight height deformation at distant edge,
  never yaw/roll/altitude-induced texture rotation. Local near fade plus
  distance/outer-ring fade. Draw two-sided with depth test, no depth write;
  all touched RW states restored. Weather and clip distance stay native.
- CanopyFlightClouds=0 default, opt-in 1..24 native crossed groups near
  center altitude only, <=900 camera range and .65 opacity multiplier.
  This is not a connected volumetric representation, and is deliberately
  off by default for reduced overdraw. Normal mode-2 path never updates
  or sorts the legacy field. Existing modes 0/1 remain as before.
- Removed obsolete cumulus header/test/preview source; old JPGs intentionally
  kept as history. New canopy_tests and canopy_preview sources. Portable
  tests built with legacy Win32 near/far macros, Werror. Tests exercise
  seamless wrap, deterministic generation, weather coverage, brightness,
  opposite sun direction, rate-limited completion, camera orientation/height
  invariance, UV anchoring, fade, wind and previous core regression tests.
  Preview software rasterizer clips triangles against near plane, performs
  perspective-correct bilinear wrapping on actual C++ texture, renders
  day/zenith/dawn/night/rain/above/inside. Synthetic sky; no GTA game or
  GPU render pipeline, no native wisps or mipmaps simulated. Above-layer
  angles can still reveal the flat sheet. This is period-style sky-layer
  rendering, not actual modern volumetric cumulus. No real FPS claims.
- Next in-game setup: set RenderMode=2 in existing INI and detail 0, compare
  LA clear dawn/noon/midnight and storm, yaw/roll, vertical ascent through
  layer, aircraft, clip changes, low draw distance, alt-tab, SkyGfx.
  Check log for successful texture initialization, compare visual and
  performance with modes 0/1 and optional detail 1. Profile on actual weak
  GPU before claiming measured minimum hardware.

## Follow-up: original 3D density-baked hybrid (mode 2), 2026-11-27

User supplied a GTA V executable and `New folder (2).zip` containing
cloud-related resources as reference, explicitly wanted original clouds
rather than an asset conversion, and asked to continue improving PR #1.
Prior mode 2 was a flat canopy. Keep the same SA 1.0 US hook, exact SDK
pin 624a6a49265fd7a6fc63bda1611013ceabeacb8a, native weather/camera
constraints, and modes 0/1. Mode 2 with HybridClouds=0 is the previous
canopy-only fallback. No proprietary assets or extracted code included.

### Reference investigation

- Supplied GTA V executable metadata: x64 version 1.0.3889.0; RSDS debug
  directory references game_win64_master.pdb (GUID
  b41a0c8a-a6aa-4ff0-b955-f47cb30f481c, age 1), not the PDB itself.
  Strings and the XML identify sky and cloud-hat systems; this does not
  reveal a complete renderer or shader implementation.
- Archive: 83 files; clouds.xml describes 20 named presets including
  Puffs, Wispy, Cloudy, Nimbus, cirrus and storm/rain. Layers contain
  altitude triggers/fade ranges, cost factors, UV animation and transition
  settings. Puffs has three principal drawables, a horizon ring and two
  altitude layers. cloudkeyframes.xml has 14 clock times and controls for
  ambient, bounce, sun, density, normal strength, thickness and scattering.
- RSC7 headers and zlib payloads decoded in memory; inspected only geometry
  metadata using offsets checked against CodeWalker Drawable.cs
  (DrawableBase, DrawableModelsBlock, DrawableModel, DrawableGeometry).
  Resource system pointer base 0x50000000; graphics base 0x60000000.
  Model list pointer at drawable +0x50, list count at +8,
  model geometry list ptr +8/count +16; geometry index/triangle counts
  +0x58/+0x5c, vertex count +0x60, vertex stride +0x70.
  Verified original vertex/triangle counts:

  | Reference | Vertices | Triangles |
  |---|---:|---:|
  | cloudhat_cloudy_a | 67 | 53 |
  | cloudhat_puff_a | 437 | 704 |
  | cloudhat_puff_b | 337 | 467 |
  | cloudhat_puff_c | 325 | 468 |
  | cloudhat_wispy_a | 1053 | 1920 |
  | cloudhat_stormy01_a (2 geometries total) | 328 | 480 |
  | horizonring | 455 | 768 |

  All inspected geometries have 52-byte vertex stride. Puffs A/B/C
  bounding heights: approx 0–1.2, 0.9–2.3, 1.4–2.4 resource units, with
  much wider horizontal extent. This demonstrates stacked mesh layers,
  not that the full GTA V volumetric/cloud shading algorithm is known.
  No vertex buffers, pictures, model geometry or shader bytes were
  exported or committed. This motivates multi-layer geometry, distinct
  far canopy and near forms, and time-dependent lighting.
- Reviewed primary GitHub references: CodeWalker Drawable.cs, Texture.cs,
  ResourceFile.cs, YdrFile.cs; prior Quake III, OpenMW, SimGear sources.
  Additional research context: Harris/Lastra real-time cloud impostors,
  directional precomputed scattering as a period-appropriate technique.
  Original implementation rather than copied code.

### Implementation

- CloudVolumes.hpp is portable, deterministic, RW-independent. Three
  64³ generated fields with 25 smoothly joined eroded lobes each, radial
  guard fade to prevent rectangular clipping, six cardinal extinction
  coefficient fields. 26 world-space capture directions per form:
  8 azimuths × 3 elevations (−60, 0, +60) plus poles. True CPU optical
  integration of coherent density and light basis, depth weighted by
  front-to-back alpha. 78 captures total.
- Economy 96-pixel slots in 1024² RGBA raster, detail 128 in 2048×1024,
  transparent 2-pixel gutters and empty atlas slots. No mipmap bleeding.
  Atlas output has no game assets. Coefficients store 8 bytes/texel
  (alpha, 6 directional response channels, signed depth), separately
  shaded to ordinary RGBA. Dynamic sun response samples cardinal bases
  rather than tracing again; approximation, not exact light ray solution.
  Night follows native sky palette and does not retain bright sunlight
  floor. Day ambient bounce and edges restrained, no additive halo.
- 5×5 world-fixed grids (32 tris) per capture; depth proxy uses locally
  alpha-weighted average integrated depth. Angular triangle interpolation
  blends up to 3 neighboring fixed capture meshes, suppresses glancing
  views; weights depend on eye-to-center position, not camera orientation.
  Fixed order within groups; alpha optical split approximate. Compare
  same-cloud orbit montage; still not genuine 3D correspondence at
  arbitrary close angles. Fade into interior; future local 3D density
  haze is a possible separate iteration.
- Stateless jittered world cells at 440m default, actual world displacement
  shared with canopy wrapped UV drift. Absolute altitude and random
  archetype/size fixed per cell. Weather threshold from macro canopy
  density, soft radial/range/inside fades. Radius min(1480*SizeScale,
  .84*range.end). Maximum 48 groups; test position 25 clear/32 rainy.
  Geometry worst-case 5088 triangles incl. canopy, before frustum cull;
  480-vertex/1920-index bounded batches using standard RW Im3D and saved
  render states. Atlas and canopy use separate ordinary rasters. No
  shader, per-frame raymarch, new DLL dependencies, GPU render target,
  GPU 3D texture or screen readback.
- Atlas generated in bounded work units, soft 2ms per frame (not a hard
  deadline for single unit, allocations or raster uploads). Canopy remains
  visible while building; initial shading 32 rows/frame, atlas hybrid
  fade-in over approximately two simulated seconds. Relight 16 rows/frame
  after 75 native timestep units and meaningful light change. Previous
  texture remains visible until all rows complete. RW lifetime owns
  atlas/raster/staging; failure retains canopy and logs, no repeated retries.
  Canopy-only failure chain unchanged. All legacy setting keys remain
  recognized, new HybridClouds=1 defaults on. CumulusDetail changes both
  texture resolutions, geometry count stays fixed.
- Tests: volume_tests.cpp (portable near/far macro coverage) registered in
  CMake, verifies weight sum and continuity across sphere including poles,
  exact capture directions, deterministic atlas, alpha gutters, sufficient
  shape coverage, day/night and opposite sun, incremental refresh, fixed
  positions/geometry under camera axes and translation, shared wind and
  fallback. Existing core + canopy tests retained. Linux synthetic-sky
  preview uses actual C++ atlas and mesh with perspective-correct sampling,
  near clipping; no GTA captures, no GPU performance measurement.
- In-game verification still needed: low draw distance, sunsets, rainy
  weather, night contrast, yaw/roll at fixed position, ascent/orbit/flight
  through cloud, crossfades at different FOV, RW shutdown/restart and
  device reset. The sheet and nearby meshes remain separate alpha passes
  so their intersections and close-view ghosting may be visible. Render
  mode 2 is a pseudo-volumetric approximation, not full GTA V cloud
  scattering. Compare HybridClouds=0 if GPU fill-rate limited; use
  CumulusDetail=0 by default. Do not claim measured minimum hardware.
