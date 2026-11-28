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


## Native weather synchronization / local flight haze (2026-11-27)

Supersedes hybrid lighting and flight limitations described above. User
reported weather lag; keep RenderMode=2, HybridClouds=1 and CumulusDetail=0
as defaults, preserve explicit modes 0/1 and existing INI.

- Source audit: gta-reversed 85bce0be4bd3229eb5ef445d3a7ac2e611a994f4
  `source/game_sa/Weather.cpp` CWeather::Update interpolates coverage,
  foggyness, extrasunnyness from old/new weather; Rain and Sandstorm are
  separately stepped by timestep*.005. ForceWeatherNow changes both IDs.
  `TimeCycle.cpp` interpolates timecycle colors across clock/weather;
  `source/app/app_game.cpp` uses LightningFlash for scene lighting.
  Pinned SDK 624a6a49265fd7a6fc63bda1611013ceabeacb8a provides CWeather
  refs and RwIm2D vertex functions. No native weather globals written.
- CloudWeather.hpp samples live weather on every draw. Rainy IDs 8/16,
  fog ID 9, sandstorm ID 19 (SDK enum checks). Rain/dust formation intent
  follows old/new interpolation, not residual particle intensity.
  Native Rain only supplements storm intent under bScriptsForceRain
  (native timecycle fog override flag). Fog/dust visibility suppression
  and extra-sunny alpha are original artistic approximations.
- Canopy.hpp static correlated grayscale base and added-coverage masks.
  Added alpha = (hi-lo)/(1-lo), exact endpoints at full geometry fade;
  interpolated mask alpha/geometry fade approximate. Lower-than-one
  BadWeatherDensityMultiplier reverses mask weights. Two managed rasters
  (non-mip fallback), one draw in clear and two at most; static density,
  relief and alpha, current-frame RGB/alpha. CPU map/images freed after
  upload. Empty masks skipped. No weather-driven upload or threshold lag.
- Hybrid atlas shading decouples relative directional coefficients
  from weather, timecycle luminance and lightning. Current-frame RGB
  via Im3D vertices, neutral brighter fair weather, dimmer storm/night.
  Solar direction refresh only, 32 rows/frame after 30 native time units,
  pending large direction jumps superseded. First upload can still take
  time; not a guaranteed instantaneous solar direction change.
- CloudVolumes.hpp: sharper multi-scale erosion, different broad,
  tower and bank silhouettes, fixed form per world cell with continuously
  weighted opacity by weather. Near renderWeights concentrates normalized
  blends continuously to reduce ghosting. Same fixed capture geometry
  and per-group triangle cap. Three 24^3 byte density grids retained
  during bake (41,472 bytes). Groups separately store occupancy and
  exterior alpha; retiring external proxy on entry no longer destroys
  density for haze. InteriorHaze=.85 default, clamped 0–1, 0 disables.
- Interior alpha integrates six local density samples along each of up
  to nine screen rays per nearby eligible group. Max local path 90m;
  screen space 3x3 vertices / 8 triangles, textured state cleared,
  Z/fog off, states restored by RAII. Existing scene/HUD rendering order
  preserved, no native fog or other post-effects hooks added. This is a
  depth-unaware fog approximation, not raymarched per-pixel clouds:
  nearby geometry may also be tinted. Canopy-only has no density haze.
- Budget: 48 groups * 3 views * 32 tris + 2*480 canopy + 8 haze =
  5576 max conservative pre-cull triangles (optional native wisps excluded).
  4 MiB atlas + ~2.67 MiB canopy with mipmaps; high detail 8 + 10.67 MiB.
  No new GPU shader, screen render target, GPU volume texture, or extra
  libraries. Alpha fill rate and actual ASI performance unmeasured.
- Linux release g++ -O2 -DNDEBUG -Wall -Wextra -Werror: all four tests
  passed (cloud, canopy, volume, weather), ASan/UBSan weather test passed.
  weather_tests covers bidirectional native interpolation, forced clear
  with residual rain, fog/dust/extra-sunny/lightning, static mask alpha
  endpoints, live light/no weather uploads, direction supersession,
  stable group identity and interior density/haze. Preview simulation
  renders actual C++ meshes and textures with new live alpha/color/haze;
  docs/previews/weather_transition.jpg, weather_light.jpg and
  weather_interior.jpg show synthetic sky/terrain only. Not GTA captures.
- In-game test next: rapidly switch sunny/cloudy/rain/fog/sand in both
  directions and via ForceWeatherNow; compare native sky, thunder and
  cloud latency. Fly through dense center and edge at day/night, cockpit
  and third-person, check no HUD washout, low draw distance, RW reset,
  fill-rate on integrated GPU. If haze is excessive near terrain, try
  InteriorHaze=0.5 or 0, then consider depth-aware fixed-function near
  geometry rather than adding shaders. Preserve legacy fallback modes.


