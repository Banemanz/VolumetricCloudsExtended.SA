# VolumetricCloudsExtended.SA

A procedural replacement for GTA San Andreas' stock intersecting-card volumetric clouds.
The default renderer combines a world-space continuous 3D density layer with the
distant procedural canopy. It uses hardware-filtered volume texture slices and
fixed-function blending, not a per-pixel raymarch shader. It only
replaces the volumetric cloud call in the original 3D effects pass. The sky background,
low clouds, moving fog, rain and other effects remain native.

## Install

Use desktop GTA SA **1.0 US**, an ASI loader, and the Win32 ASI from the GitHub
Actions build. Put it in the game directory (or an ASI-loader-supported mod directory).
The plugin creates `VolumetricCloudsExtended.ini` beside the ASI on first load and
does not overwrite an existing INI. Restart the game after editing the settings.
The accompanying log reports whether the executable and call target matched.

An unrecognized executable or an already redirected volumetric-cloud call is left
untouched. Other graphics modifications that affect the main camera/RenderWare
pipeline may still need compatibility testing.

## Renderer

- World-space cloud groups drift with the game wind and time step, not camera yaw.
- Fixed altitude band: flying upward no longer relocates the entire cloud layer.
- Area-uniform placement, bounded minimum spacing at spawn, and perimeter recycling
  after a cloud leaves view. Teleports rebuild the field around the new position.
- Smooth weather interpolation in both directions, including bad-to-bad transitions.
  Excess clouds fade out instead of remaining permanently allocated.
- Clustered native-texture clouds (mode 1): fixed crossed core
  with independently placed, deterministically tilted satellite cards,
  depth-sorted and angle-weighted. No camera-facing billboards. Increasing
  the puff count does not simply multiply opacity.
- Crossed cards (mode 0) preserve the native world-axis geometry.
- **Continuous 3D cloud banks (default, mode 2):** the canopy's macro
  coverage drives a periodic, original 3D density field with billowing
  tops, flatter bases, and soft erosion. The nearby field is sampled by
  bounded back-to-front view slices with absolute world XYZ texture
  coordinates. Rotation of the camera does not rotate the density.
  `HybridClouds=0` selects the inexpensive original canopy-only renderer.
- Frustum rejection, independent mode 2 cloud range, smooth range and near fades,
  back-to-front view-depth ordering and bounded RenderWare batches.
- Additional textures are borrowed directly from the loaded particle TXD. Missing
  names are ignored; the native cloudhigh texture is the fallback. No global game
  RNG consumption or native cloud-table reset. Only mode 2 owns generated canopy rasters and optional D3D9 volume textures.

## INI controls

Fresh installations generate a small, commented INI (also included as
`VolumetricCloudsExtended.defaults.ini` in the ZIP). Existing INIs are
never rewritten. Copy the new example over only if you deliberately want
to reset your settings; otherwise add the three new atmosphere options.

| Setting | Default | Meaning |
|---|---:|---|
| RenderMode | 2 | 2 = continuous volume banks + distant layer; 1 = clustered classic cards; 0 = crossed classic cards |
| HybridClouds | 1 | Mode 2: 0 uses only the inexpensive distant cloud layer |
| CumulusDetail | 0 | Economy (recommended); 1 = larger textures and more samples |
| TargetProcClouds | 160 | Sky coverage in mode 2; classic cloud group count otherwise; 0 disables |
| BadWeatherDensityMultiplier | 1.4 | Cloud density in bad weather relative to clear (0–3) |
| MinSpawnZ / MaxSpawnZ | 200 / 650 | Cloud layer height above sea level |
| SizeScale | 1 | Size of the cloud formations |
| WindMoveScale | 0.9 | Wind movement multiplier |
| Opacity | 1 | Cloud thickness (0–2) |
| Seed | 17321 | Repeatable cloud pattern |
| CloudDrawDistance | 4200 | Independent cloud visibility distance in mode 2 (300–12000 world units) |
| CloudFogStart / CloudFogEnd | 800 / 2800 | Cloud-only colour haze starts/ends here. Equal values disable colour fog. |
| CumulusContrast | 0.65 | Strength of baked cloud relief (0–1) |
| CumulusNightLight | 1 | Cloud night brightness (0–2) |
| PuffsPerCloud | 5 | Mode 1 only: overlapping puffs per group |
| UseAdditionalTextures | 1 | Classic modes: use extra textures from particle.txd |
| CloudTextureNames | cloud2,cloud3,cloud4 | Classic modes: optional extra texture names |

