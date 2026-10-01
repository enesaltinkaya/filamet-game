# Environment variables

Every env var the engine (or `scripts/run.sh`) reads, with its effect.
Conventions: **"any value"** = set to anything non-empty; **"1"** = truthy
flag (`atoi`/`!= nullptr`); floats are parsed with `atof`.
All `ENGINE_*` vars are read at the point marked; most are latched on
first use (`static`), so set them before launch, not mid-run.

## Automated / headless runs

| Var                                 | Value                                                   | Effect                                                                                                                                            |
| ----------------------------------- | ------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_SCREENSHOT`                 | path                                                    | Capture one frame as JPEG (quality 90) and quit. Also runs the window hidden (headless) and parks the player (the scripted camera owns the view). |
| `ENGINE_SCREENSHOT_FRAME`           | int                                                     | Which frame to capture (default 100 when `ENGINE_SCREENSHOT` is set; 0 means 1).                                                                  |
| `ENGINE_LOG_TIMEOUT`                | ms                                                      | Auto-stop the engine after N ms of wall time — the safety net for automated runs.                                                                 |
| `ENGINE_AUTOTEST`                   | `enter` \| `pause` \| `settings` \| `credits` \| `exit` | Main-menu scripted action, fired once. `pause` enters the world and opens the pause menu; `exit` quits.                                           |
| `ENGINE_PAUSE_AUTOTEST`             | `back` \| `settings` \| `mainmenu`                      | Paired with `ENGINE_AUTOTEST=pause` — scripted pause-menu action, fired once.                                                                     |
| `ENGINE_SETTINGS_AUTOTEST`          | `close` \| `audio` \| `video` \| `graphics`             | Open the matching settings page (or close it) in the settings GUI.                                                                                |
| `ENGINE_AUDIO_SETTINGS_AUTOTEST`    | `close` \| `effects50`                                  | `close` exercises the BACK path; `effects50` applies a slider change (bind → debounce → settings.json).                                           |
| `ENGINE_VIDEO_SETTINGS_AUTOTEST`    | `close` \| `uiscale15` \| `fullscreen`                  | Same pattern; `uiscale15` verifies slider → apply → persist, `fullscreen` verifies the window toggle.                                             |
| `ENGINE_GRAPHICS_SETTINGS_AUTOTEST` | `close` \| `wire`                                       | `wire` flips 5 toggles + sets 6 sliders, lets `update()` apply them to `data/settings.json`, then closes — the settings wiring test.              |
| `ENGINE_FAKE_ESC_FRAMES`            | comma-sep frames                                        | Inject a synthetic ESC keypress at each listed frame number (up to 16).                                                                           |
| `ENGINE_FAKE_DRAG`                  | any value                                               | Hold RMB + sweep yaw every frame — exercises the interactive orbit-drag path headlessly.                                                          |
| `ENGINE_STAGE_DUMP`                 | dir                                                     | Per-frame burst dump (frames `ENGINE_SCREENSHOT_FRAME`.., `ENGINE_SCREENSHOT_BURST` files, `..._BURST_STRIDE` spacing): `N_ssao.png` (AO map), `N_depth.png` (inverse-depth, 3 m/R8), `N_depth.f32` (raw linear view-z: u32 w, h + f32 rows — the precision the R8 map cannot give at distance, where one R8 step is ~2 m at 40 m), `N_moved.txt` (camera-motion flags + dEye/dRot + camera basis + `eye` = this frame's world anchor), `N_world.png` + `N_accum.png` (TAA chain; `N_world.png` is written with TAA off too — the raw scene color, so debug passes are measurable without TAA). |
| `ENGINE_WINDOW`                     | `WxH` (≥320x240)                                       | Force the window/client size (e.g. `640x360`). A full-size capture costs ~1.2 s of GPU readback + PNG per frame, and the fixed-step loop advances the sim by the 250 ms-capped frameTime per frame — a small window drops that to ~110 ms/frame, so bursts sample the camera dolly ~10x finer. |

## RenderDoc

Debug builds only (the whole block is `#ifndef NDEBUG`). The app-side API
talks to a preloaded `librenderdoc.so` (see `scripts/run.sh renderdoc`, which
also exports `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` and pins the radeon ICD).

