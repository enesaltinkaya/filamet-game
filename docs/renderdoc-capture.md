# RenderDoc — Frame Capture & Inspection

How to capture a game frame with RenderDoc and inspect what's in the capture,
both interactively (GUI) and headlessly (Python replay API + `scripts/rdc.py`).

## What's installed

| Component                                                     | Location                                                                                        |
| ------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| RenderDoc source + build (v1.46-dev, `ENABLE_PYRENDERDOC=ON`) | `/home/enes/Apps/renderdoc`                                                                     |
| Core library (hooks + API)                                    | `/home/enes/Apps/renderdoc/build/lib/librenderdoc.so`                                           |
| GUI / CLI tools                                               | `/home/enes/Apps/renderdoc/build/bin/{qrenderdoc,renderdoccmd}` (symlinked into `~/.local/bin`) |
| Python replay module                                          | `/home/enes/Apps/renderdoc/build/lib/renderdoc.so` (use `PYTHONPATH=.../build/lib`)             |
| Implicit Vulkan layer registration                            | `/etc/vulkan/implicit_layer.d/renderdoc_capture.json`                                           |
| Game-side header (`renderdoc_app.h`)                          | `/home/enes/Apps/renderdoc/renderdoc/api/app/` (wired in `c-engine/CMakeLists.txt`)             |

The local build has `ENABLE_UNSUPPORTED_EXPERIMENTAL_POSSIBLY_BROKEN_WAYLAND=OFF`
— the layer therefore filters `VK_KHR_wayland_surface` out of the instance
extensions, which matters on Wayland sessions (see below).

If you rebuild or move the RenderDoc tree, update the layer json,
the `~/.local/bin` symlinks, `c-engine/CMakeLists.txt`, and the
`LD_PRELOAD` line in `scripts/run.sh`.

### Why layer + preload (not just LD_PRELOAD)

The engine loads Vulkan entry points through **volk** (`dlopen` + `dlsym`),
which bypasses the PLT — plain `LD_PRELOAD` symbol interposition never
intercepts those calls. The **implicit Vulkan layer** wraps the driver inside
the loader itself, so it works regardless of how the app fetches entry points.
Both `LD_PRELOAD` and the layer map the _same_ library file (deduped by
inode), so there is exactly one RenderDoc instance: the layer installs the
Vulkan hooks, the preload gives us `RENDERDOC_GetAPI` for triggering.

(`ENABLE_DLSYM_HOOKING` in the RenderDoc build is not an option here: it
requires glibc's internal `_dl_sym`, which glibc ≥ 2.34 no longer exports, so
it fails to link.)

### Wayland sessions

The layer's no-Wayland build interacts with two things:

- Diligent's default Linux build defines `VK_USE_PLATFORM_WAYLAND_KHR` and
  `CreateDeviceAndContextsVk` throws when the extension is missing. The local
  Diligent build strips that define (`sed` in `cpp-thirdparty/diligent/build.sh`,
  idempotent after updates) — the engine's Vk surface path is X11-only anyway
  (`c-engine/renderer/diligent/DiligentRenderer.cpp` reads only SDL X11
  properties).
- `scripts/run.sh renderdoc` exports `SDL_VIDEO_BACKEND=x11`, so under a
  Wayland compositor the window goes through XWayland and yields the X11
  window handle the surface path needs.

If you update Diligent and a renderdoc run dies with
`diligent: Required extension VK_KHR_wayland_surface is not available`,
re-run `cpp-thirdparty/diligent/build.sh` (it re-applies the patch).
See `docs/lessons/2026-09-07.md`.

## Capturing a frame

### 1. Programmatic (headless)

The game arms an in-process trigger behind env vars
(`c-engine/renderer/RenderDoc.cpp`, `c-engine/renderer/Renderer.cpp`;
debug builds, Linux):

```bash
ENGINE_RENDERDOC_CAPTURE=1 \
ENGINE_RENDERDOC_CAPTURE_FRAMES=300 \
./scripts/run.sh renderdoc
```

