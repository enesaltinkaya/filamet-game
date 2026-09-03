# Plan

Phase 6 of plans/azgaar-terrain.md: implement the **Diligent terrain render pass** — an HLSL mirror of the completed Filament pass (phase 5), sharing the CPU lattice and the "surface is a pure function of (x,z)" invariant.

## Strategy

- Study the reference: `c-engine/renderer/filament/HeightmapTerrainFilament.{h,cpp}` (686 lines) and `materials/terrain.mat` (415 lines), plus how `DiligentRenderer.cpp` (367 lines, currently a basic scene) is wired into the backend selection and where the Filament terrain pass is invoked from the game side.
- Create `c-engine/renderer/diligent/HeightmapTerrainDiligent.{h,cpp}` mirroring the Filament one: per READY tile, CPU-generated 256² lattice corners (world pos + border-aware stencil normal) + 255-segment shared IBO, uploaded as VBO/IBO, drawn with a pipeline the DiligentRenderer exposes. The CPU lattice code path must stay identical to the Filament one (same `readyStamp` cache key, LRU eviction, no re-upload of unchanged tiles).
- Port `terrain.mat` look to an HLSL **pixel** shader (all look work in the pixel stage; vertex stage is a thin transform using the precomputed normals — no VS texture fetches, no implicit lattice enumeration). Reuse the already-registered climate/biome textures from world load.
- Shaders are **pre-compiled at build time** per plans/diligent-migration.md: HLSL → SPIRV via the glslang chain already linked in c-engine (see the "shader compiler chain" comment in c-engine/CMakeLists.txt), or a dxc/spirv-build step in CMake if that fits the existing pattern better; do not rely on runtime compilation for the terrain pass.
- Wire into whichever backend the build currently selects, and verify visually with `ENGINE_SCREENSHOT` (seams, look, streaming) in addition to the build command.

## Verification

Verification: ./scripts/build.sh

Baseline commit: e547d56995d873a6e408b128ae9271d064f50917 (clean)