## Continuous 3D cloud banks — consolidate the endpoint (2026-11-27)

User's direction: keep the visually successful world-space canopy, but
replace isolated 3D blobs. Stop adding separate experimental modes; GTA V
visual target, cheap enough for older GPUs. New default still RenderMode=2,
HybridClouds=1, CumulusDetail=0. Modes 0/1 and HybridClouds=0 fallback retained.

- Inspected three supplied flight screenshots (airbreak at approx z=523,
  552, 468). They show a thin sheet well below sparse/translucent nearby
  objects. GTA V and RDR1/RDR2 screenshot search; Rockstar SIGGRAPH 2019
  Creating the Atmospheric World of RDR2, D3D9 volume texture documentation,
  GPU Gems 39, GitHub jaagupku/volumetric-clouds (weather mask, height,
  erosion), rsrodev/ProjectVAtmosphere (raymarch and half-res tradeoffs).
  Original implementation, no game asset/code transfer. Rejected an
  experimental connected heightfield shell after Linux preview looked
  like folded terrain.
- CloudBanks.hpp: two original periodic 128x128x32 RGBA density/relief
  textures (detail 256x256x32), generated from canopy macro density plus
  3D warped multi-frequency noise, flat bases, rounded uneven crowns,
  eroded edges. Clear/wet alpha endpoints depend on actual configured
  density. Static overhead extinction and oblique density taps provide
  diffuse shading; current timecycle RGB and lightning live. No atlas,
  view-specific baked texture, random puff geometry or directional refresh.
- Mesh: 16 view-aligned slices max (24 detail), 12x10 grid cells/slice,
  world XYZ texture coords and fixed altitude, same wind as canopy.
  Slice positions depend on view, but sampled density remains stationary.
  Far-to-near list, radial/range/near fade. Conservative 16x16x8 block
  occupancy data includes filter halos to reject empty quads. Local
  volume fades to distant canopy; above-layer canopy more strongly faded.
  InteriorHaze existing key now retains opacity at the closest sample
  planes rather than triggering the removed screen-space overlay.
- Fixed function D3D9 direct indexed-UP draw, because RW Im3D lacks 3D UVs.
  FVF XYZ+DIFFUSE+TEX1+TEXCOORDSIZE3(0), texture wrap U/V, clamp W,
  hardware trilinear volume sampling, LERP with TFACTOR for weather then
  MODULATE diffuse. One draw, Z test on, Z write off, SRCALPHA/INVSRCALPHA.
  RW pending state cache flushed before D3DSBT_ALL capture. Restore all
  state on exit; no persistent state block. Volume textures created
  SYSTEMMEM staging -> DEFAULT pool (D3D9Ex compatible in principle),
  SDK d3dLostEvent/d3dResetEvent release owned textures, CPU pixels
  retained for reupload. RW shutdown also releases all CPU/GPU resources.
  Pure device, unsupported caps, failures fall back to full canopy.
  Explicit device support checks; no actual D3D9 runtime here.
- Cost: 4 MiB volume memory plus ~2.67 MiB canopy, 16x12x10x2=3840
  conservative pre-cull slice triangles + up to 960 canopy, one volume
  draw + 1/2 canopy draws. Alpha fill rate is the principal risk; up to
  16 layers per pixel, actual depends on screen coverage and occupancy.
  CPU density map generated once ~0.21s in Linux release tests; first-use
  hitch possible (detail more expensive), no per-frame uploads. 2070
  triangles at user's approximate flight coordinates vs 2564 unculled.
  No measured actual game performance or minimum hardware claim.
- Removed CloudVolumes.hpp, old volume_tests and hybrid_preview files;
  previous designs remain in git history, not selectable modes. Updated
  native weather test for live endpoint interpolation, added banks_tests
  for deterministic texture, coverage, wrap, empty vertical borders,
  occupancy rejection, fixed XYZ, depth order, bounded geometry. Preview
  tests/banks_preview.cpp/.py render actual geometry and trilinear texture
  in software; docs/previews/banks_*.jpg are synthetic-sky diagnostics.
- In-game validation next: inspect shape at ground/523m/above, orbit/yaw,
  depth against player and world, rain/clear/night/flash, alt-tab/device
  reset with D3D9Ex and SkyGfx. On pure or unsupported volume devices
  canopy remains. This is an approximation with finite sampling
  artifacts, not exact GTA V/RDR2 appearance. Do not increase default
  sampling without weak-GPU evidence. Shader path only if actual results
  demonstrate it can improve visuals at equal or lower fill cost.

- Win32 CI exposed the private RW flush prototype being hidden by SDK
  header guards. Call the exact validated SA 1.0 US wrapper target
  0x7FC200 (SDK RenderWare.cpp and supplied function list).

## Runtime failure: pure device and volume texture support (2026-11-27)

