# Lessons — Filament era archive

Filament-API-specific entries; the Filament render path was removed 2026-09-05 (implementation survives in git history as the look/parity reference). Knowledge here only re-activates if that code is touched again. Backend-agnostic entries from the same era stay in `lessons/2026-09-04.md` and `lessons/2026-09.md`.

---

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