### Existing INIs and advanced compatibility controls

Older INIs remain readable. The following advanced keys are intentionally
not emitted in a fresh INI: most are only relevant to the classic card
renderer or duplicate useful controls above. Values are clamped and
malformed text is ignored.

- `RenderMaxDist`, `SpawnRadiusMin/Max`, `RespawnMaxDist`,
  `MinSpacing2D`, `SpawnFadeInSpeed`, `QuadFacingMin`,
  `FarClipFadeRange`, `FarClipFadeMargin`, `TimecycFarClipScale`:
  classic card simulation/range controls. Mode 2 does not read the game's
  far clip or timecycle fog distance for cloud visibility.
- `LightingMode=0`: classic neutral native brightness (average sky RGB +
  stock 64). Explicit `LightingMode=1` enables the older tint and shading
  options: `UseTimecycTint`, `TimecycTintStrength` (also
  `TimecycleTintStrength`), `FluffyCloudBottomBlend` (also
  `TimecycTintSkyInfluence`), `BottomShade`, `HazeStrength`.
  `ColorBrightnessBoost` sets the original luminance reference.
- `UseGameFog` is classic-only: it has no influence on mode 2.
- `IncludeOriginalTextureInRandom` remains a classic-only override.
- `CumulusDensity`, `CumulusRimStrength`, `InteriorHaze`, and
  `NearFadeDistance` retain their existing behavior as advanced overrides.
  For most users, `Opacity`, `CumulusContrast`, and the defaults suffice.
- Removed unused `CumulusImpostors` and optional `CanopyFlightClouds`:
  the latter mixed old card wisps into mode 2 and is no longer needed.

To switch an existing installation to the recommended renderer:

```ini
RenderMode=2
HybridClouds=1
CumulusDetail=0
CloudDrawDistance=4200
CloudFogStart=800
CloudFogEnd=2800
```

## Continuous 3D cloud banks (mode 2, default)

### Independent cloud atmosphere

Mode 2 uses its own distance and colour-haze parameters for **both** the
volume banks and distant canopy. It uses the current sky palette instead
of the world's active fog colour and parameters. Radial fog is calculated
per vertex from world distance; D3D9 applies it via specular alpha on
pretransformed vertices, after texture blending, without a fog overlay or
a second full-screen pass. Weather changes still control live cloud
coverage, lighting and visibility.

The game's original projection and viewport are preserved. Local cloud
fragments use their normal scene depth. Triangles crossing the original
far clip are split, and cloud geometry beyond it is assigned the maximum
scene depth while retaining the correct XY projection and reciprocal W.
It can therefore occupy sky pixels but remains behind rendered scene
geometry. No edits to the camera far plane, fog globals or world drawing
distance; the game's fog and post-processing continue as normal. Hardware
without vertex fog instead fades distant cloud opacity. Standard D3D9
depth-buffer precision and other mods' rendering remain limitations.

See Microsoft's [vertex fog documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d9/vertex-fog)
and [pure-device GetViewport documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getviewport).

### One density field, not isolated impostors

The 78-view atlas, individual cumulus groups, orientation blending and
screen-space haze from earlier revisions are removed. The far canopy
remains unchanged when HybridClouds=0. In the default path it fades
toward a continuous 3D field derived from the same macro weather map.

- **Shape:** original periodic multi-scale 3D noise distorts a smoothed
  version of the canopy coverage. Height envelopes, irregular crowns,
  erosion and a flatter base form continuous banks. XYZ density remains
  in world space, translated by native wind, including during flight.