- `scripts/run.sh renderdoc` sets `LD_PRELOAD` (the lib above),
  `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` (activates the implicit layer),
  `SDL_VIDEO_BACKEND=x11` (see above), and pins the radeon ICD.
- `ENGINE_RENDERDOC_CAPTURE=1` arms the trigger; it fires at frame
  `ENGINE_RENDERDOC_CAPTURE_FRAMES` (default 30). The window is created
  hidden and the player is parked (scripted camera), so the captured
  frame is deterministic.
- The frame trigger is a frame count, not a wall-clock delay: pick it so the
  world is loaded (asset streaming takes a while — 300 lands in gameplay).
- **The engine exits on its own** two frames after a successful trigger —
  the layer serialises the capture during the triggering frame's queue
  submit, so the `.rdc` is complete when the process leaves. No
  `ENGINE_LOG_TIMEOUT` needed; keep one as a safety net for the case where
  the RenderDoc lib is missing and the trigger no-ops.
- Also set `ENGINE_AUTOTEST=enter` (run.sh does) to skip the main menu.

On success you'll see in the console:

```
renderdoc: using preloaded /home/enes/Apps/renderdoc/build/lib/librenderdoc.so
renderdoc: api ready, captures land at /tmp/RenderDoc/c-game_<...>_frameN.rdc
renderdoc: TriggerCapture
```

The capture file lands in `/tmp/RenderDoc/`:

```
/tmp/RenderDoc/c-game_frame300.rdc   (~400 MB)
```

(`ENGINE_RENDERDOC_DIR` overrides the filename prefix. Old captures pile
up — delete them, `scripts/rdc.py clean`.)

### 2. Interactive (GUI / target control)

```bash
./scripts/run.sh renderdoc     # no timeout: runs until you kill it
qrenderdoc                     # pick the running target, capture from the UI
```

RenderDoc's default in-app hotkey (F12, while the game window has focus) also
triggers a capture.

## Inspecting a capture

### GUI

```bash
qrenderdoc /tmp/RenderDoc/c-game_frame300.rdc
```

Draw calls, pipeline state, textures, buffers, shader sources, GPU counters.
`renderdoccmd thumb --out=/tmp/t.jpg <capture>` renders the backbuffer
headlessly (quick validity check of a capture).

### Headless (Python replay API)

The module is built with `ENABLE_PYRENDERDOC=ON`; no extra install needed:

```bash
PYTHONPATH=/home/enes/Apps/renderdoc/build/lib python3 <script>
```

Verified working recipe (action census + dumping a render target to PNG):

```python
import sys
sys.path.insert(0, "/home/enes/Apps/renderdoc/build/lib")
import renderdoc as rd
from collections import Counter

CAP = "/tmp/RenderDoc/c-game_frame300.rdc"

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
print("open:", cap.OpenFile(CAP, "", None), "| driver:", cap.DriverName())
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
print("replay:", res)

F = rd.ActionFlags

def walk(a):                       # draws are nested under pass/cmd-buffer
    yield a                        # boundary actions -> recurse children
    for c in a.children:
        yield from walk(c)

acts = [a for r in ctrl.GetRootActions() for a in walk(r)]
cnt = Counter()
for a in acts:
    for f in a.flags:
        cnt[f] += 1
names = {v: k for k, v in vars(F).items() if isinstance(v, int)}
print({names.get(k, str(k)): v for k, v in cnt.most_common(6)})

draws = [a for a in acts if F.Drawcall in a.flags]
print("draw calls:", len(draws))
for a in draws[:5]:
    print("  eid=%-5d %s" % (a.eventId, (a.customName or "?")[:60]))

# dump the last draw's output target to PNG
last = draws[-1]
ctrl.SetFrameEvent(last.eventId, True)
out = ctrl.GetPipelineState().GetOutputTargets()[0]
ts = rd.TextureSave()
ts.resourceId = out.resource
ts.mip = 0
sm = rd.TextureSliceMapping(); sm.first = 0; sm.count = 1
ss = rd.TextureSampleMapping(); ss.first = 0; ss.count = 1
ts.slice, ts.sample, ts.destType = sm, ss, rd.FileType.PNG
print("save:", ctrl.SaveTexture(ts, "/tmp/rdc_lastout.png"))

ctrl.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
```