| Var                               | Default                                               | Effect                                                                                      |
| --------------------------------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `ENGINE_RENDERDOC_CAPTURE`        | —                                                     | Arm the app-side capture.                                                                   |
| `ENGINE_RENDERDOC_CAPTURE_FRAMES` | 30                                                    | Frame at which the capture is triggered.                                                    |
| `ENGINE_RENDERDOC_DIR`            | `/tmp2/RenderDoc/c-game`                              | Capture filename prefix; files land at `<dir>_frameN.rdc`.                                  |
| `ENGINE_RENDERDOC_LIB`            | `/home/enes/Apps/renderdoc/build/lib/librenderdoc.so` | `librenderdoc.so` path for the `dlopen` (an already-loaded `LD_PRELOAD` copy is preferred). |

## Camera / player

| Var                   | Value                               | Effect                                                                                                  |
| --------------------- | ----------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `ENGINE_TELEPORT`     | `x,y,z`                             | Override the spawn position (world metres, f32).                                                        |
| `ENGINE_CAMERA_DOLLY` | truthy                              | From wherever the camera sits when the world comes up, dolly straight backwards (opposite the look direction) 10 m over one leg, then back to the start over one leg, looping. Turnarounds are smooth (cosine, velocity zero at both ends); the orientation never changes. Parks the player (the scripted camera owns the view) — pair with `ENGINE_SCREENSHOT_FRAME` / `ENGINE_DEBUG_CAM` to inspect a frame that has actually moved. Interactively the same dolly is the **`V`** key: press it to arm the dolly at the live camera (it takes the view from player mode; `C` hands the view back, which ends the dolly) and press it again to stop with the camera back at the anchor — no env-var round-trip. |
| `ENGINE_CAMERA_DOLLY_LEG`   | float (s)  | Dolly leg duration (default 1.5; distance stays 10 m → speed = 10/leg s — e.g. 30 = a 0.33 m/s dolly for slow-motion A/B). |
| `ENGINE_CAMERA_DOLLY_FRAME_DT` | float (s/frame) | Advance the dolly by a FIXED dt each frame instead of wall-clock dt (0.0154 reproduces the 65 fps step at the default leg). Wall-clock dt ties the pose lattice to fps, so two runs never share a pose and temporal A/B measures ~3 gray of phase noise; with a fixed dt and `ENGINE_SCREENSHOT_BURST_STRIDE`, runs are pose-identical and TAA/jitter deltas become resolvable. |
| `ENGINE_AUTO_RUN`     | truthy                              | Auto-run forward from spawn; the third-person camera follows (the camera-follow test). Any W/S cancels. |
| `ENGINE_AUTO_JUMP`  | int (fixed ticks)                   | Fire a jump every N ticks (60 = 1 s) — the character-controller jump/air-state test. Needs player mode: pair with `ENGINE_AUTO_RUN=1` in automated runs. Implies the state-db write gate (see `automatedRun()`).                                       |
| `ENGINE_TICKS_PER_FRAME` | int (ticks/frame) | Run exactly N fixed simulation ticks per rendered frame and clear the wall-clock accumulator. The stage-dump/screenshot path renders ~6 fps, and the wall-clock accumulator then hands every frame 10+ ticks — per-frame camera/object motion (every velocity law, TAA included) scales with fps, so a burst run does not reproduce real-time running flow. `=1` makes each frame advance 1/60 s of simulation at any fps: the pose lattice is fps-independent and running flow per frame matches a 60 fps session. |
| `ENGINE_TPOSE`        | truthy                              | Always play the character T-pose (inspect hook).                                                        |
| `ENGINE_NO_ANIM`      | any value                           | Skip loading/playing the character animation clips.                                                     |
| `ENGINE_NO_PLAYER`    | any value                           | Keep the player parked (same gate as screenshot runs).                                            |
| `ENGINE_FOG_DENSITY`  | float                               | Override the exponential fog density (default 0.00035).                                                 |

## GLTF / character

| Var                   | Value     | Effect                                                                                                            |
| --------------------- | --------- | ----------------------------------------------------------------------------------------------------------------- |
| `ENGINE_GLTF_DEBUG`   | any value | Log GLB structure at load + first pose frames (nodes, clips, root row).                                           |
| `ENGINE_JITTER_PROBE` | any value | Log player eye/pos and glTF mesh anchor-space position every 5th frame — the f32-absolute-placement jitter check. |

## Renderer

