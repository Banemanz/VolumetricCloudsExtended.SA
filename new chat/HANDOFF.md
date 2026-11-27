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