Useful controller methods beyond the above: `GetBuffers()` /
`GetBufferData()`, `GetShader()` + `DisassembleShader()`, `FetchCounters()` /
`EnumerateCounters()`, `GetDebugMessages()`.

### scripts/rdc.py (capture inspector)

A CLI around the replay API for the everyday workflow — extract output
images without opening the GUI:

```bash
scripts/rdc.py list                          # pass groups + their output targets
scripts/rdc.py dump <event-id>               # raw event id, e.g. 241
scripts/rdc.py dump --last                    # output of the frame's last draw call
scripts/rdc.py dump --all                     # every pass group's color output
scripts/rdc.py clean --keep 1 --dry-run       # .rdc files are ~400 MB+ each
```

It defaults to the newest `.rdc` in `/tmp/RenderDoc`; dumps land in
`/tmp/rdc-dump/`.

**Pass labels:** the engine wraps its passes in `Diligent::ScopedDebugGroup`
(`DiligentRenderer.cpp draw()`): `shadow`, `player`, `taa_resolve` (the
DiligentFX TAA groups nest inside it), `gui` (only when the ImGui pass is
active), `rmlui`. So `list` shows them and `dump <name>` works by pass name.
Two exceptions: the `shadow` pass writes a DEPTH atlas (its cascade targets
are depth attachments — `GetOutputTargets` reports no color outputs, so check
it in `qrenderdoc` or via raw event ids); and the `gui` group is absent from
headless/menu captures where the ImGui pass is inactive.