- **Lighting:** static optical-depth accumulation down the cloud column
  and four oblique density taps, a local density gradient and a soft
  multiple-scattering proxy give finer billow relief, softer shaded cores
  and brighter edges. Original detailed noise erodes thinner edges.
  Native sky/timecycle RGB and lightning are applied live, with daylight
  headroom for SA’s post-processing colour filter, especially at dawn. This is approximate baked relief, not dynamic directional
  shadowing or full multiple scattering.
- **Rendering:** ordinary hardware-filtered RGBA volume textures
  (128×128×32 each, clear/overcast endpoints). A bounded stack of
  parallel view-aligned sample planes carries actual world XYZ texture coordinates.
  A nested, wind-advected sampling lattice uses 20/40/80 m intervals
  (14/28/56 m in detail mode). Smooth optical-weight transitions retain
  the near samples and reduce distant overdraw. A persistent normal has
  an eight-degree angular dead zone. Beyond it, the sampling planes rotate
  about the eye rather than the world origin, retaining the sampling phase.
  Changing camera position or scene clip range cannot rescale the lattice.
  Large turns still change the finite quadrature orientation, not density.
  Fixed-function texture stages linearly interpolate the weather endpoints
  and multiply live vertex RGB/alpha. The indexed triangle list is submitted
  back-to-front in one draw, with existing scene depth testing, no depth
  writes, no screen render targets, no 3D noise generation per frame and
  no custom shaders. The 16×16×8 conservative occupancy map rejects
  transparent cells including a one-texel filtering halo. The cloud-only
  range drives the distant layer; the actual 3D bank radius is separately
  bounded to 1400m for fill-rate economy.
- **Flight:** the volume exists at its actual sampled depths, including
  in front of and behind the player/aircraft. Sampling near the eye
  creates local visibility loss, replacing the depth-unaware
  full-screen haze. Closest planes fade more gently to prevent one slice
  suddenly occupying the entire view. InteriorHaze controls the residual
  near-sample opacity rather
  than a separate overlay. There may be gaps between slices and banding
  at very close range. Limited sampling does not provide true per-pixel
  volumetric integration.
- **Cost:** one 3D draw (plus one or two existing canopy draws). Economy
  caps at 60 × 12 × 10 × 2 = 14,400 source triangles in unusually wide
  custom views, detail 90 planes. Normal views use far fewer: the reference
  economy flight pose submits 3514 source triangles after rejection versus
  3374 before the change. Near planes use a 6×5 grid. Projection may split
  some triangles at the viewport/scene far plane. Cached ray lengths avoid
  repeated sqrt calls; only referenced vertices are projected.
  The canopy and volume share one state-block capture/restore per pass.
  Finer near sampling may increase fill; coarser distance sampling offsets it.
  Two RGBA volume textures occupy 4 MiB (16 MiB at detail 1); total
  with mipmapped canopy ~6.67 MiB / ~26.67 MiB, excluding driver overhead.
  CPU keeps the 4/16 MiB volume pixels and 2/8 MiB canopy source pixels
  for device restore, plus a tiny occupancy grid and bounded mesh buffers.
  No recurring texture uploads for weather or lighting.
  GPU fill rate, particularly inside a thick overcast, matters more than
  triangle counts. Performance on old/integrated hardware is not measured.
- **Compatibility:** the direct D3D9 draw carries actual three-component
  texture coordinates, not a RenderWare 2D atlas. SA's normal pure-device
  mode is accepted. The SA 1.0 US device pointer and camera matrices are
  read directly (verified against the IDB); the SDK's device getter
  invalidates RW's matrix cache and is deliberately avoided. Pipeline
  state is captured/restored via D3DSBT_ALL, with no prohibited pure-device
  state getters. RW's pending states are flushed first. Both canopy and
  banks use native D3D9 textures/vertices in a shared state scope. Volume textures
  are uploaded through system-memory staging into the default pool; a
  managed-pool attempt and smaller power-of-two textures are tried on
  failure. Lower-capability hardware can use point sampling and, only
  if two-volume blending is unavailable, the nearest weather endpoint.
  No volume support or failed draws retain the canopy. Upload failures
  log the precise operation and HRESULT, and a successful first draw
  logs submitted slice/triangle counts. A one-shot optional asynchronous
  GPU occlusion query records depth-passing samples without waiting.
  Device loss/reset and RW shutdown release owned resources.