| Var                  | Value     | Effect                                                                                |
| -------------------- | --------- | ------------------------------------------------------------------------------------- |
| `ENGINE_NO_GPU_TIME` | any value | Disable the per-frame GPU-time query (A/B the timing overhead).                       |
| `ENGINE_DEBUG_CAM`   | any value | Log the camera eye/center/up + view matrix every frame.                               |
| `ENGINE_RML_PROBE`   | any value | Verbose rmlui log: frame commands/bounding boxes, texture table, per-batch draw info. |
| `ENGINE_AO_MOTION_RESET`    | float (px/frame) | Camera-motion threshold for the SSAO history reset (eye translation at near-plane depth + view rotation, converted to screen pixels). Default 0.1; 0 = never reset (keep history while the camera moves — the ghost baseline). |
| `ENGINE_SSAO_RESET_TRACE`   | any value | Log each SSAO accumulation-reset state change (ACTIVE/off) with this frame's dEye + dRot. |
| `ENGINE_SSAO_NO_HISTORY`    | any value | Force the SSAO accumulation reset every frame (single-frame AO — the ghost-free ground truth). |
| `ENGINE_TAA_MOTION_PX`          | float (px/frame) | Jitter-gate cutoff: at this image displacement of a point at `ENGINE_TAA_MOTION_Z` depth the TAA Halton jitter is fully faded out (linear fade starting at 0.25× the value). Default 2. Above it the frame renders unjittered: with no history to dither, the jitter only shimmers contrast edges. `ENGINE_TAA_MOTION_JITTER_OFF=1` disables the gate. |
| `ENGINE_TAA_MOTION_Z`           | float (m)        | Reference depth of the jitter gate's px/frame measure (the near field). Default 12. |
| `ENGINE_TAA_MOTION_VECTOR_DIFF` | float            | DiligentFX `TAA_MOTION_VECTOR_DIFF_FACTOR`: the stock per-pixel-motion scale that kills TAA's history alpha (`alpha *= saturate(1 - |mv_uv| * factor)`); engine default 16 (kills the history at ~102 px/frame of flow at 1627 px height; the library default 256 kills it near 1 px/frame). Only the response below: with `taaGhost`/`ENGINE_TAA_GHOST` above 0 the runtime velocity knee (px/frame) replaces this law. |
| `ENGINE_TAA_GHOST`            | float (0–1)      | Anti-ghost strength, overriding the `taaGhost` setting. 0 = the stock DiligentFX response (library law + `TAA_MAX_VARIANCE_GAMMA` 2.5 + threshold 0.9). Above 0 all three terms move together: velocity knee `102 − 98 × ghost` px/frame, neighborhood clamp-box `2.5 − 1.5 × ghost` × stddev, depth threshold `0.9 + 0.07 × ghost`. |
| `ENGINE_TAA_GAMMA_MAX`        | float (× stddev) | Neighborhood color-clamp box scale at zero velocity (`TAA_MAX_VARIANCE_GAMMA` override; default 2.5 − 1.5 × ghost, clamped by the adaptive edge term down to `TAA_MIN_VARIANCE_GAMMA` 0.75). Smaller box = stale history is clipped harder = shorter ghosts. |
| `ENGINE_TAA_VELOCITY_KNEE`    | float (px/frame) | Per-pixel velocity where the TAA history weight dies out: `alpha *= 1 - smoothstep(0.25×knee, knee, px/frame)`. Default `102 − 98 × ghost`; ghost 0 disables the law (the factor-16 law above takes over). Running flow at the character's depth is 10–20 px/frame, so the knee is the ghost control. |
| `ENGINE_JITTER_SCALE`           | float            | TAA sub-pixel jitter amplitude (default 0.25 px; 0 = no jitter). |
| `ENGINE_FX_SHADER_DEFINES`      | `NAME=VAL;...` | Extra HLSL defines appended to every DiligentFX post-FX shader compile — the channel `ENGINE_TAA_MOTION_VECTOR_DIFF` writes through; set it directly to override the engine default. |
| `ENGINE_NO_SHADER_CACHE`      | any value | Bypass the SPIR-V shader cache (compile every shader from source). The diagnostic for "my shader edit did nothing". |
| `DILIGENT_FX_SHADER_INC_DIR`  | dir       | Directory of the DiligentFX embedded shader sources (`shaders_inc`, the File2String output) folded into the shader-cache key. The FX factory serves those bytes, so the key tracks exactly what the compiler sees. Default `/home/enes/Projects/c/cpp-thirdparty/diligent/git/build-linux/DiligentFX/shaders_inc`; a missing dir disables the fingerprint. |
| `ENGINE_SHADER_CACHE_TRACE`   | any value | Log each shader-cache key (tree fingerprint, key length, source form) before lookup — the diagnostic when a shader edit looks ignored even with the fingerprint in place. |