What a frame looks like (frame 500, 2880x1627 — the TAA "compute" runs as
fullscreen draws; eids are illustrative, they move with the pass set — the
the old map world's `terrain`/`props` passes were removed 2026-09-10 with it):

| eids    | group         | what                                                       |
| ------- | ------------- | ---------------------------------------------------------- |
| 16–158  | `shadow`      | CSM cascade shadow maps (depth atlas)                      |
| 163–170 | `player`      | glTF character (PBR)                                       |
| 172–228 | `taa_resolve` | TAA chain → SRGB backbuffer (DiligentFX sub-groups nested) |
| 230–308 | `rmlui`       | HUD on top of the SRGB target                              |

## API gotchas (this v1.46-dev module)

- **Recurse the action tree.** `GetRootActions()` returns only top-level
  entries; draw/dispatch actions live in `children` under
  `BeginPass`/`CommandBufferBoundary`/`PushMarker` nodes.
- **Result objects, not bools.** `CaptureFile.OpenFile()` and
  `OpenCapture()` return `ResultDetails` — check `r.OK()` / `r.Message()`.
  `SetFrameEvent()` returns void (None) in this build.
- **Names:** `ActionDescription.GetName()` takes an `SDFile` out-param in this
  version — just use `.customName` (the Vulkan debug label, e.g.
  `vkCmdDrawIndexedIndirectCount(<0>)`).
- **`SetFrameEvent(eid, force)`** then **`GetPipelineState()`** (no args) —
  the state call is relative to the _current_ event.
- **`GetTextures()` is on the controller** (no args; returns every texture in
  the capture). Fields are lowercase (`arraysize`, `mips`, `cubemap`); the
  format name comes from `t.format.Name()`. `ResourceId` exposes no field
  accessors — key on `str(rid)` (`ResourceId::157`).
- **`SaveTexture(saveStruct, path)`** — two args; `slice`/`sample` need
  `TextureSliceMapping`/`TextureSampleMapping` objects, not ints.
- **Pass outputs:** render targets are only reported in _draw_ pipeline
  state — not at `vkCmdBeginRendering` (post-clear), dispatch, or EndPass
  events.
- **Float outputs** (R16F G-buffer, HDR) are converted 0–1 → 0–255 on save,
  so HDR images look dark; that's expected, not a bug.
- Shutdown order matters: `controller` → `capture file` → `ShutdownReplay()`
  (skipping it aborts with a double-free).
- **`GetConstantBlocks(stage)`** needs the `ShaderStage` arg and returns
  `UsedDescriptor`s (`.descriptor.resource`, `.access`), not constant blocks;
  pair with `GetBufferData(rid, 0, 0)` to read the raw bytes.
- **`GetShader(stage)` returns a `ResourceId`**, not a shader object —
  reflection comes from `ps.GetShaderReflection(stage)` (fields incl.
  `readWriteResources`, `readOnlyResources`, `constantBlocks`, `samplers`).
- **Get `ResourceId`s from `GetTextures()` / `GetBuffers()` objects** and key
  on the int in `str(rid)` (`ResourceId::157`) — there is no
  `(type, index)` constructor in this build.
- **`de.type` on a `UsedDescriptor`'s descriptor is a raw int** in this SWIG
  build (no `.Name()` on it); `TextureDescription.format` DOES have `.Name()`.
- Dumping 3D textures and cubemaps works through the slice mapping —
  z-slice for 3D, `first=0, count=6` for the six cube faces.

## Environment variables

| Variable                                     | Read by             | Effect                                                                                         |
| -------------------------------------------- | ------------------- | ---------------------------------------------------------------------------------------------- |
| `ENGINE_RENDERDOC_CAPTURE=1`                 | game (debug, Linux) | arm the in-process `TriggerCapture()`                                                          |
| `ENGINE_RENDERDOC_CAPTURE_FRAMES=N`          | game                | frame at which the trigger fires (default 30)                                                  |
| `ENGINE_RENDERDOC_DIR=/prefix`               | game                | capture filename prefix (default `/tmp/RenderDoc/c-game`)                                      |
| `ENGINE_RENDERDOC_LIB=/path/librenderdoc.so` | game                | override which lib to `dlopen` for the API                                                     |
| `LD_PRELOAD=.../build/lib/librenderdoc.so`   | dynamic loader      | map RenderDoc early (set by `run.sh renderdoc`)                                                |
| `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`          | Vulkan loader       | load the implicit capture layer (**required for hooks**)                                       |
| `DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46=1`    | Vulkan loader       | force-disable the layer                                                                        |
| `SDL_VIDEO_BACKEND=x11`                      | SDL                 | forced by `run.sh renderdoc` — the layer build is wayland-OFF and the surface path is X11-only |

## Troubleshooting

| Symptom                                                                           | Cause / fix                                                                                                                                                                                                                   |
| --------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `renderdoc: not present`                                                          | Lib not mapped — run via `./scripts/run.sh renderdoc` (or set `LD_PRELOAD` yourself)                                                                                                                                          |
| `renderdoc: api ready` + `TriggerCapture` but no `.rdc` appears                   | Layer not active: `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` missing; or the trigger frame landed after process exit; check `/tmp/RenderDoc` and `RenderDoc_app_*.log` there                                                         |
| `diligent: Required extension VK_KHR_wayland_surface is not available` + segfault | Update of the Diligent checkout lost the wayland-define strip — re-run `cpp-thirdparty/diligent/build.sh`; also make sure the run went through `run.sh renderdoc` (`SDL_VIDEO_BACKEND=x11`). See `docs/lessons/2026-09-07.md` |
| Capture has 0 draw calls                                                          | Triggered during asset loading — raise `ENGINE_RENDERDOC_CAPTURE_FRAMES`                                                                                                                                                      |
| `qrenderdoc` not found                                                            | `~/.local/bin` symlink missing — recreate from `build/bin/`                                                                                                                                                                   |