User's actual log: Canopy: static weather masks generated (live coverage).
3D cloud bank unavailable; retaining canopy. The previous portable
previews did not establish runtime output. User explicitly wants to keep
native 3D volume textures (no return to an atlas or RenderWare cards).
No new mode or changes to density generation, settings or shader budget.

- SA 1.0 US supplied Hoodlum IDB: D3D9System 0x7F5F70; 0x7F6741
  tests D3DDEVCAPS_HWTRANSFORMANDLIGHT (0x10000), 0x7F6749 tests
  D3DDEVCAPS_PUREDEVICE (0x100000), 0x7F6751 ORs 0x50
  (D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE).
  0x7F6781 invokes IDirect3D9::CreateDevice. The old EnsureBanks
  unconditionally rejected pure mode, making its failure likely on
  a normal SA installation. Actual user's flags were not previously logged.
- RwD3D9GetCurrentD3DDevice at 0x7F9D50 is NOT a trivial getter:
  invalidates surfaces, streams/FVF/shader state and _LastMatrixUsed.
  Do not repeatedly call it during rendering. Read _RwD3DDevice
  0xC97C28 directly (also confirmed in gta-reversed RenderWare.h).
  CameraBeginUpdate 0x7F8F20 computes D3D view matrix at 0xC9BC80
  and projection at 0x8E2458; 0x7F8FB1/0x7F9068 apply them through
  RwD3D9SetTransform. Use these current matrices explicitly. Direct
  state getters are prohibited on pure devices, but D3DSBT_ALL
  captures/restores state independently; 0x7FC200 flush pending RW
  changes before capture. Verify Apply HRESULT.
- CloudD3D9.hpp factors the native upload/draw path so Win32 device
  test exercises exact same code. Stage upload uses proper pitches and
  checks every operation, logs HRESULT on failure; retry managed
  resources only when staging fails, then progressively smaller
  powers of two. Linear or point filter from actual caps. Dual-volume
  interpolation requires 3 active blend stages (not 4), supports LERP
  or BLENDFACTORALPHA; nearest endpoint only for hardware lacking both.
  Pure device flags are diagnostic, never a rejection criterion.
  Bounded world-space mesh, one hardware draw, same memory budget.
  A one-time optional occlusion query is polled without flush or
  waiting and released on reset; zero samples means no depth-passing
  raster fragments on that frame, not necessarily broken density.
- Win32 cloud_d3d9_volume test: native CreateDevice with pure flags
  first, then non-pure HAL/REF if available; uploads actual volume
  textures, renders two quads with distinct W coordinates and checks
  RGB/alpha at all weather endpoints via GPU readback. No synthetic
  rasterizer. Test skip code 77 if no volume-capable device, verbose CI
  logs identify pure/non-pure/skip. Test does not exercise SA's camera
  and engine state. Portable four tests passed locally with g++.
- Next: install fresh ASI artifact from PR #1, keep RenderMode=2,
  HybridClouds=1, inspect log device flags, ready dims, draw submission
  and optional depth-passing sample count. If failure, exact HRESULT
  and operation are now recorded. Do not infer successful in-game
  rendering from Linux preview or build alone.


## Flight distortion follow-up (2026-11-27)

User's in-game log establishes that the prior pure-device compatibility fix
worked, including linear-filtered 128×128×32 native volume textures, 8
submitted planes / 256 triangles on first frame (camera Z=24.7), and 282194
depth-passing GPU occlusion samples. This is real game evidence, distinct
from the software diagnostics.

Examined the user's 22.4 s / 2560×1440 recording frame-by-frame around 7–10 s.
At approximately Z=673, player coordinates (2164,-1265,673) move toward
(2255,-1341,673); cloud contours smear and change during traversal.
At ~490m, bright cloud almost fills the view; above the layer, broad
white/yellow shapes lack discernible relief. Camera heading is approximate,
not reconstructed from a game-state capture. Dawn colour filter likely
contributes to washout, so do not interpret software preview as exact game
lighting.

Root code issues: sample distances were recalculated from near/far projected
bounds and capped at 16 (24 detail); step and phase could change while
moving or pitching, causing undersampling and apparent shape deformation.
Opacity often accumulated too strongly. A fixed XYZ cell prototype was
discarded after local diagnostics exposed discontinuities at axis transitions.

- Keep Mode 2 + HybridClouds as default, no new mode, shader or texture
  memory increase. Use parallel fixed-phase planes (40m economy, 28m
  detail, max 36/52) anchored to projected world position minus unwrapped
  wind displacement. At fixed heading, camera movement and clip changes
  preserve sample locations, even crossing a whole slice boundary. Rotate
  only the integration plane orientation with the view (finite sampling
  can still alias as pitch/yaw changes). Actual density remains world fixed.