| Var                        | Value     | Effect                                                                                                                              |
| -------------------------- | --------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_SHADOW_MODE`       | 0–4       | Override `shadowMode` at startup: 0=off, 1=PCF, 2=VSM, 3=EVSM2 (default), 4=EVSM4.                                                    |
| `ENGINE_SHADOW_FOCUS_BOX`  | `0`       | Disable the player-focused cascade-0 re-fit (A/B: stock slice-sized cascade 0).                                                      |
| `ENGINE_SHADOW_FOCUS_HALF` | float (m) | Override the focus-box half-extent (4–40 m; default auto: caster height / tan(sun elevation) + focus margin, clamped 6–32 m).         |
| `ENGINE_SHADOW_SLOPE_BIAS` | float     | Shadow-caster `SlopeScaledDepthBias` (all casters, all cascades). Default 0.5; 2.0 = the old stock value (wall-base shadows detach and flash while the camera moves). |
| `ENGINE_SHADOW_TRACE`      | frameN    | One-shot trace line: band, focus box, cascade z rows, texel sizes, player light NDC, cascade ring radius (`R`), player screen position (`px`/`py`). |
| `ENGINE_SHADOW_TRACE_STRIDE` | frames   | Trace cadence (1–1000, default 50): `1` = every frame, for per-frame cascade/caster traces.                                        |
| `ENGINE_SHADOW_NO_SNAP`    | any       | Disable cascade texel snapping (manager `SnapCascades` + the anchor-relative centre snap) — cascade centres go bit-constant.       |
| `ENGINE_SHADOW_C1_TEXEL_OFFSET` | float | Shift cascade 1's box centre by N texels (A/B: how much a cascade-1 grid shift is worth on screen).                              |
| `ENGINE_SHADOW_ORACLE`     | frameN    | One-shot caster-vs-receiver matrix consistency check (pass/fail per point/cascade).                                                  |
| `ENGINE_SHADOW_RINGS`      | `0`       | Disable player-anchored cascade rings: cascade k = cube centered on the player with radius `focusHalf·(tierDist/focusHalf)^(k/(N-1))`, z-gate = player view-Z + radius. Default on for all shadow modes; `0` = stock camera-frustum distribution. Ring sets per quality tier (focusHalf 6 m): low 3 cascades/1024/60 m = 6/19/60 m → 11.7/37/117 mm texel; medium 4/2048/80 m = 6/14.2/33.7/80 → 5.9/13.9/32.9/78 mm; high 5/2048/120 m = 6/12.7/26.8/56.7/120 → 5.9/12.4/26.2/55.4/117 mm. Measured shadow-pass cost +0.65 ms GPU per extra cascade (2 casc 4.50 ms, 4 = 5.79, 5 = 6.40). |
| `ENGINE_SHADOW_RING_GRID`  | `0`       | Ring cascade box centres snap on a grid anchored at the CAMERA's light-space position (`0`) instead of the world origin (default). Camera-anchored = the snap grid slides with the camera, so every dolly frame re-picks the centre in texel steps and the whole atlas re-lays: measured 100 % of atlas texels change per frame vs 0 % world-anchored, and the dolly shadow-edge residual 37.9 → 2.9 gray. |
| `ENGINE_SHADOW_FILTER_WORLD` | float (m) | World-space shadow filter footprint: baked default **0.12**, applied to the moments modes (VSM/EVSM) only. With it `ShadowMapManager::ConvertToFilterable` derives each cascade's atlas blur radius from its own light-space scale, so every cascade filters the same WORLD area; the splat receiver scales its tap kernel and the Chebyshev variance floor by the same per-cascade factors. `0` = the legacy fixed-texel kernel (cascade 1's penumbra = 13x cascade 0's at the 2048/2-cascade tier = the cascade-boundary seam). **PCF (mode 1) runs the fixed-texel kernel (radius 1 texel) unless this var is set explicitly**: PCF has no pre-blurred atlas, and 8 taps cannot average a coarse-texel disc — at 0.12 m the fine cascade's 20-texel disc printed concentric rings, and capping the radius at 8 texels left cascade 0 filtering a SMALLER world disc than cascade 1 (47 mm vs 120 mm), splitting two penumbra widths across the cascade-0 box faces. |
| `ENGINE_SHADOW_TRANSITION`     | float (0–1) | `ShadowAttribs.fCascadeTransitionRegion` (default **0.3**, was 0.1): width of the cascade-to-cascade blend band, as a fraction of the cascade's z-range AND of its distance to a box face (the receivers take the MIN: player-anchored ring boxes are not cubes, so an XY-face crossing sits at a large z distance — DiligentFX's `max()` never fires the band there and the face renders as a straight-edged box around the player). The band is the last camera-depth-driven term in the shadow field, so its width sets the band residual: 0.1 = 13.65, 0.3 = 11.70, 0.5 = 5.82 gray dolly-frame. |
| `ENGINE_SHADOW_BLEND_DITHER`   | float (0–1) | Amplitude of the interleaved-gradient-noise dither added to the cascade-blend factor in the terrain AND props receivers (default 0): breaks the blend band's iso-lines into per-pixel noise for the AA/TAA to average, anchored to the atlas texel grid so it does not shimmer with the camera. |
| `ENGINE_SHADOW_UNION`          | 0/1       | Terrain cascade composition: 1 (default) = multiply every covering cascade; 0 = lit-wins (the finest covering cascade is authoritative, the props receiver's policy). |
| `ENGINE_SPLAT_SHADOW_DEBUG`    | mode      | Terrain PS shadow debug output: 1 = cascade coverage (the FINEST covering cascade), 2 = final attenuation, 3 = raw per-cascade attenuation (magenta = uncovered), 4 = EVSM pos/neg moments, 6 = view-z + warped moments + tap UV, 7 = slope/cliff/sand weights, 100+ = material terms (100 roughness, 101 spec term, 102 normal, 103 NdotH). |
| `ENGINE_SHADOW_ATLAS_DUMP`   | dir       | Per-frame shadow atlas readback burst (same `ENGINE_SCREENSHOT_FRAME`/`_BURST`/`_BURST_STRIDE` window as the stage dump, so files share frame numbers): `<frame>_depthN.f32` = cascade N's raw depth slice, `<frame>_momN.f32` = its filterable (VSM/EVSM) moments. Header = u32 w, h, slices + f32 rows. The temporal test for atlas stability: consecutive frames must be bit-identical while only the camera moves. |

## IBL

| Var                         | Value | Effect                                                                                                               |
| --------------------------- | ----- | -------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_IBL_ENV`            | name  | Environment file from `images/studiolights/` (stem or filename); default `kloofendal_48d_partly_cloudy_puresky_1k`.  |
| `ENGINE_IBL_INTENSITY`      | float | Global IBL scale (diffuse + specular, each path's IBLScale); also the DebugGui IBL intensity ±0.25 steps.            |
| `ENGINE_IBL_SPEC_INTENSITY` | float | Specular-only attenuation: scales the prefiltered env the spec lobe samples; diffuse irradiance stays full.          |
| `ENGINE_IBL_SPEC_CLAMP`     | float | Luminance clamp on the specular env copy (default 100, ≤0 disables). Blanks the HDRI's baked sun disk — the analytic |
|                             |       | sun already supplies that energy; unclamped it prefiltered into a sheen spike on sun-facing reflections.             |

## GUI (rmlui)

| Var                | Value     | Effect                                                                     |
| ------------------ | --------- | -------------------------------------------------------------------------- |
| `ENGINE_NO_RMLUI`  | any value | Disable rmlui entirely — no menus shown (ESC is ignored in-world).         |
| `ENGINE_DEBUG_GUI` | any value | Show the debug GUI headlessly (interactive toggle is Ctrl+B).              |
| `ENGINE_STATS_GUI` | any value | Show the stats + pass-stats GUIs headlessly (toggles are Ctrl+D / Ctrl+P). |

## General debug

| Var            | Value | Effect                                                                                                              |
| -------------- | ----- | ------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_DEBUG` | `1`   | `utils::isDebug()`: enables the rmlui debugger UI and debug signal-catcher behaviour. Exported by `scripts/run.sh`. |

## Standard / external

Read, not engine-specific (listed because they change behaviour):

| Var                                             | Effect                                                                                                                                  |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `XDG_CONFIG_HOME` / `HOME`                      | `c-utils/cfgpath` base for the `settings.json` location.                                                                                |
| `XDG_DATA_HOME` / `HOME`                        | Data path (cfgpath).                                                                                                                    |
| `XDG_CACHE_HOME` / `HOME`                       | Cache path (cfgpath).                                                                                                                   |
| `WAYLAND_DISPLAY`                               | If unset (X11), display scale is also stored as `cursorScale`; on Wayland the rmlui cursor scaling is left alone.                       |
| `TERM`                                          | Not read by the engine — `scripts/run.sh` runs `clear` under `set -e` and aborts if it's unset (so no-TTY invocations must set it).     |
| `LD_PRELOAD`, `ENABLE_VULKAN_RENDERDOC_CAPTURE` | RenderDoc layer mode (see above) — the implicit Vulkan layer is required because Diligent's Vulkan backend loads entry points via volk. |
| `VK_ICD_FILENAMES`                              | Set by `scripts/run.sh` to the radeon ICD.                                                                                              |
