# Lessons

Dated log of hard-won debugging knowledge. One entry per incident, rule first.

---

## 2026-09-05 — Player moved ~2x faster at a 120fps cap than at 60: `utils::timer.dt` is the fixed 1/UPS tick, not the frame's dt, but the ported `ecsUpdate()` ran every system once per rendered frame

**Rule:** `utils::timer.dt` (1/UPS, UPS=60) is the fixed simulation tick length and is only valid inside the fixed-step loop. Any system advancing state by `timer.dt` must be invoked through `utils::timerUpdate(...)` (the accumulator), never once per rendered frame. The converse: passes that run once per frame (GUI postUpdate etc.) must use `utils::timer.frameTime` (already clamped to 250 ms), never `timer.dt`. Consequence: simulation speed is fps-cap independent — at 120 fps the simulation ticks at 60 Hz (one substep every 2nd frame), at 60 fps it ticks 1:1.

**Incident:** "when vsync is off and fpslimit is 120, player character moves faster than when it was 60fps capped". The port dropped the old engine's wrapper: old `Ecs.cpp` had `ecsUpdate() { utils::timerUpdate(ecsUpdateForTimer); }` (comment: "might not run every frame, might run multiple times per frame"); the new one looped systems directly, once per frame. `timer.dt` is set once in `timerInit` and never reassigned, so at 120 fps every `timer.dt`-advancing system (player via joltCharacterUpdate, fly camera, model turn, anim blending) advanced 1/60 s twice per 1/60 s of wall time = 2x speed. The timer's accumulator/alpha machinery was left as dead code in the new Timer.cpp — the trace of the dropped call.

**Fix:** `c-engine/ecs/Ecs.cpp` — restored `ecsUpdateForTimer` + `ecsUpdate` → `utils::timerUpdate(ecsUpdateForTimer)`; `cpuElapsed` now accumulates across substeps and is snapshotted into `cpuElapsedLastFrame` once per frame (per-system stats would otherwise flicker as 0/tick/0…). `c-engine/gui/GuiManager.cpp` — ImGui's NewFrame now gets `timer.frameTime/BILLION` (gui postUpdate runs once per frame; with `timer.dt` menu animations would have run 2x at 120 fps). Verified headless with a temporary tick counter (removed after verification): at the 120 fps cap, render ≈114–120 Hz while sim ticks landed exactly 60 per 120 frames across consecutive 120-frame windows (60 Hz sim); the same accumulator gives 1 tick per frame at 60 fps. Screenshot boot runs clean.

**Note:** `timer.alpha` (substep phase) is computed for render interpolation but nothing consumes it yet — positions snap between the 60 Hz ticks, which is what the old engine also did.

---

## 2026-09-05 — audio settings gui flush in `removed()` segfaulted in `SoLoud::play` at shutdown: gui `removed()` runs after lower-priority systems are destroyed

**Rules:** (1) `ecsDestroy` runs systems in LIST order (priority ascending), so a gui's `removed()` (the rmlui guis die inside `guiManagerRmlUi::removed()`, priority 4) runs AFTER `soundSystem::removed()` (priority 3) has destroyed SoLoud — never play a sound from a gui's `removed()` without knowing the audio stack is still up. (2) `utils::settingsWrite()` emits `settingsSaved` to ALL subscribers regardless of teardown state — a signal handler that touches a destroyed resource (SoundSystem's handler did `Soloud_setVolume` on the freed handle) must null-guard, because settings writes happen at shutdown too (the audio gui persists its last slider change in `removed()`).

**Incident:** the new audio settings gui (old engine's futureTask debounce became a timestamp flush in `update()`, with a safety flush in `removed()`) segfaulted in `SoLoud::Soloud::play` whenever it was open at app exit: `GuiManagerRmlUi::removed → settingsAudioGui.removed → flush → soundPlayClick` on the destroyed audio stack. The `settingsSaved` crash one line later in `flush` was found by reading, not by crashing.

**Fix:** `SettingsAudioGui::flush(soundFeedback)` — the in-run debounce path plays the feedback click, the `removed()` flush persists silently; `SoundSystem::settingsSaved` returns early when the SoLoud handle is gone.

**Verification:** `ENGINE_AUTOTEST=settings ENGINE_SETTINGS_AUTOTEST=audio ENGINE_SCREENSHOT=...` (audio page open at screenshot/exit) exits clean with all four guis removed in order (pre-fix: SIGSEGV); `ENGINE_AUDIO_SETTINGS_AUTOTEST=effects50` persists effects=50 to settings.json with the label/slider updating, `=close` re-shows the settings page.

---

## 2026-09-05 — RMLUI gui queue: `guiManagerAddGuiNextFrame` called from inside a gui's `added()` (menu ENTER WORLD runs inside the queue-application loop) reallocated `pendingAdds` under the range-for's captured `end()` — dangling iterator, SEGV on `gui->name`; and the post-loop `clear()` would have silently dropped the queued items

**Rules:** (1) Never iterate a queue while the body can enqueue into it — `for (x : vec)` captures `begin()`/`end()` once, so a `push_back` that grows past capacity reallocates the buffer and the captured `end()` dangles; the loop then marches into freed/reused heap and dereferences garbage (`gdb` showed the vector's contents intact while the loop variable held `0x4ec20f5297f20952`). The fix is to iterate a copy and clear the live queue BEFORE the loop: items queued during the loop land next frame (the documented contract) instead of dying in a post-loop `clear()`. This engine's `GuiManagerRmlUi::postUpdate` is the first place the pattern was hit: `MainMenuGui::added()` (itself running inside the adds loop) fires the ENTER WORLD action, which queues camera/player gui adds + the menu's remove. (2) The interactive lua `onclick` path (menu button) does NOT hit the loop — it fires inside `rmlUpdate()`, after the queue was applied — so a bug that only reproduces under `ENGINE_AUTOTEST=enter` (or any queued-during-add action) is still a real bug, and the headless autotest is the canary for it. (3) `ENGINE_LOG_TIMEOUT` is in MILLISECONDS (`atof(env) * MILLION` ns) — passing seconds makes the engine quit ~1 frame after a blocking world load, which masquerades as "my newly deferred gui never showed" (its applying frame simply never ran); pair it with a low `ENGINE_SCREENSHOT_FRAME` when the world load eats frame 0. (4) Headless screenshot runs must pin `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json` like `run.sh` does: unpinned, Vulkan picks the NVIDIA 5090 here and the whole frame renders pure black (0,0,0) at ~5 fps — pixel-sample a "black" screenshot before suspecting the render passes.

**Incident:** porting the old engine's rmlui camera/player gui (replacing the interim ImGui camera window) crashed with `ENGINE_AUTOTEST=enter`: SIGSEGV in `utils::debug("rmlui: showing %s", gui->name)` with a garbage `gui`, `gdb` showing `pendingAdds` = {showFps, cameraGui, playerGui} intact while the iterator read freed heap.

**Fix:** `GuiManagerRmlUi::postUpdate` — copy `pendingAdds` into a local and `clear()` the live vector before the adds loop; the removes loop still iterates live `pendingRemoves` so in-loop cancels (an add erases a pending remove of the same gui) keep working.

**Verification:** `ENGINE_AUTOTEST=enter ENGINE_SCREENSHOT=... ENGINE_SCREENSHOT_FRAME=60 ENGINE_LOG_TIMEOUT=45000` + radeon ICD: `rmlui: showing cameraGui` / `showing playerGui` on frame 1, both documents rendered (Player box at `right:270dp` with live `Cell:` id, Camera box bottom-right with pos+quaternion), clean `RML SHUTDOWN` teardown, exit 0.

---

## 2026-09-05 — FPS GUI first line garbled + terrain flickering dark navy, both only with RMLUI enabled: `UpdateBuffer` on a VIRTUAL dynamic buffer blind-copies cbuffer data into dynamic-heap OFFSET 0 — an untracked, non-exclusive region that collides with whatever ring allocation parks there (the RMLUI vbo's, in steady state)

**Rules:** (1) In this Diligent build (release), `UpdateBuffer` on a *virtual* dynamic buffer (USAGE_DYNAMIC cbuffer/VBO/IBO with no real backing buffer) issues `vkCmdCopyBuffer` with **dst = the shared dynamic-heap buffer at the UpdateBuffer's dstOffset** — the data lands at a hardcoded heap offset, NOT an allocator-tracked exclusive region. Signature in the log: `VUID-vkCmdCopyBuffer-dstBuffer-00120` (dst "Dynamic heap buffer" missing TRANSFER_DST_BIT) firing **every frame** — that VUID is the fingerprint of a copy targeting the heap itself. The DEV-only guard ("Dynamic buffers must be updated via Map()") exists precisely because this path is broken in release. (2) The dynamic heap is one 8 MB ring (256 KB pages, fence-gated release); `MapBuffer` regions come from it, and when per-frame sizes repeat, steady state parks the SAME regions at the SAME offsets every frame. If a ring page sits at heap offset 0 (it does — the RMLUI vbo's 256 KB page), the blind heap[0] cbuffer write collides with that region. Who clobbers whom is decided by in-frame ordering: the world cbuffer GPU copies (recorded before each world draw) restore heap[0] for the terrain/props draws, but the UI VBO's first ~1 KB (the first text line's staged vertices) is read AFTER those copies, so line 1 renders cbuffer floats (garbled), while lines 2+ (past the cbuffer's ~1 KB) stay clean; in other ring phases (different window size / allocation history) the cbuffer reads back another pass' staged bytes instead — garbage viewProj + light data collapses the terrain into a flat dark-navy patch over the whole ground (a DRAWN (0,15,30), not the (39,56,84) clear — pixel-sample the "dark" against the clear colour before blaming clear/blend). `ENGINE_NO_RMLUI` hides it because dropping the vbo/ibo pages moves which ring page parks at 0. (3) The safe per-frame cbuffer pattern — already used by the glTF pass — is `MapBuffer(MAP_WRITE, MAP_FLAG_DISCARD)` + memcpy + `UnmapBuffer` (GraphicsTools `MapHelper.hpp`): an exclusive per-frame ring allocation, and the draw binds at the map offset. The SRB commit (which writes the dynamic descriptor offset) must run AFTER the map in the frame — a stale/zero offset is exactly the old "coherent-but-wrong matrix from neighbouring struct fields" symptom the HeightmapTerrainDiligent comment recorded as a 112-byte shift (that incident was this same collision family, misdiagnosed at the time, and the workaround — UpdateBuffer — is what produced this bug). (4) Diagnosis toolkit that worked: log `MapBuffer` pointers relative to the first observed one per buffer (ring steady state jumps out in 3 frames); multi-frame screenshot probe (temporary `ENGINE_SCREENSHOT_FRAMES`, removed after use) to rule transient clobber in/out across a long run; per-frame VUIDs to locate blind heap copies; pixel sampling to distinguish "drawn dark" from "clear showing".

**Incident:** "first line of the fps gui is glitched; terrain (ground) is dark navy and flickers in that dark colour — both vanish with ENGINE_NO_RMLUI." The dark ground was first suspected to be the clear bleeding through; pixel sampling disproved it (dark = (0,15,30), clear = (39,56,84)) — it was the terrain drawn with a corrupted cbuffer (garbage matrices/light → flat dark patch, props and character on top fine because their own cbuffer updates re-ran after the clobber in that phase). A map-offset probe showed the RMLUI vbo region parked at heap offset 0 from frame 3 on (ibo at 0x40000), and the per-frame dstBuffer-00120 VUID showed the terrain + props cbuffer `UpdateBuffer` copies writing heap[0] every frame — the two passes sharing one untracked region.

**Fix:** `c-engine/renderer/diligent/HeightmapTerrainDiligent.cpp` and `PropsRenderDiligent.cpp` — `fillFrameAttribs` now uses `MapHelper<HLSL::PBRFrameAttribs>` (MapBuffer DISCARD + write + unmap on scope exit), exactly like `GltfDiligent.cpp`'s fill; the `frameAttribsStaging` statics are gone and no `UpdateBuffer` calls remain in the codebase. Every per-frame dynamic resource (gltf/terrain/props/rmlui cbuffers, rmlui vbo/ibo) now lives in an exclusive ring allocation; nothing writes an untracked heap offset anymore.

**Verification:** 15-shot multi-capture run (frames 60–900, RMLUI on): UI line 1 clean ("fps: 60.00") in every shot; ground uniformly beige (avg ~131,126,104), dark-frame fraction 0.000 in all 15 shots (pre-fix: garbled line 1 every frame, dark frames in user sessions); `dstBuffer-00120` gone from the log. Note: the two transient `VUID-vkCmdBindDescriptorSets-00358/00359` pairs (once at world entry, one-time SRB rebuild) predate this fix and are unrelated. Temp probes (`ENGINE_RML_MAP_PROBE`, `ENGINE_SCREENSHOT_FRAMES`) removed with the fix.

---

## 2026-09-05 — RmlUi static text turning BLACK after ~1 s (showFps gui): the geometry pool's deferred-release queue stored SLOT INDICES but the consumer subtracted 1 again — every ReleaseGeometry(H) silently freed the NEIGHBOUR's slot (handle H−1); plus "static text goes black" = the font-effect glow layer still rendering with the base layer's geometry dropped

**Rules:** (1) A deferred-release queue must store the SAME unit its consumer expects — handle or slot index, never mix. `rmlPassReleaseGeometry` pushed `handle−1` (slot) and `releaseGeometryNow` did `idx = handle−1` again, so releasing handle H freed pool slot H−2 = handle H−1's slot, while the released geometry's OWN slot stayed allocated (leak + neighbour kill). (2) Symptom signature: SOME rmlui text elements render as soft black blobs after the first data-model update, on a document with a `font-effect:` (glow/shadow) — that is the EFFECT layer (black vertices) still rendering while the BASE layer's compiled geometry was dropped by the backend's geometry pool; the surviving black shapes are the glow silhouette. Elements that never recompile keep working; each value-text recompile kills one more neighbour (the corruption "spreads" across static texts over time). (3) A render backend dropping geometry (handle not `inUse`) must WARN, never silently return — a silent drop looks like a shader/blend/buffer bug and costs hours of GPU-side barking up the wrong tree. (4) Probe ladder that cracked it: `ENGINE_RML_PROBE` cmd dumps show the missing commands never reach the GPU queue (rules out buffers/shader/blend — no RenderDoc needed for the diagnosis); instrumenting RmlUi core (`RenderManager::Render`, `FontFaceHandleDefault::GenerateString`, `ElementText::GenerateGeometry`) then splits "RmlUi stopped rendering it" (mesh_list shrank / skip) from "backend dropped it" (DROP line while RMLDBG Render fired) — here RmlUi rendered the handle every frame from compile until shutdown; only the backend's slot bookkeeping was wrong. (5) The thirdparty rmlui build tree may hold a STALE CMakeCache from an old path (`/home/enes/project/...` vs `/home/enes/Projects/...`) — `ninja` fails with a source-dir mismatch; wipe `build-linux` and reconfigure with the args from `rmlui/build.sh` (freetype/lua paths, `-DRMLUI_LUA_BINDINGS=ON -DRMLUI_CUSTOM_RTTI=ON`, Release flags). When instrumenting-and-reverting thirdparty sources, revert with targeted edits and re-verify the exact statements came back (a greedy regex once swallowed the whole `if (!texture_layout.GenerateLayout(...)) return false;` — instant startup SIGSEGV in the next run).

**Incident:** "top-left fps display: some text is black. correct at first, all white, after a second or so some get black." Frame-30 and frame-400 screenshots showed labels `frame:`/`cpu:` and later the static gpu value `0.00 ms` as black blobs while their updating values stayed white. Probe: backend cmd dumps showed the affected texts' WHITE-vertex meshes (base font layer, tex 5) absent from the command queue while their BLACK-vertex glow meshes (tex 4) rendered; no pool-exhaustion warning, no rmlui texture regeneration ever (`version=0` forever — default-glyph sets cover the digits, so new characters never dirty the font face). RmlUi-side instrumentation proved `RenderGeometry(handle)` was called every frame for the missing meshes while the backend's pool slot read `inUse=0` — and no `ReleaseGeometry` for that handle existed until shutdown. First data update (0.5 s) released value meshes 14/15/18/19/22/23; the double-decrement freed slots 12/13/16/17/20/21, killing the labels' base handles 13/17/21 (and, as more updates recompiled more values, eventually the gpu row's static meshes). Backend handles also print one lower than RmlUi handles in probes — `cmd.geometry` is the slot (`handle−1`), not a second bug.

**Fix:** `RmluiDiligent.cpp` — `rmlPassReleaseGeometry` pushes the raw handle; `releaseGeometryNow(handle)` is the single place converting handle→slot; `rmlPassRenderGeometry` warns on a free-slot render instead of dropping silently. Thirdparty `librmlui.a` rebuilt back to pristine source.

**Verification:** `ENGINE_SCREENSHOT_FRAME=30` and `=400` runs — all four rows white (gpu value green, `color:green` inline style), glow intact, 0 "free slot" warnings; pre-fix baselines (`/tmp/fps_late_crop.png` black `frame:`/`cpu:`/`0.00 ms`) vs post-fix (`/tmp/fps_fixed_*_crop.png`).

---

## 2026-09-05 — Porting the old engine's RmlUi GUI (crmlui C wrapper) onto the Diligent render path: a stack-local `RmlParams` SIGSEGV'd inside `rmlRenderVulkan`, and a "Vulkan NDC" ortho rendered every document upside-down

**Rules:** (1) `crmlui` stores the POINTER to your `RmlParams` (`RenderInterface_VK(rmlParams)`; `rmlParams = rmlParamsInc`, no copy) — the params storage must outlive every `rmlUpdate`/`rmlRenderVulkan`/`rmlDestroy` call: make it a member of the owning System, never a local in `added()`/init. The file/log/SDL handles in `RmlParams` copy their members, but only the `vulkan` callback block dangles, which makes the crash appear deep inside RmlUi (`endFrame` = `mov rax,[rdi+8]; jmp *0xe8(rax)` with a garbage function pointer) rather than at the obvious place. (2) The UI ortho must match the old engine's GLM y-down convention — `glm_ortho(0, W, H, 0, ...)` (i.e. `_22 = -2/H, _42 = +1`) then TRANSPOSED per the Diligent row-vector rule; a "correct" Vulkan y-down ortho renders the document upside-down. This RmlUi version never calls `setTransform` (transform stays identity), so the ortho IS the only orientation knob. (3) Build ONE shader resource binding set PER UI texture; in-place updates of a mutable texture SRV (UPDATE_AFTER_BIND doesn't exist in this Diligent) invalidated the whole command buffer and produced blank UI. (4) The rmlui draw pass must own its `vkCmdBeginRendering`/`vkCmdEndRendering` scope: the preceding ImGui pass's Flush+InvalidateState leaves no active render pass; use `gp.DSVFormat = TEX_FORMAT_UNKNOWN` (color-only), set viewports per scope, clamp scissor to the render area. (5) `rmlDestroy()` dereferences the Lua state (`~LuaEventListener`), so the LuaSystem must not close its state in `removed()` — `luaDestroy()` runs after `ecsDestroy()` (old-engine order). (6) Lua 5.4 headers have no `extern "C"` guard: include them in `extern "C"` from C++ or the linker demangles the undefined references and masks the cause.