- Use a lower reference optical density, nonlinear soft-edge/dense-core
  opacity curve, larger vertical crown variation, sharper coverage ramp,
  and four baked oblique extinction taps with overhead extinction. Reserve
  headroom for the post effect at dawn/noon, leave canopy/night/lightning
  unchanged. No per-frame lighting bake or texture upload; no game assets.
  Fade close samples rather than letting one slice overwhelm the view.
- Keep one native D3D9 draw call and 4 MiB texture memory in economy.
  Increase theoretical triangle cap to 8640 before occupancy rejection.
  Portable ordinary flight pose draws 3374/5312 after/before culling.
  Alpha fill may increase; no potato GPU benchmark or game FPS guarantee.
- Four local portable regression tests pass with g++ -O2 -Wall -Wextra
  -Werror; banks test verifies lattice phase at 144 pose/detail combinations
  plus travel/clip continuity, with 30 shared planes across video poses.
  Baked volume measured ~177ms on this Linux host. Added real D3D9
  perspective scene readback test to complement the existing sampler
  isolation test. CI validates actual rasterization, not SA's post effects.
- Updated rendered software diagnostics for all scenes in docs/previews;
  these are not in-game captures. For next verification, use the same
  Air Break test at dawn and noon, ascent around 400–700m then level
  travel/yaw at 670m. Look for stationary outlines and relief instead of
  white slabs; compare frametime against prior ASI. If residual warp under
  changes in angle persists, measure whether a small bounded PS2.0
  integration shader (optional, at equal/lower fill budget) would be
  preferable. Do not add successive experimental modes.


## Independent cloud atmosphere, rotation stability and final cosmetic pass (2026-11-27)

User tested prior build and says substantially improved. Screenshot at
(2532.6,-905.3,399.3), 07:31, still reveals lateral haze and scene fog
limitations; reports some close-cloud jitter during camera rotations.
Requested independent D3D9 fog/distance, cosmetic quality and optimization,
and a concise, human-commented INI.

- Mode 2 no longer calls legacy range() for its visibility. CloudDrawDistance
  4200, CloudFogStart 800, CloudFogEnd 2800 (world units), independently
  configurable. Legacy card modes keep their original settings. The near
  true volume remains 1400m max radius; long-range clouds are the canopy.
- Direct3D9 pretransformed XYZRHW vertices carry perspective-correct 3D UVs
  and vertex fog factor in specular alpha. Read the verified current game
  matrices from their known addresses and GetViewport (explicitly supported
  on pure devices). Clip against near and lateral frustum planes (CPU);
  split any triangles across far boundary and clamp only far Z. Keep
  original XY and reciprocal W. Far clouds render at MaxZ with existing
  Z test and no Z write; nearby rendered geometry remains in front.
  Native table fog is disabled for these draws, vertex fog blends into
  current horizon/sky colour according to radial distance. No extra
  fullscreen pass, shader, or scene fog/camera changes. Unavailable
  vertex fog caps fall back to alpha haze. Original game post FX remain.
- Distant canopy now native D3D9 2D textures with CPU alpha-weighted
  mipmaps, staged upload and managed fallback; CPU source pixels kept for
  device reset. No RenderWare private raster extension, same default GPU
  texture budget. Both canopy masks and volume share a single D3DSBT_ALL
  snapshot with RW flush; restore before other game effects.
- Persistent sampling normal with 8-degree dead zone, moves only outside
  it, preserving phase about current eye during turns. Translation/wind
  advances phase using world displacement. Nested 20/40/80m economy
  (14/28/56 detail) samples crossfade optical weights, not coordinates.
  Finer nearby detail, farther fewer planes. Near slices use 6x5 rather
  than 12x10 quads; cache rays and lengths, project only referenced
  vertices. 3514 vs previous 3374 source triangles at reference flight
  pose; actual alpha fill and old hardware performance still need testing.
- Cosmetic: edge erosion varies with crown density, local fine relief,
  softer scattering floor, shaded internal structure and pale rims.
  Distant canopy has broader directional gradient. Still baked in same
  two texture endpoints, no per-frame bake/raymarch or extra texture
  fetches. Original sky palette, night and weather behavior remain.
- Fresh INIs grouped and commented, reduced to 19 main controls including
  three independent atmosphere options. Remove CumulusImpostors and
  CanopyFlightClouds; old INI ignored keys harmless. Classic-only and
  redundant advanced settings no longer generated but still parsed.
  Never overwrite an existing file. Ship separately named defaults.ini.
- Local portable regression: all four passed, including fixed phase
  through turns/translation/clip, 4-sample optical weight conservation,
  independent range/fog clamps and generated INI cleanup. Native D3D9
  CI test extended to cover actual projected 3D/2D draw, far-depth
  occlusion, fog/restore sentinel, weather blend and perspective scenes
  with short game clip. Linux previews are diagnostics, not GTA captures.
