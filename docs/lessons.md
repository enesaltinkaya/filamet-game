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