- **Initialization:** the original map and volume are generated once on
  the game thread. The economy volume bake measured ~0.18 s in local Linux
  release tests (plus canopy generation/upload); high detail takes longer.
  First-use hitch is possible, unlike the old gradual atlas bake.

The target is convincing 2013-era cloud volumes at 2004-game budgets,
not a recreation of Rockstar assets or renderer. References:
[Rockstar's RDR2 atmospheric rendering presentation](https://advances.realtimerendering.com/s2019/),
[GPU Gems 1, chapter 39: Volume Rendering Techniques](https://developer.nvidia.com/gpugems/gpugems/part-vi-beyond-triangles/chapter-39-volume-rendering-techniques),
[Direct3D 9 volume textures](https://learn.microsoft.com/en-us/windows/win32/direct3d9/volume-texture-resources),
[Unity community cloud renderer](https://github.com/jaagupku/volumetric-clouds)
(weather mask, vertical profile, erosion), and
[Project V Atmosphere](https://github.com/rsrodev/ProjectVAtmosphere)
(reference for modern volume costs, not copied code or architecture).
A future optional simple shader could combine density integration and
lighting in fewer passes; it is not required in this iteration.

### Native weather synchronization

Mode 2 takes the game's already-interpolated `CWeather::CloudCoverage`,
`Foggyness` and `ExtraSunnyness` every frame. Rainy and sandstorm intent are
interpolated from the old/new weather types and `InterpolationValue`.
The independently step-limited rain/dust particle intensities do not
hold the cloud field in an old weather state after a forced weather
change (script-forced rain can additionally contribute). Fog and dust
reduce cloud visibility, extra-sunny reduces coverage opacity, and
lightning follows `LightningFlash` immediately. Native interpolated sky
and timecycle colors set live brightness; ordinary weather changes no
longer initiate or wait for texture generation.

The canopy uses two correlated static masks: base coverage and extra
coverage. Their alpha composition matches both baked endpoints at full
geometry opacity; intermediate coverage and edge fades are approximate.
If the density multiplier is below one, the mask weights reverse.
On hardware with dual-volume support, the nearby volume interpolates two density endpoints in the same draw.
Modes 0/1 retain their existing weather behavior.

### Runtime validation and diagnostics

The Win32 CTest `cloud_d3d9_volume` creates a native D3D9 device (attempts SA's
pure-device flags first), uploads actual 3D textures with the same backend
as the ASI, draws the clear/intermediate/wet endpoints, and reads pixels
back to verify that changing the third texture coordinate changes the
output. The expanded tests check cloud-only vertex fog and foreground
occlusion outside the scene far plane, fog state restoration through a
subsequent native draw, native mipmapped canopy upload/draw, and the actual
perspective volume mesh with a 450m scene far clip at positions from the
flight recording.
A machine without a volume-capable D3D9 device reports **skipped**,
not passed. This does not test the GTA camera or integration.

The log beside the ASI distinguishes `3D cloud device:`,
`3D cloud bank ready:`, empty camera frustum, and
`3D cloud draw submitted:`. A supported occlusion query also logs the
first draw's depth-passing sample count. If the renderer falls back,
send the new log lines including HRESULT, not only the canopy screenshot.

### Linux software diagnostics — density/mesh only

| From the ground | At aircraft altitude |
|---|---|
| ![Day](docs/previews/banks_ground.jpg) | ![Flight](docs/previews/banks_flight.jpg) |
| ![Above](docs/previews/banks_above.jpg) | ![Rain](docs/previews/banks_rainflight.jpg) |
| ![Night](docs/previews/banks_night.jpg) | ![Entry](docs/previews/banks_entry.jpg) |
| ![Dawn, starting position](docs/previews/banks_dawn.jpg) | ![Dawn, after travel](docs/previews/banks_dawnmove.jpg) |

Actual C++ density textures and slice triangles, software-rendered with
trilinear filtering, synthetic sky, not GTA screenshots. They do not
verify that the game successfully initialized or drew the volume. The last
user-provided build log does confirm actual in-game 3D output on a pure D3D9
device; the updated sampling/lighting still need in-game visual confirmation.

```sh
g++ -std=c++17 -O2 tests/banks_preview.cpp -o banks_preview
python tests/banks_preview.py ./banks_preview docs/previews
```

### The distant canopy (also available standalone with HybridClouds=0)


A curved, textured sky layer has been practical since the fixed-function
console era. References: [Quake III tr_sky.c](https://github.com/id-Software/Quake-III-Arena/blob/master/code/renderer/tr_sky.c)
(curved cloud texture coordinates), [OpenMW sky.cpp](https://github.com/OpenMW/openmw/blob/master/apps/openmw/mwrender/sky.cpp)
(scrolling UVs and weather transitions), [SimGear cloud.cxx](https://github.com/FlightGear/simgear/blob/next/simgear/scene/sky/cloud.cxx)
(layer altitude, thickness and coverage), and
[Adrian Courrèges' GTA V frame capture](https://www.adriancourreges.com/blog/2015/11/02/gta-v-graphics-study/)
(the game uses a sky dome and a distant cloud ring with seamless density and normal textures).
Here directional lighting and shape are baked into original procedural pixels,
not a copy of any game's assets, code or shaders.

- **Shape:** periodic, domain-warped multi-scale value noise and cellular
  billows form connected banks with gaps and finer detail. Coverage changes
  with the interpolated weather and configured group count/multiplier.
  The same cloud map drifts with native wind; the weather threshold exposes
  more or less of the same field via two precomputed optical-alpha masks
  rather than regenerating random clouds.
- **Light:** static grayscale density-gradient relief, two-scale occlusion,
  fine detail and thin-edge shading. Per-frame live vertex RGB supplies
  neutral daylight, weather dimming and restrained sunrise/sunset warmth;
  nighttime uses the timecycle sky, not the stock +64 night brightness floor.
  Modes 0/1 retain their exact native/custom lighting options.
- **Geometry:** 257 vertices and 480 indexed triangles in one 32-segment,
  8-ring shallow disk. Absolute layer altitude and world-XY UVs stay stable
  under yaw, pitch, roll and camera altitude. The finite disk is recentered
  horizontally to cover the independent cloud range; only its gentle
  outer curvature and distance fade are camera-relative. There is no
  facing sprite, texture rotation, or per-cloud sort in the default path.
  Near the surface the alpha fades locally to avoid a hard sheet crossing
  the camera. The two-sided mesh is depth-tested against world geometry.
- **Cost:** one or two fixed-function texture+alpha blend submissions, no
  per-frame cloud raymarch, custom vertex/pixel shader, screen copy,
  3D GPU texture or render target. Economy uses two 512² RGBA masks
  (~2.67 MiB with mipmaps); detail 1 uses two 1024² masks (~10.67 MiB
  with mipmaps). Native D3D9 textures have alpha-weighted mipmaps; CPU
  source RGBA pixels are retained for lost/reset devices. The scalar map
  is freed. No subsequent uploads for weather or lighting. Empty masks
  are skipped, and clear usually needs only one pass. The same projected
  vertices are reused for both masks. On resource upload failure the
  plugin falls back to classic cards and logs the failure.
- **Flight:** the distant canopy remains a low-cost two-sided surface;
  the near volume supplies the actual 3D shape. Canopy-only mode remains
  available for weak hardware.

### Previous canopy-only Linux software previews

| Day | Overhead |
|---|---|
| ![Canopy daytime](docs/previews/canopy_day.jpg) | ![Canopy zenith](docs/previews/canopy_zenith.jpg) |
| ![Canopy dawn](docs/previews/canopy_sunrise.jpg) | ![Canopy night](docs/previews/canopy_night.jpg) |
| ![Canopy rain](docs/previews/canopy_rain.jpg) | ![Canopy above](docs/previews/canopy_flight.jpg) |

These render the actual C++ procedural texture and geometry against a
synthetic sky, clipping triangles at the near plane and perspective-correctly
sampling UVs. Not screenshots of GTA, no optional native wisps or RW mipmaps
simulated in this diagnostic. The above-layer view still exposes the sheet
at grazing angles. Real-game appearance, device behavior and GPU fill rate
remain to be validated. Historical cumulus images are previous versions.

## Build

Exact Plugin-SDK revision: [624a6a49265fd7a6fc63bda1611013ceabeacb8a](https://github.com/DK22Pac/plugin-sdk/commit/624a6a49265fd7a6fc63bda1611013ceabeacb8a)
(October 25, 2025). GitHub Actions builds the SDK and plugin using Visual Studio
2022 Win32 and uploads an ASI + PDB package. Local Windows build:

```powershell
$env:PLUGIN_SDK_DIR = "C:\path\to\plugin-sdk"
# From the pinned SDK checkout:
& "$env:PLUGIN_SDK_DIR\tools\premake\premake5.exe" vs2022 --file="$env:PLUGIN_SDK_DIR\tools\premake\premake5.lua"
msbuild plugin.sln /p:Configuration=Release '/p:Platform=Mixed Platforms' /t:plugin_sa /m
# From this repository:
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Portable regression tests on Linux (no game required):

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror tests/cloud_tests.cpp -o cloud_tests
./cloud_tests
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Dnear= -Dfar= tests/canopy_tests.cpp -o canopy_tests
./canopy_tests
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Dnear= -Dfar= tests/weather_tests.cpp -o weather_tests
./weather_tests
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Dnear= -Dfar= tests/banks_tests.cpp -o banks_tests
./banks_tests
```

## Engine reference

Reviewed gta-reversed [Clouds.cpp](https://github.com/gta-reversed/gta-reversed/blob/85bce0be4bd3229eb5ef445d3a7ac2e611a994f4/source/game_sa/Clouds.cpp),
[app_game.cpp](https://github.com/gta-reversed/gta-reversed/blob/85bce0be4bd3229eb5ef445d3a7ac2e611a994f4/source/app/app_game.cpp),
 [TimeCycle.cpp](https://github.com/gta-reversed/gta-reversed/blob/85bce0be4bd3229eb5ef445d3a7ac2e611a994f4/source/game_sa/TimeCycle.cpp)
and [Weather.cpp](https://github.com/gta-reversed/gta-reversed/blob/85bce0be4bd3229eb5ef445d3a7ac2e611a994f4/source/game_sa/Weather.cpp)
and the supplied SA 1.0 function index and disassembly dump. The native renderer uses three orthogonal planes at
`0x716380` and is called during `RenderEffects` at `0x53E170`, after moving fog and before
searchlights/post-effects, **not** inside `CClouds::Render`. The original repository's
call site `0x53E1B4` is verified against the IDB disassembly (part 06, E8 C7 81 1D 00) and retained with a runtime opcode/target check.
See [handoff](new%20chat/HANDOFF.md) for implementation and verification details.

## Offline geometry previews

These images are generated from the actual portable cloud geometry using a **synthetic
texture and sky**, not GTA assets or an in-game capture. They illustrate placement
and the two geometry modes, plus the shared C++ lighting calculation; real-game
appearance must be tested separately. The last row uses identical geometry,
synthetic texture and illustrative sunrise palette; only lighting settings differ.

| Clear / clustered | Rain / clustered |
|---|---|
| ![Clear scene](docs/previews/clear.jpg) | ![Rain scene](docs/previews/rain.jpg) |
| ![Flight scene](docs/previews/flight.jpg) | ![Legacy crossed cards](docs/previews/crossed.jpg) |

| Corrected sunrise (native colour) | Previous sunrise (full custom tint/shading) |
|---|---|
| ![Corrected](docs/previews/sunrise.jpg) | ![Previous](docs/previews/sunrise_old.jpg) |

To regenerate the previews (requires Python, NumPy and Pillow):

```sh
g++ -std=c++17 -O2 tests/dump_preview.cpp -o dump_preview
python tests/preview.py ./dump_preview docs/previews
```

To regenerate the canopy previews:

```sh
g++ -std=c++17 -O2 tests/canopy_preview.cpp -o canopy_preview
python tests/canopy_preview.py ./canopy_preview docs/previews
```