- Verify in game: test same flight through 399m, noon/dawn/night, slow
  orbit/pitch and rapid turns, very short vs long world draw distance,
  and unchanged distant map/near CJ/objects. Cloud colour distance must
  no longer track world fog; test alternative CloudFogStart/End and
  CloudDrawDistance. Check SkyGfx and D3D9Ex alt-tab/reset. No new modes.

## Radial shells and true long-range volume with far LOD (2026-11-27)

User liked the independent-atmosphere iteration but supplied two new 30fps
2560x1440 recordings: gta_sa_tQwqYibadk.mp4 (619 decoded frames) and
gta_sa_9fGI0C9byo.mp4 (387). Decoded all frames to reduced-size diagnostics;
inspected time sheets, 0.1-second successive crops, and adjacent pairs
selected by sky-patch absolute difference after a small XY shift.
First video: rapid airbreak travel around 2.0–3.1s visibly reshapes banks;
5.23–5.30s and 9.60–9.67s contain abrupt cloud changes with substantial
scene motion (15.1s peak is tree occlusion, not a cloud defect).
Second: especially 0–7.7s the dense bank has a distinct lower boundary
and nearly empty horizontal strip; 7.8–9.0s moving through the seam
makes it obvious. Compression, physical parallax and motion are mixed
in the recordings; cannot prove every change is a renderer error.

Actual source problems: 1400m hard cap (also tile-size-dependent),
hybrid canopy ring fading away with altitude, fixed-phase but rotating
view-aligned quadrature. This revision removes these, without adopting
shaders or copying another game's assets.

- Static world-axis cube-sphere triangles, concentric about eye. Fixed
  radial samples at 16/32/64/90m step (detail 12/24/48/72m), angle/FOV
  independent; shells sorted by radial distance. At fixed eye, same
  world ray sees identical interpolants regardless of view. Small
  nearest shells use 6×6 grids instead of 12×12; conservative frustum
  masks once per topology, slab clipping and occupancy skip. Source
  vertex limit 44k protects 16-bit projection; extreme custom settings
  may truncate geometry. Translation changes the samples continuously
  and still may alias; cannot guarantee no motion artifacts in GTA.
- 8000m default range, fog 1600–8000; old explicit INI values retained.
  Squared opacity taper beyond 75% range softens terminal edge. New
  FarBillboards=1, BillboardStartDistance=5000, BillboardStartRatio=.8.
  Effective start max(minimum,ratio*range); blend across max(350,10% of
  range), bounded by range. Full volume available with FarBillboards=0.
  No billboard at 4200m. UseAdditionalTextures=0 new default.
- Bake two 512² original atlas textures by integrating the SAME
  3D density inside overlapping world cells, 4×4 periodic cells ×
  eight azimuths, 64×32 tile each with guarded edges. Smooth cell
  partition, front-to-back optical compositing, shared atlas draw,
  no mip bleeding, continuous azimuth fade and radial distance sort.
  Only generated if useful; fallback keeps full range on unsupported
  allocation or extreme tiny cells. GPU +2 MiB optional. Atlas direction
  fades and distant vertical cards are approximations, not true volume.
- Full-volume canopy a faint distant underlay, no 1400m ring or height
  cutoff. Canopy-only path unchanged. Slightly stronger fine erosion,
  pale crown highlights and less heavy baked core shadows.
- Local portable regression: all four pass with GCC O2/Werror. Actual
  same-ray triangle intersections + trilinear alpha through ±35° yaw,
  ±0.30 pitch and scene clip changes: all 98 samples match. Range
  1800/4200/6000/8000/12000 yields max source radius
  1665/4005/5445/7065/10665m; disabling billboards at 12000 reaches
  >11000m. Default flight 7643 source triangles and 76 distant cards;
  4200m 5200 triangles. Approx. 0.68s one-time atlas bake. Added native
  D3D9 test with actual atlas geometry past 450m scene clip plus clear/
  interpolated/wet rendering. Real-game validation still required.
- Visual regression previews use actual portable density, shells and
  atlas against synthetic sky, not GTA screenshots. Compare same flight
  poses, rotate in place and translate slowly, try 4200/8000/12000
  with independent fog end adjusted accordingly. Compare FPS on weak
  hardware; longer range increases the scene's alpha fill despite LOD.

## Player-height handover and fixed world lattice (2026-11-27)

User supplied `gta_sa_ld7LQlESSy.mp4` (2560x1440, 30 fps, 526 frames at
17.53s). Examined all decoded frames in contact sheets and contiguous
eight-frame crops around 2.0s, 4.5s, 6.3s and 10.4s. The aircraft/airbreak
movement visibly changes bank silhouettes and internal relief, with
large changes during ascent/descent (especially around 4.67–4.73s).
Actual parallax, rapid altitude changes and compression are mixed with
rendering artifacts; this does not isolate the error pixel-for-pixel.
The prior radial shells were rotation-invariant, but moved all density
sampling surfaces with the camera during translation. That limitation
remained a credible source of the swimming seen in flight.

