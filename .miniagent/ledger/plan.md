# Bloom (DiligentFX) — manager plan

## Strategy

The deliverable of this task is a detailed implementation-plan document (the code-inspection
is the input to it): write `plans/bloom-diligentfx.md` mirroring the structure of the existing
`plans/ssao-diligentfx.md` precedent (current state, gaps, approach/decisions, concrete steps
with file anchors). Workers first inspect the three required source areas under
`/home/enes/Projects/c/cpp-thirdparty/diligent/git/`: the `DiligentFX/PostProcess/Bloom` module
(README, `interface/Bloom.hpp`, `src/Bloom.cpp`, plus `PostProcess/Common` for the shared
framebuffer/mip infrastructure), the integration shape from `DiligentSamples/Tutorials/
Tutorial27_PostProcessing` and the samples/Hydrogent wiring that consume `PostFXContext`, and
the `DiligentFX/Radient` high-level pipeline to see how bloom is composed with tonemap/
auto-exposure there. The plan must then map those findings onto the engine's verified
integration surface — TaaDiligent's existing `PostFXContext`, the `sceneColorTex` RGBA16F HDR
offscreen chain, the post-TAA/pre-backbuffer application point and tonemap in DiligentRenderer,
the `SsaoDiligent` thin-module C pattern, the `ssao` settings-flag flow to copy, and the
`libDiligentFX.a` link path — identifying concrete gaps (no bloom settings flag, HDR/tonemap
interaction, where the bloom SRV is consumed) and listing implementation steps, parameters
(Intensity/Threshold/SoftThreshold/Radius), and risks.

## Verification

Verification: ./scripts/build.sh
