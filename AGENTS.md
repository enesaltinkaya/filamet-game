Use renderdoc to debug graphical issues.
Do not use comments in the code.
Do not use "find / ...".
Do not use git.
Always use export ENGINE_HIDDEN_WINDOW=1 env variable.
Always use export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json env variable.
Use export ENGINE_AUTOTEST=enter env variable if not working on main menu.

### File locations

Everything related to project is either here /media/extra/Projects/c/filament-game or in thirdparty directory /home/enes/Projects/c/cpp-thirdparty.

Set TERM env variable is to run the game.

### Screenshot feature

`ENGINE_SCREENSHOT=path` (env var) makes the engine capture one frame and save it as a JPEG (quality 90, via stb_image_write in c-engine/renderer/Renderer.cpp) a few frames after startup, for automated runs. One-shot — only the first capture is written.

Example: `ENGINE_SCREENSHOT=/tmp/shot.jpg ./build/c-game/c-game` — save screenshots to `/tmp/` to keep the project folder uncluttered.

### RenderDoc (frame capture & inspection)

For any work involving RenderDoc, frame captures, or GPU debugging, read
`docs/renderdoc-capture.md` first. It documents the local v1.46-dev build,
the layer+preload setup, the Python replay module, and `scripts/rdc.py`
(headless pass-output dumper: `scripts/rdc.py list`, `scripts/rdc.py dump <eid|--last>`).

```bash
# Headless capture; .rdc lands in /tmp/RenderDoc/ (~400 MB each, clean up old ones)
ENGINE_RENDERDOC_CAPTURE=1 ENGINE_RENDERDOC_CAPTURE_FRAMES=300 \
./scripts/run.sh renderdoc      # exits on its own after the capture fires
qrenderdoc /tmp/RenderDoc/c-game_frame300.rdc           # GUI
PYTHONPATH=/home/enes/Apps/renderdoc/build/lib python3 <script>  # headless replay API
```

- `run.sh renderdoc` sets `LD_PRELOAD` **and** the implicit layer (`ENABLE_VULKAN_RENDERDOC_CAPTURE=1`) — both are required, since volk's dlopen/dlsym bypasses plain symbol interposition. It also forces `SDL_VIDEO_BACKEND=x11` (the layer build is wayland-OFF).
- The trigger is frame-based (`ENGINE_RENDERDOC_CAPTURE_FRAMES`), not delay-based; pick a frame past asset loading. The engine exits on its own two frames after the capture fires (`ENGINE_LOG_TIMEOUT` is only a safety net).
- Passes are labeled with `Diligent::ScopedDebugGroup` in `DiligentRenderer.cpp draw()`: `shadow`, `world` (nested: `terrain`, then `player` — both drawn through the shared PBR world pass; `props` will nest here when it lands), `taa_resolve`, `gui`, `rmlui` — so `rdc.py list` / `dump <name>` work by pass name (nested groups match too). (The `shadow` group has no color outputs — depth atlas; use qrenderdoc for it.)
- The Diligent wayland-define strip lives in `cpp-thirdparty/diligent/build.sh` — after updating Diligent, re-run it before renderdoc runs.

### Old engine

We will be porting our old engine /home/enes/Projects/c/game-001-cpp to this new engine.

### Render path

We are only using diligent engine.
Sources are here with samples and docs;
/home/enes/Projects/c/cpp-thirdparty/diligent

### Lessons

Index: `docs/lessons.md`; full entries in `docs/lessons/<date>.md` (active engine) and `docs/lessons-filament-archive.md` (Filament-era, backend removed 2026-09-05). After a multi-hour debugging session, add a rule-first entry to the dated file: rule + diagnostic fingerprint (VUID id, error string, measured signature) + one-line incident — no verification logs once the fix is proven. Read the index before fighting renderer/texture/buffer weirdness — known pitfalls (e.g. Diligent dynamic-buffer ring clobbering) are listed there.