- Player Z is authoritative: Plugin-SDK `FindPlayerEntity()` at 0x56E120
  returns ped or occupied vehicle, null if absent (also verified in
  gta-reversed `source/game_sa/World_Helper.cpp`). No camera-height
  substitution. User explicitly requested a 90% height ratio:
  canopy altitude = min + .52 * (max-min). Default 434m, start 390.6m,
  full volume at 434m. Smoothstep through the gap, reversed on descent;
  at ground no per-frame 3D mesh, draw or far impostors. No new INI
  option; changing layer heights automatically changes the threshold.
  Bank alpha follows squared blend to account for accumulation through
  multiple planes, atlas card alpha follows the normal blend.
  No player or failed bank => full canopy. Hybrid canopy is fixed flat
  at canopy Z, no geometry bending during the blend. Standalone curved
  canopy retained. Bakes done once, volume/atlas GPU upload on demand.
- Replace camera-centred shell geometry with world-anchored X/Y/Z
  lattice. Economy 20/40/80m nested sample spacing, detail 16/32/64m;
  vertical spacing aligns to height band. Per-vertex normalized
  dominant-axis weights crossfade diagonals. LOD weights sum to a
  constant optical path for each four consecutive lattice samples.
  Never translate the sample lattice with the camera, including wind
  (density/UV drift as before). Only texture-period integer offsets
  change to retain precision. Spatial bricks subdivide locally where
  angular interpolation requires it; exact same plane locations both
  sides of subdivision. AABB frustum rejection, hierarchical distance
  ordering, stable patch ties, conservative occupancy prefix sums (two
  periodic XY copies, 16-bit counts). No shaders, new mode or render
  targets, no per-frame texture allocation or texture generation. Intersecting
  slices during the diagonal blend are still approximately sorted.
- Existing independent cloud fog, draw distance and atlas LOD retained.
  8km default: volume reaches ~7196m before taper, 76 atlas cards;
  12km with impostors reaches ~10800m, without >11000m. The source
  geometry budget remains 44k vertices. No claims about actual FPS on
  low-end hardware: local 180-pose CPU mesh benchmark, prior shells
  mean ~1.05ms / 8400 triangles, new ~1.46ms / 7717 triangles. Reference
  flight ~7850 triangles vs old ~7643. Ground-level volume work is zero.
  Local benchmark is only the mesh, not the GTA frame/GPU.
- All four portable GCC release regressions pass, including real
  triangle/ray intersections during yaw/pitch, translations up to
  231m on the same world line, corresponding actual world XYZ/UVW and
  density, axis/LOD continuity, occupancy prefix parity against brute
  force across periodic boundaries, and player-height ratios at
  custom heights. D3D9 Win32 tests retained for true native readback.
  Updated ground/entry/flight/above diagnostics generated from same
  C++ density geometry, using synthetic sky, not a GTA capture.
- In-game priority for the next tester: hover with airbreak below
  390m, move camera without moving CJ, ascend through 391–434m,
  fly sideways at constant height then independently rotate, descend
  through the same band. Look for remaining seam/diagonal/cell boundary
  artifacts. Check 4200/8000/12000 ranges and frame times in flight.
  No assertion that every real-game distortion is fixed; test the
  actual ASI before making new changes to the density art.

## Potato CPU optimization iteration

- User approved the stabilized world-fixed volume look; **do not replace the
  art or reduce sampling resolution**. Target Celeron 847 / HD Graphics class,
  not a measured FPS promise. Altitude handover now starts 85% of canopy
  height (368.9m for default 434m); full volume unchanged at 434m.
- CloudCache.hpp stores bit-exact baked masks, volumetric endpoints,
  conservative occupancy arrays and billboard atlas. Version + relevant
  bake settings + exact expected lengths + FNV-1a checksum. On a cache miss
  write through a temporary sibling and atomically replace. File lives beside
  ASI, no per-frame reads/writes. Wind changes UV; live weather blends
  fixed endpoints. CI builds/packs the default cache with tools/bake_cache.cpp.
- Hardware vertex processing: VS 2.0 compiled with FXC at build time (optional
  fallback if unavailable), fixed-function texture stage/pixels unchanged.
  Per-corner UV, distance, nested-plane opacity, axis weighting and fog on
  hardware. Dynamic streamed VB + static reversed quad IB. Two GPU
  projections preserve exact XY/W with near/far clipping, distant pass depth
  clamped to the cleared viewport depth; restore viewport and full D3D state.
  No pixel shader, screen copy, extra sample per pixel, or new RT.
  Hardware creation/draw failure uses proven CPU projection path.
