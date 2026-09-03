# Lessons

Dated log of hard-won debugging knowledge. One entry per incident, rule first.

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
