# Plan — SSR reflections from DiligentFX

Add DiligentFX's `ScreenSpaceReflection` (prebuilt `libDiligentFX.a`, symbols verified exported in
`/media/extra/Projects/c/cpp-thirdparty/diligent/git/build-linux/DiligentFX/libDiligentFX.a`) to the
Diligent render path, mirroring the proven SSAO integration (`plans/ssao-diligentfx.md`, code in
`SsaoDiligent.*` called from `TaaDiligent.cpp`): a thin `SsrDiligent` module running on the existing
shared `PostFXContext`, fed sceneColor SRV, world depth SRV, the existing world-pass normal buffer
(RGBA16F, added for SSAO), TAA motion vectors, and a roughness/material input packed into the
normal buffer's alpha channel (extend world shaders in `GltfDiligent.cpp` / `SplatTerrainDiligent.cpp`
to write roughness there, `RoughnessChannel=3` or per fxh contract). SSR `Execute` runs inside
`taaWorldResolve` after `postFXContext->Execute` (same reason as SSAO: it consumes context-produced
reprojected/previous depth + closest motion), under a `ScopedDebugGroup "ssr"`; the radiance output
is composited into the scene color after the TAA resolve next to the existing `aoCompositeApply`
composite, gated by reflectance/roughness so only glossy surfaces show it. Settings follow the SSAO
idiom end-to-end (`GraphicsSettings.ssr`, settings persistence, DebugGui + SettingsGraphicsGui
toggles, `applyGraphicsSettings` forwarding), default ON with conservative attribs
(`RoughnessThreshold`, `FEATURE_FLAG_NONE` or HALF_RESOLUTION if perf demands).

Reference wiring to mirror: `DiligentFX/Hydrogent/src/Tasks/HnPostProcessTask.cpp` lines ~600-830
(PrepareResources every frame, Execute with RenderAttributes{color, depth, normal, material,
motion}, composite g_SSR in the post shader) and `PostProcess/ScreenSpaceReflection/` (hpp/cpp +
`ScreenSpaceReflectionStructures.fxh` attribs).

Constraints from AGENTS.md: no git writes (read-only `rev-parse`/`status`/`restore` for this
scaffold only), no comments in code, `ENGINE_HIDDEN_WINDOW=1`, `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json`, do not move the parked player/camera.

Verification: ./scripts/build.sh
Baseline commit: e7208cfbf160bedf34d0b95a76121c8d4606ed38 (clean)