- Local measurements (not target device): 480 moving poses, previous mesh
  1.18ms, optimized CPU 1.14ms, GPU input 0.43ms; 7275/7276 mean triangles.
  Shared conservative endpoint culling avoids compiler rounding mismatch.
  Default cache 8,432,196 bytes, 919ms bake / 29ms warm reopen on Linux.
  Regression tests verify exact cache data and invalidation and same CPU/GPU
  world-lattice geometry at 12 altitude/weather combinations. Windows test
  compiles VS2 and compares actual D3D9 pixel readback against CPU backend
  where hardware vertex processing is available. Device logs identify path.
- Next real-game test: compare same flight/location, time, weather on Celeron
  with frame time overlay. Ground, altitude transition 369–434m, flight close
  inside clouds and short/long scene fog. Check log for cache hit and hardware
  vertex path; test GPU and CPU fallback if there are driver artifacts.
- Historical research: GPU Gems 1 ch39 (texture slicing, baked lighting);
  Mark Harris GDC 2002 (amortize cloud rendering via impostors);
  OGRE SkyX geometry-block reference; D3D9 SDK buffer, fog and viewport docs.
  See README for sources. No copied game assets/code.

- Initial Win32 CI (commit c3a657c) passed all six tests on an actual D3D9
  pure device. Hardware-vs-CPU framebuffer mean error on the three flight
  scenes was 0.032, 0.042 and 0.070 RGB levels out of 255, including clear,
  interpolated and overcast masks, with foreground depth preserved. FXC
  compiled the shader successfully. Follow-up uses one conservative
  candidate classifier for both backends to satisfy GCC 15 as well.

## Source mesh, memory bandwidth and HDD preload optimization

- Kept mode 2 defaults, density/opacity art, world lattice, altitude handover
  and LOD unchanged. GPU corner representation now 16 bytes (XYZ float3 +
  D3DCOLOR axis/parity) instead of 28; VS2 decodes exact one-hot axis flags,
  parity and uniform lattice step. Static reversed index buffer means
  no per-frame index creation on hardware. Shared culling stops testing
  corners once a patch is known potentially visible; sort only bounds and
  generate surviving CPU vertices after culling. 480 poses, same local
  host: previous/new GPU-input 0.422/0.321ms, CPU 1.200/0.944ms; both
  7276 mean triangles, 14552 vertices (excludes projection/draw).
- D3D9 A8L8 (two 8-bit channels) is chosen only with advertised actual
  resource type + filter support and successful upload. Whole weather
  pair falls back at the same resolution to RGBA on failure. All existing
  pixels are greyscale + alpha; reject unexpected colour data. Canopy
  mips remain alpha-weighted and atlas has no mips. Default logical GPU
  texture footprint ~4.33 MiB if all compact formats work, ~8.67 MiB
  otherwise. No measured bandwidth or frame-time claim for this GPU.
- CloudCache.hpp v2 saves exact original luminance/alpha, decodes into
  the original RGBA CPU vectors via bounded sequential reads, preserves
  occupancy and atlas data. V1 remains loadable; no forced rebuild.
  Default v2: 4,237,892 bytes vs 8,432,196 v1. Local warm filesystem
  average of 20 full loads: 10.4ms vs 16.8ms. Actual physical HDD and
  Windows driver upload times unmeasured. Cache tests check exact
  byte-for-byte decoded data, legacy compatibility, stale key, truncation,
  corruption and rejection of non-greyscale writes.
- HDD mode is always on, no INI knob: PrepareSchematic loads at initRwEvent;
  PreloadClouds also uploads all available textures before gameplay.
  initGameEvent offers an idempotent second chance after startup/device
  initialization; fallback remains lazy if a loader missed both events.
  The CPU schematic survives reInitGameEvent and D3D lost/reset, and is
  only freed on RW shutdown. GPU resources reupload from resident data.
  No cache reads or writes during camera movement or live weather/wind;
  memory is pageable, not force-pinned. Preloading trades a small startup
  cost for no first-flight texture creation.
- Native D3D9 test now compares mipmapped patterned 2D textures, generated
  end-to-end volume flight views and far atlas weather endpoints against
  RGBA framebuffer readback. No scene/lattice downgrade. Pure device
  support, depth sentinel and CPU/GPU parity remain CI gates. Verify
  cache hit/preload log and actual in-game stutter on the Celeron before
  claiming FPS or target hardware success.

## Exact-output pass trimming and state/visibility optimization

- Prior CI 46084519550 (2305562) passed 5 Linux and 6 Win32 tests:
  real D3D9 pure-device A8L8 volume, canopy and atlas readbacks differed
  from RGBA by at most 1 RGB level out of 255. Hardware-vs-CPU mean
  error remained .032/.042/.070. Preloaded v2 cache 4,237,892 bytes.
- This pass keeps texture bakes, cache version, lighting, defaults, LOD
  and sampling lattice intact. GPU source mesh retains a contiguous
  range + enclosing AABB per brick. Each of the complementary
  projections identifies the first/last span that may survive its
  homogeneous clipping plane; skip the fully clipped ends of the
  existing reversed IB. Use double bounds plus conservative floating
  dot-product guard, including cancellation in scene w-z. Ambiguous
  spans and interior gaps are still hardware-clipped. Empty projection
  can be skipped completely. Never reorder alpha slices or add draws.
