# Lessons

Dated log of hard-won debugging knowledge. One entry per incident, rule first.

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