**Incident:** round 3 smoke run (first live run of the new RmluiDiligent pass) crashed after a few frames with PC in NX'd BSS; gdb + core showed `rmlVKHandle->rmlParams` = a stack address from `added()` — the reused stack slot for `endFrame`'s vtable slot held garbage (the `beginFrame` slot wasn't clobbered yet, so it had been "working"). After the lifetime fix, the document rendered but upside-down; an A/B/C/D screenshot matrix exonerated the transposed cbuffer, the 1-float colour input element, and the sRGB RTV format, and isolated the ortho `_22` sign.

**Fix (all kept):** `RmlParams` as a `GuiManagerRmlUi` member (smoke system had its own member before retirement); GLM y-down ortho + transpose in `RmluiDiligent.cpp`; per-texture SRBs; own render scope with unknown DSV; `luaDestroy` moved to `Engine.cpp` after `ecsDestroy`.

**Verification:** `DISPLAY=:0 ENGINE_SCREENSHOT=... ENGINE_DEBUG_GUI=1` runs show all four manager-driven guis (showFps/debug/passstats/stats) rendering upright with live data-model values; clean `RML SHUTDOWN` teardown. Known stubs: per-pass GPU times (no Diligent equivalent) report 0.

---

## 2026-09-05 — The props (vegetation) pass on Diligent: frustum planes extracted from the WRONG matrix orientation culled every tile (0 draws, no error), and a zeroed `DrawIndexedAttribs.FirstIndexLocation` silently drew range 0's sub-mesh for every other range — giant grass-cards + conifer geometry rendered under the deciduous range's instance data

**Rules:** (1) Stored Diligent matrices use the ROW-VECTOR convention (the shader computes `clip = mul(v, S)` = Sᵀ·v, matrix TRANSPOSED at fill per the 2026-09-05 transpose lesson). Therefore the Gribb-Hartmann clip-plane coefficient vectors are the memory COLUMNS of `view*proj` (`m[k*4 + col]`, NOT `m[col*4 + k]` — the latter reads row `col`, which looks plausible because a view–projection matrix's rows also resemble planes and the normalization hides scale): left = col3+col0, right = col3−col0, bottom = col3+col1, top = col3−col1, near = col2 (D3D-style NDC z ∈ [0,1]; `w+z` for GL), far = col3−col2. Verify: side planes of a rotation-only view MUST pass through the eye (w = 0) and the near plane's w must equal −near (0.1). (2) The planes live in CAMERA-RELATIVE space (the view is rotation-only), so any AABB tested against them must be camera-relative too — absolute world AABBs at |x| ~ 4e4 m are ~14 km outside every plane and cull the whole pass with zero errors or warnings. (3) In a multi-range instanced draw, EVERY range-dependent draw argument must come from the range: `FirstIndexLocation = range.indexOffset`, `FirstInstanceLocation = range.start`. A zeroed FirstIndexLocation renders range 0's geometry (indexOffset 0 is the common case — grass cards) under every other range's instance data: the symptom is species A's mesh at species B's positions/scales (giant crossed cards + conifer geometry under the deciduous range), NOT a missing draw — and ranges whose indexOffset happens to be 0 render perfectly, which hides the bug. (4) glslang-HLSL `SV_IsFrontFace` must be declared `bool` in the PS input; `uint` compiles but fails spirv-val (VUID-FrontFacing-FrontFacing-04231) while the shader still loads.

**Incident:** the ported azgaar props pass applied tiles (18 MB GPU) but drew nothing; after fixing the plane extraction, trees appeared at the aerial vantage while the ground view showed giant flat green sheets + conifer geometry scattered among correct grass. Probes that mattered: a one-shot ENGINE_PROPS_DEBUG log (tile count, per-range start/count/indexOffset/aabb), CPU-side dump of the packed instance texels + merged mesh (both clean — which moved suspicion to the draw path), and a MAGENTA debug albedo (`f4ExtraData` flag → every fragment from the pass magenta) which proved the sheets+pines WERE the pass — the per-instance data was right and the only per-range GPU argument still wrong was FirstIndexLocation. The earlier "0 draws" had also been misread as a perf-log artifact: the game-side log reads `statDrawsThisFrame` before the render phase resets it, so it shows the previous frame's count.

**Fix (all kept):** `extractFrustumPlanes` reads memory columns; tile/range AABBs are tested camera-relative (`aabb − camPos`); `attrs.FirstIndexLocation = r.indexOffset`; PS input `bool FrontFacing : SV_IsFrontFace`. Verification: `ENGINE_CAMERA=props` oblique + `propsground` 7 m eye-height shots match the phase-7 filament acceptance frames (individually readable trees, grounded trunks, per-instance hue/scale variation, dense grass undergrowth); 100 m/s dolly streams (claims/rescatters/evictions tick, queue drains, 0.2 ms pass cost, 14–19 MB GPU steady).

---

## 2026-09-05 — A single-key glTF channel whose value differs from the node's static TRS is a legit POSE HOLD (eve's run clip holds curled fingers one-key-per-bone) — flattening constants to rest (the 2026-09-04 singlekey-fix) straightened the hands, and the "0.01 head scale in the asset" was never in the source: it was gltf-standardize.py writing a Hips accessor IN PLACE that the exporter had shared with Head/LeftHand/\*4 channels

**Rule:** a 1-key sampler is a constant and must be sampled + applied every frame (glTF semantics; Mixamo/Blender collapse constant tracks to one key — that's how clips hold a pose: eve_run1's fingers curl 30–42° at the MCP with ONE key per finger bone). Never rewrite constant channels to the node's static TRS. The 2026-09-04 "constant channels corrupt the pose" lesson was misdiagnosed: the 0.01 (cm-factor) scale constants on Head/LeftHand/\*4 were NOT authored data — the Blender exporter dedupes identical constant values into a SHARED accessor, and gltf-standardize.py rewrote the Hips scale accessor in place (×0.01), silently converting every other channel sharing it ("1.0" constants on a dozen bones, across all 20 clips) into 0.01. Fix shared-storage rewrites with copy-on-write (fresh accessor per rewritten channel), not by neutralizing the data. And when a runtime "skips single-key samplers" (Diligent's loader does; it had become our engine's behaviour too), any pose held by a constant silently collapses to rest — the skip is only safe when the constant equals the static TRS.

**Incident:** "in the original animation (Blender) the fingers are not straight, but in the game they are." Diagnosis ladder: (1) the packed animations.zstd run clip's finger channels were 1-key and EQUAL to static → the curl wasn't in the shipped data (Blender's animations.blend still had it: 26–42° constant per finger bone); (2) export-models.sh log showed `singlekey-fix: 3000 constant channels set to static pose` — the 2026-09-04 step had flattened the curl to rest, and clipSampleTRS's 1-key skip then kept rest; (3) removing that step exposed the 0.01 scale constants — traced to standardize's in-place shared-accessor write (raw Blender export: zero non-1.0 scale channels; post-standardize: 0.01 on Head etc.), which is also why the old singlekey-fix "worked" for the head: it was masking this bug.

**Fix (all kept):** (1) `c-engine/gltf/GltfDiligent.cpp` clipSampleTRS applies 1-key samplers as constants (`idxB = idx` alias keeps the lerp/slerp in-bounds; only 0-key samplers are skipped); (2) `scripts/gltf-standardize.py` copy-on-write — each rewritten channel gets a fresh bufferView+accessor (`alloc_accessor`), and the grown BIN chunk updates `buffers[].byteLength` (the 2026-09-04 gltfpack "buffer too short" rule); (3) `scripts/gltf-singlekey-fix.py` deleted and its step dropped from `scripts/export-models.sh`; (4) eve.zstd + animations.zstd re-exported (stamps in scripts/.tmp/ cleared) and pak_1 repacked.

**Probe:** dump the packed glb (zstd -d, parse JSON chunk) and compare every 1-key channel to its node's static TRS per path — rotation diffs = legit holds (341 across the clips, 30–42° on the fingers), SCALE diffs = corruption (was 82, now 0; translation diffs must be explainable pose data: eve_t/female_walk Hips offsets). In-game: `ENGINE_CAMERA=character` portrait (idle, fingers must be curled, head full size) and `ENGINE_AUTOTEST=enter ENGINE_AUTO_RUN=1 ENGINE_SCREENSHOT` at frame 400 (run clip, fists clenched — a flat open hand is the regression).

**Status: FIXED (2026-09-05).** Idle portrait shows curled fingers + normal head; auto-run shows clenched fists matching the Blender reference. The old engine's pipeline never had the singlekey step, which is why the original asset looked right there.

---

## 2026-09-05 — the Diligent backend never ported the 2026-09-04 relative-to-anchor rework: at Azgaar cell 0 (x/z ~ 39 km) the f32 absolute camera + placement shimmers the character animation by up to 4 px — fixed with camera-anchored rendering (rotation-only view, f64 anchor-relative placement, split-anchor terrain)

**Rule:** any world coordinate that reaches the GPU in f32 far from the origin sits on a 3.9 mm ULP grid (ULP(39 km) = 2⁻⁸ m); animation deltas (cm-scale) quantize to that grid and shimmer. The only fix is to keep every f32 magnitude SMALL: store the camera in f64, make the view matrix rotation-only (camera = render-space origin), and express every renderable relative to the f64 camera eye — the subtraction happens in double on the CPU (or exact-in-f32 in the shader, see below) and only the small difference is rounded once. Two traps hit while wiring this: (1) an f32 COPY of the anchor reintroduces the ULP step — as the camera moves, f32(anchor) jumps 3.9 mm and the whole scene steps with it; keep the anchor f64 (glTF placement) or split it (terrain: f32 high + sub-mm residual, both subtractions exact). (2) per-instance vertex attributes on the runtime-HLSL/Vulkan input-layout path silently fed nothing (terrain invisible) — on this backend pass small per-frame values through the existing cbuffer (`PBRFrameAttribs.f4ExtraData`), not through new input-layout elements or a second cbuffer (the 2026-09-04 second-cbuffer-ambiguity lesson still applies).

**Incident:** "character is at azgaar cell 0, x/z ~ 39 k, animation jittery" — the exact 2026-09-04 symptom, back on the new engine: the Diligent port had an absolute f32 camera (`cameraLookAt` cast to f32), an absolute-translation view matrix, an absolute-f32 glTF placement translation, and absolute-f32 terrain VBOs — the f64 game side (Jolt `GetPositionD64`, f64 `p.pos`, f64 orbit eye) was already ported and even had the `RenderBackend::worldAnchorX/Z` hook in place, but the backend reported anchor 0 and ignored it.

**Fix (all in c-engine):** `DiligentRenderer.cpp` — `camEye/camCenter/camUp` are f64; `viewMatrix()` returns the basis rotation only (translation row zero), built in double; `worldAnchorX/Z` report the f64 eye; new `diligentWorldAnchor(f64 out[3])`. `GltfDiligent.cpp` — `placementRootMatrix()` derives the root translation as f32(placePos_f64 − anchor_f64) and re-derives when the ANCHOR changes (the anchor moves every frame, so placement-dirty alone is not enough — a static-pose character must track camera moves); `gltfBoundingBoxDiligent` re-absolutizes with the anchor for callers. `HeightmapTerrainDiligent.cpp` — `fillFrameAttribs` puts the anchor split in `f4ExtraData[3]` (f32(anchor)) and `f4ExtraData[4]` (the residual); terrain VS/PS subtract both before the rotation-only view (the high-part subtraction is exact: same binade, Sterbenz; the residual kills the anchor ULP step — remaining per-frame error ~60 µm, sub-pixel). `f4Position` becomes (0,0,0,1) automatically (derived from `viewInv[3]` of a rotation-only view), so the prebuilt PBR shaders (character ViewDir, no shadow path) need no change.

**Probe:** `ENGINE_JITTER_PROBE` now also logs the mesh node's ANCHOR-SPACE position per frame (`jitg: …` in GltfDiligent) — the decisive render-path check: with the bug the value sits at ~3.9e4 m on a 3.9 mm grid; fixed it is a few metres moving at ~30 µm resolution (f32 grid of the small magnitude) even while the camera dolleys (`ENGINE_CAMERA_DOLLY`). `ENGINE_GLTF_DEBUG`'s `pose: root row4` shows the placement translation (small = anchored, ~39000 = not). Screenshots: saved cell-0 state — character AND terrain must be visible (with a rotation-only view an un-anchored renderable at 39 km is beyond the 20 km far plane and simply vanishes — a vanished character is the "anchor not wired" signature); regression shots at the default spawn (28 km) and true origin.

**Status: FIXED (2026-09-05).** Character animation verified smooth at cell 0 (probe data above); terrain/character/mid-field/origin screenshots all coherent. Known residual: terrain VBO corners are still absolute f32 (3.9 mm fixed per-corner offset — static, sub-pixel, no jitter); the old engine's tile-local VBO + per-tile f64 transform would remove even that, but per-instance attributes don't work on this backend's runtime-HLSL input-layout path (see rule). The flying camera and `ENGINE_CAMERA_DOLLY` step the eye in f32 on the game side (slow movers — acceptable); the player's third-person orbit is fully f64.

---

## 2026-09-05 — The Diligent gltf port regressed the 2026-09-04 pivot rule: `placementRootMatrix` pinned the AABB MIN CORNER (`T(-minc) * R(yaw) * T(pos - minc)`), swinging eve's visible body 0.65 m around the orbit target and rendering the player ~10% off-centre of screen

**Rule:** when porting a placement/pivot fix, port the PINNED POINT, not the matrix shape. The 2026-09-04 yaw-pivot rule ("anchor the point that should stay fixed") fixed `gltfPlaceAtFacingFilament` to pin the LOCAL ORIGIN (the feet), but the diligent port documented its contract as "min corner lands at (x,y,z)" and built `T(-minc) * R * T(pos - minc)` — that pins `minc`, so eve's feet (local origin, 0.64 m from her min corner in X) orbit a 0.65 m arc as facingYaw changes, and the third-person camera (which targets feet + 0.70) sees the character drift off-centre, in a direction that rotates with the model yaw (it even looked "fine" at some yaws — the offset cancels when facing aligns with the min-corner vector, exactly the 2026-09-04 "only S looked wrong" trap). Verify a pivot fix with the MODEL YAWED AWAY from the camera, not at the cancelling yaw.

**Incident:** "player character is not centered" screenshots — the character sat ~11% right of screen centre while running. Diagnosis: log the local bounds (`gltf: ... bounds [-0.64 -0.01 -0.13]-[0.64 1.39 0.12]`); the visual centre (local (0, 0.69, 0)) sits at `pos + R(yaw)*(0.64, 0.70, 0.13)`, i.e. 0.65 m off the orbit target horizontally. Fix: `M = R(yaw) * T(pos)` — the origin/feet land at pos for every yaw (the old filament form minus the scale comp, which the diligent assets don't have); `Game.cpp`'s `ENGINE_CAMERA=character` framing now computes the body centre as spawn + (centre − origin) instead of spawn + (centre − min). Verified with `ENGINE_AUTOTEST=enter ENGINE_AUTO_RUN=1 ENGINE_SCREENSHOT` — character centred while running.

---

## 2026-09-05 — gltfpack output is only partially compatible with Diligent's GLTF loader: meshopt-compressed buffers AND int16 rotation keys both load as garbage — the character renders invisible/NaN

**Rule:** the gltf → Diligent asset path has two hard constraints, neither
checked at load time (both are `VERIFY`s compiled out of the release
prebuilt libs, so failures are silent):

1. **No meshopt buffer compression** (`gltfpack -cc`): Diligent has no
   EXT_meshopt_compression support. The JSON parses fine (accessor min/max
   intact) but every buffer read is compressed bytes — bounding boxes come
   out at ~3e38 and geometry is garbage. Old filament/cgltf decoded it; the
   new path must not ship `-cc`.
2. **Animation rotation keys must be fp32**: gltfpack hard-codes rotation
   sampler outputs as int16 NORMALIZED snorm (`writeKeyframeStream`), and
   NO flag changes it (`-noq` still emits r_16 normalized; `-ar` only picks
   the bit depth). Diligent's `LoadAnimations` memcpy/reinterprets sampler
   outputs as float4 — quats like (-298, 437, -1576, 32725) flow through
   slerp/normalize into the joint matrices, NaN the entire skeleton (probe:
   every bone-matrix scale reads NaN, not 0.0) and the character vanishes.
   Translation/scale keys stay fp32 (unless `-at/-as` quantize them); vertex
   attribute quantization (u16 uvs/normals) is fine — the VERTEX converter
   handles normalized ints.

**Fix:** `scripts/export-models.sh` drops `-cc`, and a new post-gltfpack step
`scripts/gltf-rotation-f32.py` rewrites every int16-normalized rotation
output accessor as fp32 (v = max(snorm16/32767, -1)) into a fresh buffer
region + accessor. Re-run the export whenever eve/animations change.

**Probe:** `ENGINE_GLTF_DEBUG=1` in the new engine dumps the mesh AABB at
load (~3e38 = compressed buffer data) and, for the first 3 animation frames,
walks the joint chain for the first NaN plus the min/max bone-matrix scale —
**1.0 everywhere = healthy** (IBMs legitimately compensate the 0.01 cm-rig
scale on Hips); 0.0 below the root = compounding local scale (the 2026-09-04
gltf-standardize lesson); NaN = quantized/garbage sampler data.

**Also hit in the same session (each silently fatal):**

- `utils::dataManagerRead` does NOT decompress zstd — the shipped
  `<model>.zstd` must be decompressed by the consumer (magic 28 B5 2F FD;
  tinygltf's parse error reads "invalid literal; last read: '('" — 0x28 is
  the zstd magic byte).
- tinygltf picks GLB-vs-JSON parsing from the FileName EXTENSION — a pak
  path like `models/eve.zstd` (even decompressed correctly) is parsed as
  ASCII JSON and fails at the 'glTF' magic. Stage a `"<path>.glb"` name for
  the loader; serve the real pak bytes from the ReadWholeFile callback.
- `GLTF::Model(CI)` (the CPU-only ctor) does NOT parse the file at all — it
  only sets up attribute layouts (0 nodes, no error). For a CPU-only load
  (the animation-source glb) use `Model(nullptr, nullptr, CI)`: the device/
  context ctor runs the full parse and skips GPU resources on nulls.
- `GLTF_PBR_Renderer::CreateInfo.MaxJointCount` defaults to 64 — eve has 65
  joints; the 65th silently clips (set 128).
- Appending to a GLB's BIN chunk (the f32 rotation pass) requires updating
  `buffers[0].byteLength` (same rule as the 2026-09-04 singlekey lesson;
  forgetting it segfaults tinygltf at load).
- Placement for the Diligent path (row-vector matrices, applied left to
  right): root = `T(-minc) * R(yaw) * T(pos - minc)` — the local AABB min
  corner (feet) lands exactly at pos for every yaw. Cache the built matrix:
  `gltfPlaceAt*` sets a dirty flag, `gltfUpdate` consumes it, and frames
  with no placement call (waitingForGround) must keep reusing the cached
  root, not fall back to identity.

**Incident:** bringing the player character to the Diligent backend after the
filament removal. Sequence of silent failures, each invisible without its
probe: (1) raw zstd parsed as JSON ('(' error); (2) GLB parsed as JSON ('g'
error) after adding decompression; (3) bounds ~3e38 from meshopt-compressed
buffers — the loader had no error; (4) anim source loaded with 0 nodes from
the CPU-only ctor; (5) character present but invisible — NaN skeleton from
gltfpack's int16 rotation keys; (6) segfault from the BIN-append without
`buffers[].byteLength`. Each fix is in the rules above; the final state is
verified by the character portrait (ENGINE_CAMERA=character), the auto-run
third-person view (back to camera, mid-stride), and ENGINE_TPOSE=1.

---

## 2026-09-05 — Automated-run env pitfalls: ENGINE_LOG_TIMEOUT is MILLISECONDS, and piping the game through `head` SIGPIPE-kills it mid-run

**Rule:** `ENGINE_LOG_TIMEOUT` is multiplied by MILLION and added to nanos(),
so the value is milliseconds (`ENGINE_LOG_TIMEOUT=30000` = 30 s — `25` quits on
frame 1, which looks like "the game starts and instantly shuts down"). And never
pipe the game's stdout into `head` (or any early-exiting reader): when head
exits, the next log write gets SIGPIPE and the game dies silently mid-run — no
"engine: stopping", no crash, screenshot never fires. Redirect to a file and
grep the file afterwards.

**Incident:** during the filament-removal verification, three consecutive
"broken" runs (no screenshot, instant shutdown) were actually self-inflicted:
`ENGINE_LOG_TIMEOUT=25` meant 25 ms, and `... | grep ... | head -25` killed the
game by SIGPIPE the moment head collected its 25 lines.

---

## 2026-09-05 — Runtime-compiled HLSL (glslang) consumes cbuffer matrices TRANSPOSED relative to Diligent's row-major math — and a second cbuffer in the same PSO bound ambiguously

**Rule:** every runtime-compiled HLSL shader on the Diligent backend (device->
CreateShader + SHADER_SOURCE_LANGUAGE_HLSL, resolved against
DiligentFXShaderSourceStreamFactory) must receive float4x4 cbuffer members
written as `matrix.Transpose()`. With the plain Diligent matrix stored, the
shader applies the transpose: rotation-without-translation plus the
translation row as the projective row — the world collapses to a camera-
hugging sliver/patch that looks like a depth-buffer or culling bug. The
prebuilt DiligentFX SPIRV shaders are precompiled with a matching convention
and hide this; only runtime-compiled HLSL exposes it. Corollary: do NOT put
per-pass data in a SECOND cbuffer declared alongside cbFrameAttribs in the
same PSO — that cbuffer bound ambiguously (the PS read the frame buffer
instead, so `debugView` read fFarPlaneZ = 20000 and every debug branch was
dead while the cbuffer's GPU bytes verified byte-correct). Carry small
per-pass values in `CameraAttribs.f4ExtraData[]` (application-specific,
unused by DiligentFX) instead.

**Incident:** the azgaar heightmap terrain (phase 6, Diligent backend) rendered
as a white/grey sheet truncated to a few metres around the camera at eye-level
views and to nothing from 5 km up (landtop), while every byte-level probe
verified correct: VBO readback bit-identical to CPU lattice corners, cbuffer
readback byte-identical to the staged fill, SPIR-V linkage correct (WorldPos
loc 0, Normal loc 1), and the projection math checked out numerically. The
turnaround came from switching the validation vantage (ENGINE_CAMERA=close,
60 m up): the terrain rendered as a paper-thin SLIVER = a ground plane seen
EDGE-ON = the camera's height missing from the transform. With the transpose,
the identical scene rendered the full streamed window with a correct horizon.

**Probes that misled (documented so future sessions don't re-chase them):**
probe outputs written to the RGBA8_UNORM_SRGB swapchain are sRGB-ENCODED —
sample values must be linearized ((v+0.055)/1.055)^2.4 before decoding; NEGATIVE
values clamp to 0 (an abs/centered encoding is mandatory); and a PS-side
recompute of clip(WorldPos) is SELF-CONSISTENT under any matrix (both stages
share it), so "re-projects to its own screen position" proves nothing about
matrix correctness.

**Second incident, same session:** after the fixes verified green, a cleanup
strip of the temp shader probes truncated the VS file (the removal regex cut
the real main between #else/#endif), the game still ran (the .pak zip held the
previous good VS) and the NEXT full build shipped the truncated VS —
"everything went navy" with a compile error only visible in the log. The pak
pipeline (scripts/data.sh) zips whatever sits in c-game/data/pak_1/ — always
verify `zipping pak_1` ran AFTER a shader edit (plain `cmake --build` does NOT
refresh paks), and grep the shipped archive, not the working tree.

**Also:** scripted validation cameras (ENGINE_CAMERA=\*) are clobbered by the
persisted camera/player rows in build/c-game/data/db/db.db, and EVERY run
re-saves them at exit — delete both tables before each scripted capture
(python sqlite3 DELETE FROM camera; DELETE FROM player).

**Verification (pixel-diff A/B, same camera, ENGINE_SCREENSHOT_FRAME=1500):**
Diligent dry-turf look now matches the Filament baseline (same rust turf
texture, same pale noise patches, same horizon hills); landtop ramp from
5.5 km shows the full streamed window, seam-free; props/player remain
Filament-only by design (deferred with phase 6 / gltf zstd loader gap).

---

## 2026-09-04 — settings.json type validation silently REWRITES the file: one wrong-typed key (e.g. `"upscalerMode": 2` int vs "double" template) nukes every user setting via writeDefault()

**Rule:** when editing `build/c-game/data/settings.json` for an A/B test (or
hand-tuning), every key's JSON type must match its Settings.cpp template:
"double" keys need a decimal point (`2.0`, never `2`), "int" keys must be bare
ints, "boolean" real JSON booleans. ONE mismatched key makes validateSetting()
call writeDefault() + re-read, silently discarding ALL of the user's settings —
and until today writeDefault() didn't handle the "int" type at all, so
`shadowQuality`/`rendererBackend` were additionally dropped from the fresh file
(fixed 2026-09-04: writeDefault() now writes "int" keys; "number" branch was
dead code). Symptom signature: a settings-driven feature "doesn't apply" while
an identical earlier run worked, and the file's keys change between runs.
Probe: `grep` the file BEFORE and after the run; any unexpected value means a
writeDefault() clobber happened mid-run, invalidating the A/B.

**Incident:** the graphics-settings FSR upscaler test (upscalerMode=2 set with
python json.dump → bare int `2`) appeared to do nothing — the in-game DynamicResolution
stayed at 1.0 while the shadow-quality A/Bs (correctly-typed int key) had worked
minutes earlier. `getLastDynamicResolutionScale()` probes + an applyUpscale
readback log (renderScale=1.000 despite the edit) pointed at the loader; the
culprit was validateSetting's writeDefault path resetting the file at startup.
Re-ran with `2.0` → scale 0.770 immediately. The renderer code was correct all
along; the test harness (the hand-edited JSON) was the bug.

---

## 2026-09-04 — `Texture::setImage` is zero-copy too: a `PixelBufferDescriptor` with a `nullptr` callback must be backed by storage that outlives the command (freed/stack source = garbage texture, different every launch)

**Rule:** the zero-copy rule proven for `setBufferAt`/`setBuffer` (see the 2026-09 buffer entry below) applies equally to texture uploads: `Texture::setImage` only REFERENCES the `PixelBufferDescriptor`'s CPU buffer — the driver reads it later, on the engine loop thread. A `nullptr` callback means "caller keeps ownership", so the caller must keep the memory valid indefinitely; the safe patterns are (a) heap-copy the pixels and release them from the descriptor's callback (`PixelBufferDescriptor::make(data, size, fmt, type, [](void* b, size_t){ free(b); })` — the functor fires exactly when the driver has consumed the buffer, the pattern GuiFilament already used), or (b) transfer ownership of an already-heap buffer to the callback. A STACK source is always wrong: the frame is dead long before the upload executes. A freed source is worse than a crash: glibc mmaps multi-MB image blocks and reuses the same addresses, so texture N's upload happily reads texture N+1's pixels (stride mismatch → skewed/repeated bands) or arbitrary heap bytes (alpha usually ≥ 0.5 → the cutout discard never fires → solid card).

**Incident:** "grass dont look correct and they are different looking every launch" — cards rendered as solid striped teal/green walls whose stripes were squashed copies of a grass tuft; the pattern changed per run. Root cause: `loadGrassTexture` called `utils::imageDestory(&img)` (→ `stbi_image_free`) immediately after `setImage` with a `nullptr` callback — the GPU copied freed/reused heap. Per-launch variance (the signature the two earlier grass incidents lacked) is the allocator race: which uploads win decides which textures are garbage. This also explains why the RADV LINEAR-min fix (entry below) only reduced the symptom class: two bugs produced the same-looking solid striped cards. Same latent pattern fixed in the same pass: the props 1×1 white fallbackTex and the terrain 1×1 white fallback (`createRgba8(white /*stack*/, …)` + `nullptr` callback) — `createRgba8` now always copies + callback-frees, which also closes the theoretical race of a later look-register `vector.assign` realloc against in-flight biome/climate uploads.

**Probe:** the complaint itself is the probe — "texture content differs between launches of the same binary" with no code path explaining it means the GPU is reading storage the CPU already released (cf. the Diligent dynamic-ring entry: same "changes with no reason", different mechanism). Fix verified: two full launches at frame 400 differ by 0.045 % of pixels >16 (wind-sway noise); every card a clean cutout.

**Wiring:** `PropsRenderFilament.cpp` — `uploadCopy()` helper + `loadGrassTexture`/`initPass` fallbackTex now upload malloc'd copies via `PixelBufferDescriptor::make(..., free)`; `HeightmapTerrainFilament.cpp` — `createRgba8` copies internally (caller buffer may be stack/short-lived).

## 2026-09-04 — Filament `doubleSided` flips the normal on back faces, blacking out thin up-normal vegetation (grass cards); and never mipmap sparse alpha-cutout grass textures

**Rule:** the built-in lit model computes `n = gl_FrontFacing ? n : -n` (`surface_shading_parameters.fs`), so `doubleSided: true` lights the back face with the _flipped_ normal. For thin vegetation cards whose mesh normals all point one way (grass cards are all (0,1,0) up, but the two quads are vertical), the back face gets normal (0,−1,0) → N·L ≈ 0 → black tuft silhouettes. The correct lighting for thin blades is to light BOTH faces with the _unflipped_ normal (that's exactly what the old engine's `azgaar_props.frag` did for grass/palm/reed, `isThin` = species 0/6/9/12). In Filament, `MaterialInstance::setDoubleSided(false)` disables that normal flip **without** changing culling, so pair it with `setCullingMode(NONE)`: both faces still rasterize, neither gets flipped → both light from the same side. (Setting `doubleSided:false` alone would also re-enable back-face culling and drop the back face, so you must re-set culling NONE.)

**Incident:** "grass plane objects look wrong" — two screenshots at the same camera: green grass tufts with black back faces (the flip), plus solid filled cards. Root cause 1: `props.filamat` `doubleSided: true`. Root cause 2 (separate): generating mipmaps on the sparse alpha-cutout grass textures (`.levels(7)+generateMipmaps()`) averages the alpha _upward_ in the transparent border, raising it above the hard 0.5 discard, so distant (minified) cards render as solid filled rectangles — reverting to `.levels(1)` (level-0-only sampling) restores proper cutout. These sparse cutout textures must NOT be mipmappable.

**Wiring:** `AZGAAR_PROPS_FLAG_DOUBLE_SIDED` (bit 1, was "reserved") in AzgaarProps.h; `azgaarPropsSpeciesRenderFlags` returns it for grass(0)/palm(6)/reed(9)/flower(12); `PropsRenderFilament::buildTile` sets `setDoubleSided(false)`+`setCullingMode(NONE)` when bit 1 is set (the material's `doubleSided:true` is the default, so closed-solid species are unaffected). Verified at the user's actual eye-level camera (`ENGINE_CAMERA=propsground`, pos −109.67 2.24 −93.39): proper cutout tufts, no black, no solid cards. (A far 89 m orbit still shows minification aliasing on level-0-only sampling — cosmetic, not the reported bug.)

---

## 2026-09-04 — camera_at_origin does NOT fix f32 world state: at 39 km every f32 position sits on a 3.9 mm grid and the character/ground shimmer by ~4 px

**Rule:** `Engine.debug.view.camera_at_origin` (on by default) only shifts the SHADER frame (view matrix / lighting) to the camera; it cannot de-quantize geometry that was already stored as f32 absolute coords. Any world state kept as f32 far from the origin (player pos from the Jolt C API, the model mat4f, camera eye, terrain corner data, prop instance positions) is quantized to ULP(39 km) = 2⁻⁸ m ≈ 3.9 mm — visible as up to ~4 px of jitter at a 1.5 m camera (~1 mm/px). To fix far-field precision: keep world state in f64 (Jolt is double internally — add an f64 GetPosition to the C wrapper) and place renderables RELATIVE to a double anchor (camera or grid-snapped origin): model translation = f32(pos_f64 − anchor_f64) is a small number with sub-mm precision. Two ways to wire the anchor: (a) camera_at_origin OFF + game-side relative placement with an f64 view matrix (Filament's Camera API already takes doubles), or (b) `features.view.enable_grid_based_world_origin` + `View::setGridSize()`, which needs a filament patch to expose the snapped origin (no public getter; internal hysteresis makes re-implementing the snap fragile). The grid feature alone does NOT help — the f32 geometry inputs are still quantized.

**Incident:** "teleported player to Azgaar cell 0 (~39 km x/z) — the animation is jittery. I thought filament was handling that (camera at origin)." Repro: `ENGINE_TELEPORT="39000,100,39000" ENGINE_AUTO_RUN=1 ENGINE_JITTER_PROBE=1` (env hooks added in Game.cpp loadWorld + PlayerSystem::update, kept). The probe logged p.pos and the camera eye at full f32 precision: every value was an exact multiple of 2⁻⁸ m (e.g. 39300.945312 = 39300 + 242/256) stepping ~55 mm/frame while running — the position is on a 3.90625 mm grid, ULP(39300) in f32. The model (mat4f translation), the orbit eye (f32 lookAt) and the terrain corners (f32 absolute) are each on that grid with different phases, so character, ground and the whole view all shimmer by up to one grid step per frame. Filament's per-frame −cameraPos shift (the non-grid camera_at_origin branch in View.cpp computeCameraInfo) operates on these already-quantized inputs — nothing downstream can recover the lost bits.

**Probe:** full-precision (%.6f) per-frame pos/eye log is decisive — if every logged coordinate is a multiple of 2⁻ⁿ m for some n, it is f32 quantization, not physics or animation. The ULP follows |pos|: 3.9 mm at 32–64 km, 1.9 mm at 16–32 km, 0.12 mm at 1–2 km (why nobody saw it before the 39 km Azgaar teleport — a 0.12 mm step is sub-pixel at normal spawn distances).

**Status: FIXED (2026-09-04).** Implemented the relative-to-anchor rework: Jolt f64 position getter, `camera_at_origin` disabled (the debug property must be set **after** `Engine::createView()` — it is registered in the View constructor, so setting it earlier silently fails), and all renderables placed in anchor-space (the camera eye's xz, f64): terrain corners are tile-local (the renderable transform carries tile-origin−anchor), props instance data is tile-local (+ a `tileRel` uniform), and the camera is posed at (eye−anchor). Two shader gotchas found while verifying: (1) Filament's final vertex position is `material.worldPosition` (world-space, used directly via `getClipFromWorldMatrix()` — the renderable transform is applied ONLY to the culling box, NOT to a manually-set worldPosition), so props add their tile offset explicitly in the vertex stage; (2) world-anchored value noise (micro-bump / dry-turf) is **aperiodic**, so a `fract(anchor/freq)` phase does NOT re-anchor it (that corrupted the ground into black smears) — it must use the WORLD xz (`anchorSpace.xz + anchor`, stationary per point, ~4 mm f32 grid = sub-pixel for 4–48 m features); only _periodic_ tiling (grass, cliff) takes the exact `fract(anchor·freq)` phase. Verified: origin view matches base (autorun), and the 39 km teleported cell (`ENGINE_TELEPORT="39000,100,39000"`) renders a coherent, non-shimmering scene.

---

## 2026-09-04 — gltfio skips single-keyframe animation channels, so a joint's transform from the PREVIOUS clip survives forever; constant channels that don't match the node's static TRS then corrupt the pose

**Rule:** a glTF sampler with fewer than 2 keys must still be applied — gltfio's `Animator::applyAnimation` historically did `if (sampler->times.size() < 2) continue;`, which silently never writes such a joint, leaving whatever transform it had from the last clip that DID animate it (stale-pose leak between clips). And a "constant" channel is only safe if its constant equals the node's static TRS — otherwise applying it corrupts the pose (e.g. a Head scale channel of 0.01 shrinks the head to 1%).

**Incident:** "after running (W) and releasing, the left arm stays in the run pose instead of returning to idle." Mixamo/Blender collapse constant channels to 1 key (idle's `LeftForeArm rotation` etc.); gltfio skipped them, so after run→idle the arm kept the run clip's bend. The first fix (apply single-key samplers too — patch in `cpp-thirdparty/filament/git/libs/gltfio/src/Animator.cpp`, now `if (sampler->times.empty()) continue;`, **must be kept and rebuilt into `libgltfio_core.a`**) then made the head vanish: the idle action's `Head scale` channel is constant 0.01 (the cm factor) in the asset itself, so writing it every frame shrank the head.

**Fix (both kept):** (1) the gltfio patch; (2) `scripts/gltf-singlekey-fix.py`, wired into `scripts/export-models.sh` between `gltf-standardize.py` and `gltfpack` (plain GLB, pre-quantization). It rewrites every channel whose keyframes are constant (single key, or identical keys within 1e-5) to the target node's static TRS component — a strict no-op that makes constant channels harmless and forces any joint not animated by the active clip back to rest each frame. 3000 channels rewritten on animations.blend, incl. the 0.01 Head scales.

**Superseded (2026-09-05):** the singlekey-fix step was REMOVED and the script deleted — the premise was wrong. The "0.01 Head scale in the asset" was actually corruption produced by gltf-standardize.py's in-place shared-accessor write (see the 2026-09-05 fingers entry), and flattening every constant to static pose destroyed legitimate pose holds (the run clip's curled fingers, 1 key per finger bone). Single-key samplers are now applied by the engine instead of skipped.

**gltfpack/glb gotchas hit along the way:** (a) gltfpack's loader rejects a GLB whose sampler **output** accessor count differs from the **input** (times) count — copied single-key outputs must duplicate the key (count = input count), and a count-1 output on a count-2 input fails with a bare "invalid GLTF"; (b) when appending data to a GLB's BIN chunk you must update `buffers[].byteLength` or gltfpack says "buffer too short"; (c) if you rewrite accessor values, update its `min`/`max` to match, and rewrite ALL keys of a shared accessor, not just the first; (d) gltfpack's "invalid GLTF" gives no detail — bisect with minimal variant files (cgltf is more lenient than gltfpack and will NOT catch this class of error).

---

## 2026-09-04 — A yaw-pivot matrix must anchor the point that should stay fixed: gltfPlaceAtFacingFilament pivoted on the AABB corner, swinging the visible model ~1.3 m off the camera target when the facing yaw differed from the camera's

**Rule:** when building a placement matrix as `T(anchor) * R(yaw) * S(comp)`, the point that stays put under the yaw is the LOCAL point that the pre-rotation transforms send to the origin — verify which local point that is, not which point you meant to anchor. In `T(off) * R * S * T(minc)`, the yaw-fixed point is local `-minc` (wherever `S*(p+minc) = 0`), NOT the origin/feet you intended; the origin then orbits a `|minc|`-radius arc around the target as yaw changes.

**Incident:** "switching to S (run backwards) moves the player to the left of the screen." The movement basis, orbit camera, and Jolt character were all self-consistent (repro with per-frame pos/cam/model logging showed the character and camera correct); the VISIBLE MODEL was offset ~1.3 m screen-left while running S. Root cause: `gltfPlaceAtFacingFilament` used `T(pos - minc) * R(yaw) * S * T(minc)`, which pins local point `-aabbMin` at a fixed world spot and makes the feet (local origin, eve's minc = (-0.64,-0.01,-0.13)) swing in a 0.65 m arc — up to ~1.3 m lateral at facing = camYaw (running backwards). At the saved camera yaw the W-facing (camYaw+π) offset nearly cancelled, which is why only S looked wrong. Fix: `M = T(pos + (comp-1)*minc) * R(yaw) * S(comp)` — origin/feet at (x,z) for every yaw, and yaw-0 placement byte-identical to the pre-pivot-fix transform. Verified: S-run pdbg log showed model == pos exactly, and S-run + auto-run screenshots show the model centred (back view on W, face view on S).

**Superseded:** the 2026-09-04 facing entry's placement form `T(off) * R * S * T(minc)` fixed the FRONT DIRECTION (R(+yaw) → front on (sin,0,cos)) but left the pivot on the wrong point; its "back at frame ~400" auto-run check passes at yaws where the W-offset happens to cancel, so it does not validate the pivot. The portrait vantage (`ENGINE_CAMERA=character`) is unaffected — it frames the model by bounding box, not by the pivot.

---

## 2026-09-04 — eve's front is local +Z and mat4f::rotation(r, +Y) maps +Z → (sin r, 0, cos r): gltfPlaceAtFacingFilament must use R(+yaw)

**Rule:** never assume a character asset's front axis from "the glTF
convention", and never trust the sign of a library rotation from memory —
both assumptions in the previous version of this entry were wrong. Two
verified facts (filament's mat4f::rotation compiled and evaluated
standalone; eve's facing verified by autorun screenshot):

1. `mat4f::rotation(r, +Y)` is right-handed: **+Z → (sin r, 0, cos r)**,
   +X → (cos r, 0, −sin r). (A "−sin r" reading of it makes a wrong
   −yaw−π formula look exactly right — self-consistent but false.)
2. eve's front (Mixamo/Blender, characters face the viewer) is **local
   +Z**. To land the front on (sin yaw, 0, cos yaw) — the old engine's
   convention that the player's W-forward and RMB facing are both derived
   from — `gltfPlaceAtFacingFilament` uses `mat4f::rotation(+yaw, +Y)`.

If you ever see "the character runs toward the camera / faces her
third-person camera", suspect the placement matrix first — the yaw math
upstream (orbit camera, movement basis, RMB facing = camYaw+π) is
self-consistent and was correct. Verify any facing change with
`ENGINE_AUTOTEST=enter ENGINE_AUTO_RUN=1 ENGINE_SCREENSHOT=/tmp/x.jpg
ENGINE_SCREENSHOT_FRAME=400` (expect her BACK at frame ~400); also re-verify
whenever models/eve.zstd is re-exported. The portrait vantage
(`ENGINE_CAMERA=character`, eye at chest + (h, ·, −h)) frames the back
with the corrected model, unchanged.

**Incident:** "when running W + RMB orbit I see her face, I should see her
back." An earlier same-day entry had fixed this to R(−yaw−π) on the
assumptions "front = −Z" and "+Z → (−sin r, cos r)" and claimed a
before/after screenshot proof — but the identical repro (autorun +
screenshot) still showed her face, so that proof was flawed. Recomputed
from the actual matrices: R(−yaw−π)·+Z = (sin yaw, 0, −cos yaw), a
frontal mirror of the target (sin yaw, 0, cos yaw), which at the saved
yaw (−152°) points straight at the orbit camera — exactly the reported
symptom. Fix: the one rotation in GltfFilament.cpp to R(+yaw) (+ comment);
before = face, after = back, proven by the autorun repro. (GltfDiligent.cpp
is a stub and ignored for now.)

**Superseded:** an earlier same-day entry claimed "front = local −Z" and
"mat4f::rotation maps +Z → (−sin r, cos r)" and fixed this to R(−yaw−π);
both assumptions were wrong and that "fix" was the live bug. (It also
flipped the portrait-camera Z offset to compensate — with the corrected
model the current (−h) offset is the correct one, so it stays.)

---

## 2026-09-04 — Pre-multiplying a hierarchy transform only preserves world poses for DIRECT children: skinning collapses to a point when applied to every joint

**Rule:** when you rewrite a node hierarchy so that a parent's transform `A`
gets baked into its children's LOCAL transforms (to make the parent identity),
`A @ L` is only the pose-preserving rewrite for the parent's DIRECT children —
for any deeper descendant the hierarchy already applies `A` once on the way
down, so pre-multiplying `A` into its local too re-applies it at EVERY depth.
With a uniform scale `s` the descendant's world scale becomes `s^depth`; with
Mixamo-style `s = 0.01` that underflows to 0.0 by depth ~5, and any matrix
derived per-depth from it (bone matrices = `W_mesh⁻¹ × W_joint × IBM`, where
the IBM inverts the ORIGINAL single-`A` world pose) goes to zero or to
`1/s^(depth-1)` — the mesh collapses to a point at the root joint (or
explodes). If the parent also has a skinned-mesh holder node as a direct
child, skip it: its transform cancels exactly in the final skinned position
(`W_mesh × (W_mesh⁻¹ × W_j × IBM) × pos`), so leaving it authored keeps the
asset bounding box in true units.

**Incident:** "eve character animations looking good in old engine, not so
much here." In the new engine eve rendered as a dark crumpled ball floating
at chest height while the rest of the scene was fine. `GLTF_DEBUG_SYNC` bone
matrices told the story in one run: `M[0]` (root joint) scale ≈ 1.0, but
`M[5]…M[64]` scale 0.0000 — every joint below depth 1 contributed zero to
the skin. Cause: scripts/gltf-standardize.py pre-multiplied the armature
transform `A` (0.01 scale, 90° X) into EVERY skin joint's local TRS and every
joint's keyframes, so all 65 joints carried 0.01 local scale (max depth 13
→ world scale 0.01¹³ ≈ 0) while the untouched IBMs compensated exactly one
0.01. The old engine's raw export (armature 0.01, joints 1.0) was skinning-
correct all along under standard glTF semantics — the standardize step
introduced the collapse. Fix: rewrite only the armature's direct children
(and only their keyframes); the mesh holder node stays authored. Re-export
regenerated eve.zstd + animations.zstd; idle and run (with crossfade) now
match the old engine.

**Probe:** one `ENGINE_SCREENSHOT` run + the `GLTF_DEBUG_SYNC=1` log is
decisive for any skinning complaint: it prints the per-joint
`boneMatrix = W_mesh⁻¹ × W_joint × IBM` scale — 1.0 everywhere = healthy,
0.0000 below the root = compounding local scale, ~100× = a single
uncompensated hierarchy factor. And inspecting the shipped GLB (unzip the
pak, zstd -d, read the JSON nodes) shows every joint's local scale directly.

---

## 2026-09-04 — Filament materials flip UV.y by default (`flipUV`): texture-sampling materials must set `flipUV : false` when mesh UVs are authored in image-row order

**Rule:** Filament's built-in vertex stage flips `uv0.y` for EVERY material
unless the material sets `flipUV : false` (default is true — `MaterialBuilder::mFlipUV = true`,
shaders/src/surface_material_inputs.vs: `material.uv0 = vec2(mesh_uv0.x, 1.0 - mesh_uv0.y)`).
Textures uploaded via `Texture::setImage` from stb_image data (row 0 = image
top) put the image top at V=0, so a mesh whose UVs are authored in raw
image-row order (V=0 = image top) renders vertically MIRROR-FLIPPED under the
default flip. Any material that samples a texture through `getUV0()`/the
built-in samplers and whose mesh UVs were ported from the old engine (Vulkan,
no such flip) must declare `flipUV : false`. Flip-invariant uses (radial/disk
alpha tests centred on 0.5, procedural geometry) are unaffected; explicit
texel fetches (instance-data textures) bypass the flip entirely.

**Incident:** "grass objects are upside down." The ported `buildGrassCard`
UVs (top of the crossed card = V 0 = image top = tuft tips; card base =
V bottomV = tuft base, trimming the texture's empty bottom band) were
authored against the raw upload convention — correct for the old Vulkan
engine, which never flipped UVs. `props.mat` never set `flipUV`, so the
Filament vertex stage mirrored every card vertically: tuft tips at the
ground, dense base at the card top. The old-engine A/B (rebuild the material
with the default flip, screenshot `ENGINE_CAMERA=propsground`):
pre-fix the dark dense tuft base sits at the TOP of each card with thin tips
hanging DOWN into the ground; post-fix the base is grounded and tips point
up. `grassMeasureBottomV`'s trim was silently cutting the wrong end (tips
instead of padding) as part of the same flip.

**Fix:** `flipUV : false` in the `material {}` block of
c-engine/renderer/filament/materials/props.mat (+ comment), rebuilt via the
CMake matc step (matc -a vulkan -l 2 → pak_1/materials/props.filamat). Only
the grass card ranges sample `cardTex` through `getUV0()` in this material —
all other species ranges are vertex-coloured (flag bit0 off), the flower
radial test is Y-flip invariant, and `instanceData` is fetched with explicit
UVs — so the flag has no side effects. Check the sibling material
(heightmap_terrain) only if it ever samples image textures through
`getUV0()`; its heightmap is procedural and its look is already validated.

---

## 2026-09-04 — Fragment `getWorldPosition()` is ALSO camera-shifted: world-anchored shading must use `getUserWorldPosition()`

**Rule:** `Engine.debug.view.camera_at_origin` (default true in this build) shifts
the shader frame for the whole pipeline, not just the vertex stage: the
fragment's `getWorldPosition()` is CAMERA-RELATIVE too. Any world-anchored
fragment math — world-space texture tiling, procedural noise fields,
sea-level/beach bands (`worldPos.y`), altitude/snow bands, mapBounds UVs —
swims with the camera unless it uses `getUserWorldPosition()` (fragment-only
API returning the API-level position; metre-scale features at ±20–40 km are
fine in f32). Directions (`getWorldViewVector`, normals) are
translation-invariant and stay on the shifted frame. Geometry is NOT
affected: baked world-space vertices with an identity transform ride the
shift correctly, which is exactly why the bug hides — trees approach, ridge
silhouettes hold still, only the SHADING detaches from the ground and reads
as a treadmill.

**Probe (one run, unambiguous):** `ENGINE_TERRAIN_DEBUG=ramp` +
`ENGINE_CAMERA_DOLLY="0,12,0"`, two runs differing only in
`ENGINE_SCREENSHOT_FRAME` (420 vs 1020). Pre-fix: the ENTIRE ground flipped
hue (green at camY 85.6 → magenta at camY 209.4) — the height ramp swept
with camera altitude. Post-fix: identical ground hue in both frames, with
the magenta band appearing only on the higher distant terrain. A uniform
full-screen hue change under pure camera translation IS the signature of a
camera-shifted `worldPos` (ramp keyed to `groundY - camY` is constant per
terrain point, so only the shift explains it).

**Incident:** "camera is moving towards the trees but terrain looks the same
— doesn't look like terrain is standing there and we are flying above it."
Streaming/lattice/props/camera were all correct (see the 2026-09-04
worldPosition entry above — same root, fragment side). Every look layer in
`terrain.mat` was keyed to the camera-shifted `getWorldPosition()`: grass
tiling, dry-turf noise (12/48 m), beach band, snow line, altitude rock,
biome `mapUV`. At near-constant flight altitude all of them stayed glued to
the camera frame while geometry moved, so the ground read as an infinite
sliding sheet.

**Fix:** `terrain.mat` fragment stage: `vec3 worldPos =
getUserWorldPosition();` (one line — every field derives from it).

**Second find in the same incident:** the default camera framing in
`Game::loadWorld` computed + logged `worldHighestLandPoint` but the actual
`rendererCameraLookAt` call still used the `{1,1,1}/{0,0,0}` placeholder —
every normal boot spawned 1 m above sea level at the map origin (open
sea/beach on Chilerel), the exact degraded vantage the "Flying doesn't look
like flying" lesson was supposed to have fixed. Completed per the comment:
eye 250 m above the peak, level gaze at the peak's XZ. When a fix's comment
describes behaviour, grep that the call site actually consumes the computed
values — a correct log line is not a correct implementation.

---

## 2026-09-04 — "Flying doesn't look like flying": verify the world scrolls, then blame the content

**Rule:** before suspecting the camera/view path when motion "does nothing",
prove the world actually scrolls: `ENGINE_CAMERA=landtop ENGINE_TERRAIN_DEBUG=ramp
ENGINE_CAMERA_DOLLY="300,0,0"` + two runs differing only in
`ENGINE_SCREENSHOT_FRAME`, then correlate the two JPEGs for a global shift
(`np.roll` search). 500 m of dolly at 5000 m altitude predicts ~140 px; the
measured best shift was 136 px — transform path correct, case closed. Do NOT
gate on pixel-diff percentages: aliased world-tiled speckle produces 40–70 %
differing pixels between ANY two frames (it reads as noise whether it
translated or re-rolled), and a vision pass reads "new speckle" as "same
speckle". Only world-anchored structures with identity (the height ramp, tree
canopies, ridge silhouettes) are honest motion probes.

**Incident:** "I'm flying over terrain but it doesn't look like it; vertical
movement works." Everything was rendering correctly — the spawn was the
problem. The default camera framed the world ORIGIN, which on Chilerel is open
sea (~-33 m seabed): no water plane yet (phase 8), no props over water, no
relief, seen from 1590 m. Horizontal motion had literally no in-frame cue;
vertical motion changed the ground's scale and read fine.

**Fixes:** default camera now frames the highest land point low and level
(the old `TODO(azgaar)` in `Game::loadWorld`); added `rendererSetFog` →
`View::setFogOptions` distance haze (`ENGINE_FOG_DENSITY` tunes it) so the
far field recedes and the 20 km far-plane cut hides behind a fade; ambient
30000 → 12000 (27 % of the sun washed every slope's NdotL contrast out —
keep ambient ≲ 1/9 of sun).

**Filament fog traps:** `FogOptions.heightFalloff` DEFAULTS to 1.0 **per
metre** — at any flight altitude `density·e^-h` collapses to zero and fog
silently does nothing; set it explicitly (0 = constant). Fog is per-renderable
(`RenderableManager::Builder::fog`, default ON) and per-view
(`options.enabled && density > 0`); the fog color must equal the sky clear
color or the horizon seams. A 0.004/m density probe (view collapses to the
fog color) is the one-run check that the fog path is live before tuning.

---

## 2026-09 — Diligent dynamic buffers are per-frame scratch, not storage

**Rule:** a Diligent buffer that is not re-mapped every frame must not be
`USAGE_DYNAMIC`. Static-per-world constants go in an `USAGE_IMMUTABLE` buffer
uploaded through initial data (`USAGE_DEFAULT` + `UpdateBuffer` if they change
rarely). `CreateUniformBuffer` defaults to `USAGE_DYNAMIC` + `CPU_ACCESS_WRITE`
— always pass the usage explicitly for anything longer-lived than one frame.

**Incident:** the terrain's default-texture painting (sand/grass/snow/cliff)
flickered and collapsed to all-sand on the Diligent backend while filament was
correct. The material-constants buffer (sandHeight, snowHeight, cliffSlope,
styleTiling, tile→layer tables) was mapped and written once at init.

**Mechanism:** Diligent's Vulkan backend sub-allocates every `Map` of a dynamic
buffer from a per-frame ring heap that is shared with _all_ other dynamic
mappings — our frame viewProj/sun constants, imgui vertex/index buffers,
everything. When the ring wrapped, the region holding the material constants
was reused, so the shader read viewProj floats as `sandHeight`/`snowHeight`
and stale heap bytes as tile tables:

- `sandHeight` ≈ 0.97 (viewProj.\_11) → smoothstep collapsed → sand everywhere
- `styleTiling` ≈ 0 → near-constant texture UVs → flat single-color wash
- garbage tile-layer ints → splat decals appearing/vanishing per tile

**Why it was hard to spot:**

- the corruption is silent — every access is in-bounds, validation layers
  have nothing to say
- the symptoms looked like texture/mip/binding bugs (flicker "while moving
  the mouse") because the ring wrap correlated with elapsed time; the input
  was coincidence
- the same code pattern works on filament, where MaterialInstance parameters
  are engine-managed

**Symptom signature to remember:** constants that read as _another buffer's
content_ (values tracking the camera = viewProj leaking in), state that is
correct right after init and degrades a few hundred frames later, or output
that changes with no code path explaining it. Suspect the dynamic ring first.

**Fix:** `c-engine/terrain/TerrainDiligent.cpp` creates the material buffer as
`USAGE_IMMUTABLE` with the filled `TerrainMaterialCpu` as initial data, at
`terrainFinishDiligent` (where the params exist). Per-frame buffers stay
dynamic — re-mapping them every draw is the correct pattern.

### Debugging techniques that paid off

- **Read GPU-visible data back through the render target.** Diag shader modes
  that output cbuffer values as colors (`ENGINE_DEBUG_TERRAIN=mat|mat2`) made
  the garbage visible and identifiable; no debugger attaches to GPU memory.
  Extending the existing `ENGINE_DEBUG_TERRAIN` view system was ~20 lines.
- **Pattern-match garbage against other live data.** 0.97 ≈ viewProj.\_11 was
  the break; confirming the value _tracked the camera_ (different
  `ENGINE_CAMERA` → different readout) proved which buffer was leaking before
  the mechanism was known.
- **Separate time from input.** The bug "responded" to mouse movement but was
  actually temporal. Fixed camera over a long run + byte-comparing frames at
  600 vs 2500 showed the degradation was about frame count.
- **Byte-identical screenshot comparison** (`ENGINE_SCREENSHOT` with
  `ENGINE_SCREENSHOT_FRAME`) is a cheap regression gate per backend/camera.

### Dead ends (checked, not the problem — don't re-chase)

- **HLSL register/binding collisions:** Diligent remaps all SPIR-V bindings
  itself at PSO creation (`RemapOrVerifyShaderResources`); explicit
  `register(bN/tN/sN)` annotations are cosmetic. Verified by dumping the
  post-remap bytecode (`ENGINE_DUMP_TERRAIN_SPIRV=path`, then `spirv-dis`).
- **glslang-HLSL cbuffer packing:** int arrays do get 16-byte element strides;
  the CPU mirror (`TerrainMaterialCpu` with its static_asserts) is correct.
- **PSO static variables vs SRB variables:** both paths showed the identical
  mis-wiring, because there was no mis-wiring — the descriptor was fine, the
  memory behind it was clobbered.
- **Texture array upload / mip layout:** subresource order and BC7 strides
  were correct all along (splat tiles and styles always rendered fine).

---

## 2026-09 — lossy KTX2 on splat weight maps is bigger AND visibly worse than PNG

**Rule:** low-entropy painted maps (splat/weight tiles, masks) must not go
through lossy texture codecs. UASTC zcmp19 + BC7 baking a 1024x1024 splat
UDIM tile produced ~1.4 MB KTX2 vs ~10–130 KB PNG (17× for the whole
splat set) with visible compression artifacts at blend edges. Ship the PNGs
lossless; the engine decodes them to RGBA8 at load (`Terrain.cpp
loadLayer` PNG branch, `TerrainDecodedArray.rgba8`, backends build an
R8G8B8A8/TEX_FORMAT_RGBA8 array with a single mip level). KTX2+BC7 is
still right for high-entropy PBR sets (style albedo/normal), where it wins.

**Incident:** small grid-like glitches on painted terrain areas traced to
the UASTC→BC7 splat pipeline; file-size comparison (roads1: 473 KB PNG vs
8.1 MB KTX2) made the switch to straight PNG copies in `build-terrain.py
convertSplatTiles` (old `.ktx2` splat outputs are pruned, manifest paths
flip to `.png`).

---

## 2026-09 — Filament buffer uploads are zero-copy: the source storage must outlive the command

**Rule:** `VertexBuffer::setBufferAt` / `IndexBuffer::setBuffer` with a plain
pointer do NOT copy. Filament hands the pointer to the driver, which reads it
when the command buffer executes (next frame, on the engine loop thread).
Any buffer that is not re-uploaded every frame must own its storage for its
whole lifetime: heap-allocate it and free it from the `BufferDescriptor`
destruction callback (or use a `PixelBufferDescriptor` with a copy callback).

**Incident:** the phase-5 terrain VBOs were filled from a function-local
`std::vector<float>` scratch that `uploadTile` reused. With
`kUploadsPerFrame = 3`, every tile uploaded in one frame except the last got
drawn with the last tile's corners — the whole visible ring briefly showed
one tile's geometry stacked 3× before the next frame's batch clobbered it
again. Only caught because the automated dolly run (camera moving, constant
fresh uploads) made the mismatch visible in screenshots; a static run
re-uploads nothing and looks fine.

**Fix:** `HeightmapTerrainFilament.cpp uploadTile` heap-allocates the
interleaved (pos, tangent-frame) corner storage per upload and frees it via
the `BufferDescriptor` callback — the same rule the shared lattice IBO
already followed.

---

## 2026-09 — World-tiling terrain textures need mipmaps + anisotropy, or distance is aliasing soup

**Rule:** any texture tiled in world space and seen across kilometres
(grass/cliff/snow/sand albedo + normal maps) MUST be sampled with
`LINEAR_MIPMAP_LINEAR` + `MagFilter::LINEAR` + anisotropy 16 + `REPEAT`.
Plain `LINEAR` minification turns a 3.4 m tiling grass albedo — seen from a
few km — into high-frequency straw speckle, and its normal map into shading
noise. The KTX2 assets ship 11 mip levels for exactly this; a sampler that
doesn't use them is a bug.

**Incident:** the first phase-5 screenshots of the new Filament pass showed
the terrain as fine speckle from any non-closeup camera, far worse than the
old engine. The sampler was a plain `LINEAR` repeat (fine for the
per-world biome/climate maps, which are clamped and only a few hundred
texels; wrong for the tiling assets).

**Fix:** `makeTilingSampler()` in `HeightmapTerrainFilament.cpp`
(`LINEAR_MIPMAP_LINEAR` + anisotropy 16 + repeat) for all default terrain
textures; the biome/climate maps keep their plain clamp samplers. Mirrors
the old engine's `SAMPLER_LINEAR`.

---

## 2026-09 — Filament emissive is photometric (nits): 0..1 debug colours are invisible

**Rule:** in a physically-lit Filament scene, `material.emissive` is in
nits and is scaled by camera exposure exactly like real lights. A debug
colour of 0..1 next to a ~1e5-nit sun is black to the eye. For
lighting-independent diagnostic views (height ramps, raw texture
readouts), do NOT use emissive — draw a flat matte surface: zero the normal
perturbation, set `material.baseColor` to the debug colour and
`material.roughness = 1.0`, and let the existing lights show it.

**Incident:** the terrain debug views (ramp / biome) ported the old
engine's "set emissive, zero base colour" trick and rendered black —
invisible in every validation screenshot, which looked at first like the
whole look pipeline was dead.

**Fix:** `terrain.mat` debug branch sets `material.normal = (0,0,1)`,
base colour = debug colour, roughness 1 (see the `dbgOn` block).

---

## 2026-09-04 — Filament material.worldPosition is camera-shifted (camera_at_origin): never write absolute world coords

**Rule:** in this Filament build, `Engine.debug.view.camera_at_origin`
defaults to `true`: every renderable's `worldFromModelMatrix` (and thus
`material.worldPosition` in `materialVertex`) lives in a frame whose
origin is the CAMERA position (or a snapped grid origin), not absolute
world space. Writing absolute/"user" world coordinates into
`material.worldPosition` places geometry at `cameraPos + worldPos` — for
a 13 km-offset map that is ~17 km off, past/at the far plane and outside
the frustum: ZERO PIXELS, silently (no warning, draw still emitted). When
placing vertices by absolute world position in the vertex stage, convert:
`material.worldPosition.xyz = worldPos - getUserWorldFromWorldMatrix()[3].xyz;`
(assumes no IBL rotation — its translation is the origin shift; the
upstream `heightfield.mat` sample does the same with the deprecated
`getWorldOffset()`). Also: `RenderableManager::Builder::boundingBox()` is
in OBJECT-LOCAL space and is transformed by the renderable's transform —
passing a world-space box to a transformed renderable puts the culling
AABB at `transform × worldBox` (e.g. ~26× the map offset) and culls every
draw.

**Incident:** the phase-7 props pass placed all 63k instanced vegetation
instances by writing absolute instance positions into
`material.worldPosition`; every real draw rendered zero pixels for three
ledgered rounds while the VBO/IBO/slots/texture delivery were all
suspected and "proven" broken. A same-bytes world-BAKED probe (data at
absolute coords through the default object-matrix path) always showed,
which is why the diagnosis meandered: baked data rides the camera shift
correctly via the identity transform, but shader-written absolute
positions do not. A constant-only override probe (no uniforms, no
textures, no instancing) finally isolated it.

**Fix:** `props.mat` vertex stage subtracts
`getUserWorldFromWorldMatrix()[3].xyz` before assigning
`material.worldPosition` (plus the `getCustom0()` read fix — a declared
material `variable` like partColor is NOT auto-filled from the CUSTOM0
attribute; without `getCustom0()` every part renders black).

## 2026-09-04 — One binary, one BasisU: two transcoder copies fail UASTC at transcode time

**Rule:** never link two copies of the BasisU transcoder into one binary
(KTX-Software's libktx vendors one; Filament's tnt bundle ships another).
Their `basist::` symbols silently interleave: with
`-Wl,--allow-multiple-definition` the first archive's definitions win, so
Filament's `ktxreader::Ktx2Reader` calls into KTX's newer-ABI basisu —
`init()` and `start_transcoding()` succeed, then `transcode_image_level()`
fails for every UASTC asset (returns COMPRESSED_TRANSCODE_FAILURE) with no
diagnostic in between. Symptom: "KTX2 transcode failed" for all textures
while the same file + same libs + same engine decode fine in a standalone
binary. If you must keep `--allow-multiple-definition`, keep it ONLY for
same-source duplicates (stb) and grep the link line for a second basisu.

**Incident:** all 6 terrain KTX2 assets (grass/cliff/snow/sand albedo+normal,
UASTC + zstd) white-fell-back in-game for every phase-7 round. Standalone
repros — basisu from source, the prebuilt tnt `libbasis_transcoder.a`, and
the full `Ktx2Reader`+`filament::Engine` path — all transcode the files OK;
delivered pak bytes were byte-identical (fnv1a). gdb in-game showed
`createTexture` OK, `start_transcoding` OK, `transcodeImageLevel` = 1 at
level 0. The ktxreader error ("Failed to transcode level 0") HAD been
printing all along, interleaved into the game logger's colored lines, and
grep for `WARN`-adjacent text missed it. The culprit was
`c-game/CMakeLists.txt`: KTX-Software listed before `${FILAMENT_LIBS}` with
`--allow-multiple-definition`, deliberately ("its definitions win the
duplicates") — which is exactly what broke the other user of the symbols.

**Fix:** drop KTX-Software from the link (nothing live used it: the Diligent
terrain pass is the only `imageLoadKtx` caller; grass cards are PNGs;
MainMenuGui already refused ktx2 for this reason). c-utils `Image.cpp` keeps
only stb decode; `imageLoadKtx` is a stub for ktx2 paths. One BasisU copy
(Filament's) remains — terrain albedo/normal maps load and the transcode
WARNs are gone. Also removed the ISO1600 exposure hack, which was masking
the blown-out white albedo in screenshots.

## 2026-09-04 — 600-frame screenshots are not md5-stable (wind runs on real dt)

**Rule:** do not gate acceptance on md5 equality of late-frame screenshots
when an animated shader (wind sway) is in frame. The wind phase accumulates
`utils::timer.dt` — real elapsed time — so two runs of the SAME binary drift
~1–3% of pixels by frame 600 even with everything else deterministic. Gate
on: exit code, log acceptance lines, and a pixel-diff magnitude against a
control (props-on vs props-off differs ~8–45%; run-to-run noise is <3%).
md5 equality CAN still appear (it did across rounds 6–8 when machine load
matched) — treat it as luck, not a contract.

**Incident:** phase-7 sign-off ran the verification command three times:
run 1 md5 58acb662 (identical to rounds 6/7), runs 2/3 differed (1.3% /
2.5% scattered, uniform over the frame) after only adding fonts to
pak_0_engine — initial suspicion fell on the font/atlas change, but the diff
had no text-shaped clusters; two runs of the identical binary reproduced the
same scatter. The round-6/7 md5 streak was same-load coincidence.

**Fix:** acceptance now quotes the pixel-diff numbers; the round-8 cleanup
verdict "strip is visually neutral" rests on the <3% uniform noise + all
log gates, not on md5.

## 2026-09-04 — Sourcing missing UI art: old-engine KTX2s and the shipped VF cover the gaps

**Rule:** before writing a "documented fallback" for a missing asset, check
whether the old engine ships the same art in KTX2 form and whether a
variable-font source is already in the pak. `basisu -unpack file.ktx2`
( Filament's tnt basisu build) writes the RGB and ALPHA planes as separate
PNGs for ETC1S sources — the auto-composited `*_rgba_*` PNG has FLAT alpha,
so recomposite RGB+A yourself before shipping. Static font weights can be
instanced offline from the variable font with
`python3 -m fontTools.varLib.instancer VF.ttf wght=N -o out.ttf` (verify
`OS/2.usWeightClass` + a render-ink ratio afterwards; instancer keeps the
VF's default-instance name, which is cosmetic).

**Incident:** task-17's `montserratLight.ttf` / `montserratBlack.ttf` /
`images/button.png` existed nowhere on disk (round 5 searched game-001-cpp,
game-001.bak, cpp-thirdparty) and the menu fell back to the ImGui default
font + a missing-strip ERROR. But `pak_0_engine/gui/images/button.png.ktx2`
was there all along (restored with the Sept-2 pak), and
`fonts/montserrat.ttf` is the full VF (wght 100–900, default instance Thin
— matching the existing GuiManager comment; a hand-parsed fvar that claimed
default 900 was wrong).

**Fix:** button.png recomposited (1024×128, black RGB + horizontal alpha
sheen 0→127→0 — the old menu's soft focus strip) into pak_1/images/ beside
logo.png (same pattern); montserratLight.ttf (300) + montserratBlack.ttf
(900) instanced from the shipped VF into pak_0_engine/fonts/ (Montserrat is
OFL). Menu now renders Montserrat Black rows with the soft strip behind the
focused row, zero font/texture warnings; the .ktx2 original stays in the pak
as provenance and the sourcing is documented at both load sites.

## 2026-09-04 — RADV LINEAR minification of sRGB8_A8 returns OPAQUE alpha: the grass-card alpha discard stops firing and mid-distance tufts render as solid tinted rectangles

**Rule:** never bind a LINEAR minification filter (`LINEAR` or `LINEAR_MIPMAP_LINEAR`) to an sRGB-format (`SRGB8_A8`) texture on RADV (Mesa) — LINEAR-minified samples come back with a corrupted OPAQUE alpha, so `alpha < 0.5` discards silently stop firing. The grass cards (1-level `.levels(1)`, sparse alpha cutout) must use NEAREST minification: on a 1-level image NEAREST minification is exactly "level-0 only" (the intended look), and the LINEAR _magnification_ filter is unaffected, keeping close-up edges smooth. If a texture genuinely needs LINEAR minification, convert sRGB→linear at load and upload as `RGBA8` — linear filtering of a LINEAR-format image is clean (uploading raw sRGB bytes as RGBA8 without conversion washes the colour).

**Incident:** user screenshot showed scattered flat striped teal/cyan-green rectangular patches on the ground amid correct grass tufts ("grass dont always look correct"). Headless A/B matrix (frame 400, all 7 card textures, full scatter): SRGB8_A8 + NEAREST/NEAREST ✅; SRGB8_A8 + LINEAR/LINEAR ❌ solid patches; SRGB8_A8 + LINEAR_MIPMAP_LINEAR (the old default) ❌; SRGB8_A8 + NEAREST min / LINEAR mag ✅; RGBA8 linear + any sampler ✅. Exonerated with direct evidence: assets (all 7 PNGs real 8-bit RGBA cutouts, byte-identical to the old engine), upload (direct vkCmdCopyBufferToImage, byte-exact — the NEAREST run matches the PNG alpha), discard gating (props.filamat disassembly: OpKill on a<0.5, flags ON, lodBias 0). The corruption is minification-specific, which is why the defect is "not always": magnified close-up cards and tiny far dots were fine, every mid-distance card was solid.

**Wiring:** `PropsRenderFilament::samplerCardClamp` (NEAREST min + LINEAR mag + CLAMP_TO_EDGE) bound to `cardTex`. The round-2 env diagnosis hooks (ENGINE_PROPS_CARD_SAMPLER / ENGINE_PROPS_CARD_FORMAT) were stripped once the config was verified — shipping a knob that could silently re-select the broken LINEAR-min config was a liability; the A/B matrix lives in .pi/ledger/notes.md round 2. Verified headless: frame-400 default view (mid-distance cards cutout, no solid patches) + ENGINE_CAMERA=propsground eye-level close-up (near-field tufts unchanged: cutout, no black, smooth edges). Long-term: file a Mesa RADV bug report (GitHub search found no existing issue).

## 2026-09-05 — Pose/placement matrices built in the update phase lag the aim by a frame: the character sits off-centre while the orbit camera rotates

**Rule:** a CPU-built per-frame transform (placement root, pose matrices) must be derived in the same frame — and after — the camera/view it is rendered with. In this engine the view matrix is rotation-only (world anchor = camera eye, built in `draw()`), and every pass re-expresses its f64 world state relative to that anchor. The glTF placement was an exception: it was rebuilt in `gltfUpdate` (Game.update, priority 0), which runs BEFORE the player system steps physics and re-aims the orbit camera — so the model was pinned to the previous frame's anchor/position while the view centred the current frame's aim. The two only agree when the camera and the character are stationary; the moment either moves (orbit drag + movement), the character renders one frame behind the aim and visibly drifts off screen-centre while rotating.

**Incident:** "when rotating the camera with mouse click, either with right or left button, player character is not on the center of the screen". Reproduced headless with a synthetic drag (Window.cpp `ENGINE_FAKE_DRAG=1` holds RMB and sweeps yaw, feeding the player system's real drag path) + `ENGINE_AUTO_RUN=1` + screenshot crosshair overlay: with a continuous orbit + running character the body sat ~35 px left of the screen centre (camDist 6 m). Exonerated everything else first: eve's local origin IS the feet (glb scene bounds y∈[-0.01, 1.39]), the orbit lookAt target (feet + 0.70) is exactly on the camera forward axis so it projects to screen centre by construction, the placement matrix yaw-fixed-point (feet) and the f32 anchor-relative translation were both verified correct, and a non-dragging follow shot measured centred — the offset only appears when the aim moves, i.e. a timing bug, not a geometry bug.

**Fix:** `c-engine/gltf/GltfDiligent.cpp` — `gltfUpdate` now only advances/samples the animation clips; the pose-matrix rebuild moved to a new `poseRebuild()` called from `worldDraw` (render pass, after the view was derived from this frame's camera), so the placement root always sees the same anchor as the view. The old "no clip playing" lazy-rebuild path is subsumed (pose rebuild now runs every draw, which is what keeps a static pose tracking the moving anchor). Verified: fake-drag + auto-run shots at two frames/orbit angles show the waist on the screen crosshair (was ~35 px off); idle/scripted-camera runs unchanged.