- D3D9 DrawState removes repeated render/sampler/texture-stage setters
  exclusively within the existing captured cloud state scope. It does
  not cache across frames, modify state-block lifetime, skip first
  setters, or query pure-device state. Standalone draws bypass cache.
  No cache for shader/declaration/texture transitions. Single-layer
  ground render bypasses state dedup. Device reset unchanged.
- Compute two constant axes of occupancy query once per brick; reuse
  wrapped integer cells, skip duplicate root visibility test, sort
  compact 16-byte patch descriptors directly with original tie order.
  Ground canopy static topology/radii + reuse already computed vertex
  distance for fog; skip zero-weight canopy and single-pass alpha
  backup. Far atlas scratch persists; test strictly zero-fade radial
  cells before square root with 1m endpoint guard. No changes to alpha.
- Same-host 480 varying camera positions, default economy, 450m
  scene far clip: packed GPU source 0.323 -> 0.250ms mesh only,
  0.268ms including range selection; CPU fallback 0.918 -> 0.848ms.
  Source CPU vertex+index hashes and packed GPU vertex hashes are
  exactly unchanged. 7276 triangles and 14552 vertices average;
  old two projections submit 29105 vertices average, new 19025
  (-34.6%). Canopy 3.59 -> 3.12us, atlas 17.93 -> 12.33us on same
  480 poses; canopy position/UV/alpha/topology and atlas buffer
  hashes unchanged. No target-device FPS claim.
- Native Win32 regression compares full and subrange hardware
  submissions at scene clips 75/450/1800/12000m in clear, mixed
  and overcast scenes. Requires exact RGB framebuffer parity,
  long-clip distant pass omitted, and actual submission reduction.
  Also compares complete multi-pass fixed-function state sequences
  with/without dedup including 2D/3D, dual/single stages, fog and
  sampler settings. Portable tests validate every AABB span and
  omitted vertex against both clip sides. Keep any future changes
  conservative and validate against full uncropped reference.

## Lossless closest-corner and coarse-grid CPU optimization

- Previous Win32 + Linux CI run 46085858567 passed; 12 hardware projection
  readbacks had exactly zero RGB differences; state dedup skipped 498 of 598
  multi-pass state setter calls in the scripted multi-pass sequence.
- A more detailed occupancy grid was prototyped and rejected: the extra
  prefix queries increased CPU work, while eliminating only ~1.6% of a
  projected-area proxy in the default 480-pose flight (not a GPU measurement).
  No additional occupancy data or cache version changes were retained.
- Instead keep the original 16x16x8 conservative occupancy data and cache
  per-axis point-cell results within each brick. Each point range is exactly
  one cell; weather endpoints and the two other dimensions are constant.
  Calculate a three-bit eligible-axis mask once, reuse for subdivision and
  slice generation. Precompute original three squared distance limits.
- For each slicing axis, select the closest independent endpoints on the
  other two axes: this corner minimizes both the squared radial distance
  and max absolute coordinate, so an angular/outer-radius failure there
  implies all four corners fail. If it is inside the original 8m near
  exclusion, use the original four-corner predicate. Maintain exact XYZ
  sum order, guarded thresholds, original slice depth key, ordering and
  lattice. Specialize the three axis loops at compile time.
- Build root z intervals once per frame; reuse y intervals per row; reject
  XY columns with nearest XY squared distance >= outer squared radius
  before constructing individual root bricks. Preserve original XYZ
  accumulation and sort order. No geometry-quality limits changed.
- Local GCC -O3, identical flags and machine, 5 alternating 480-pose
  runs (median): GPU source mesh 0.266 -> 0.201 ms (~24% faster
  in mesh generation only), identical 7276 triangles / 14552 vertices
  average, identical packed GPU source hash 16681346361779809689.
  Separate 1440-pose variation (4200/8000/12000m ranges, economy/detail
  spacing, weather, near-vertical views) combined CPU vertices, indices,
  GPU vertices and brick spans: exact same hash 4494552806871744396,
  26,192,456 total vertices; GPU 0.324 -> 0.263 ms and CPU fallback
  1.135 -> 1.084 ms for that run. Standalone fixture:
  tests/mesh_parity_benchmark.cpp (compile with -I<checkout>; compare
  old/new under same compiler flags). Timings exclude GPU rendering.
- All 5 portable GCC regression suites passed, including new direct
  randomized 300,000-candidate equivalence and original 8m/diagonal
  boundary tests. No changes to textures, shader, draw states, altitude
  handover, default INI, draw distance, or cache payload. Actual Celeron
  frame times and in-game screenshots require the target game/hardware.
