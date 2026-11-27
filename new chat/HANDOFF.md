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
