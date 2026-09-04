# notes

## manager setup (context for workers)

- User's screenshot: /var/home/enes/Screenshots/Screenshot_20260904_205244.jpeg — defect: scattered FLAT striped teal/cyan-green rectangular patches on the ground (bad grass cards), while 3D grass tufts render fine.
- Headless repro (identical framing to user shot): `VK_ICD_FILENAMES=/usr/share/vulkan/icd.json ENGINE_AUTOTEST=enter ENGINE_SCREENSHOT_FRAME=400 ENGINE_SCREENSHOT=/tmp/xxx.jpg ./build/c-game/c-game` — baseline capture saved at .pi/ledger/repro_baseline.jpg
- Grass scatter: c-game/game/azgaar/AzgaarProps.cpp (+AzgaarPropMesh.*); draw: c-engine/renderer/filament/PropsRenderFilament.cpp; shaders: c-engine/data/pak_0_engine/shaders/pass/azgaar_props/azgaar_props.{vert,frag}
- Grass scatter is seed-deterministic (mapSeed=0x5d28ace1) → same patches every run.
- Old engine reference: /home/enes/Projects/c/game-001-cpp
- Note: default screenshot frame (3) is too early — terrain tiles stream in slowly; use ENGINE_SCREENSHOT_FRAME=400.

## brainstorm

## Core difficulty

The visible symptom ("flat, striped, teal rectangular patches") misdirects toward billboard/orientation logic, but the grass-card geometry is provably vertical end-to-end; the real failure axis is the alpha channel of the card textures (asset vs upload vs sampling), and it manifests only on some variants (mixed good/bad instances in one frame). There is no runtime introspection into per-variant texture state, so the diagnosis must isolate which of 7 lazily-resolved card textures is broken and where (asset, forced 4-channel decode, Filament upload, or mip sampler) the alpha goes away.

## Reductions / key lemmas

1. **Geometry is exonerated (proven, not assumed).** `buildGrassCard` (c-game/game/azgaar/AzgaarPropMesh.cpp) builds two perpendicular quads in the XY and ZY planes (vertical). The only transforms between mesh and pixels are: uniform scale, yaw rotation about Y (props.mat vertex stage: `rot` is a standard Y-rotation; GLSL column-major checked), wind XZ drift, and a pure translation renderable transform (`tcm.setTransform(..., mat4f::translation(...))`, and `material.worldPosition` bypasses the object transform). Nothing in the current code can tilt a card into the ground plane. The "lying flat on the ground" reading of the user screenshot is foreshortening: a wide SOLID card viewed by a camera pitched ~20° down projects as a perspective parallelogram hugging the ground. The defect is solid + tint, not orientation.
2. **Solid card ⇒ sampled alpha ≥ 0.5 everywhere.** The material's discard (`if (t.a < 0.5) discard;`, c-engine/renderer/filament/materials/props.mat fragment stage) is flag-gated on bit 0 of `meshB.w`; grass ranges carry it (azgaarPropsSpeciesRenderFlags → ALPHA_TEST|DOUBLE_SIDED, set via Game.cpp propsRegisterRender → propsRenderSetVariants). So the discard logic is on; if a card renders solid, the uploaded/sampled texture's alpha is opaque.
3. **The loader cannot detect missing alpha.** `utils::imageLoad` (c-utils/image/Image.cpp, `imageLoadFromData`) calls `stbi_load_from_memory(..., 4)` (forces RGBA) and hard-codes `image.channels = 4`. A PNG with no alpha channel (or an opaque white background) decodes with A=255 and passes `loadGrassTexture`'s `img.channels != 4` guard — which is dead code, since channels is always 4. `grassMeasureBottomV` (AzgaarProps.cpp) likewise silently returns bottomV=1.0 for such files. Result: a flattened asset silently becomes a fully opaque Filament texture with zero warnings.
4. **Per-variant texture resolution makes the defect "scattered."** `resolveVariantTexture` lazily loads one texture per (species,variant) and caches it (`texResolved`); one bad PNG ⇒ every instance of that variant in every tile is bad. The scatter picks a variant per instance deterministically (propsRand 0xD8+gi), so bad instances appear scattered across the ground. The tuft-blade stripes visible INSIDE the patches prove the real texture content is being sampled (not the 1×1 white fallback — that would give a uniform solid rectangle), i.e. the texture loaded but its alpha is broken.
5. **Old-engine parity narrows the suspect to the pak assets.** The old engine (game-001-cpp) uses the same `kGrassTexPaths` (c-game/data/pak_1/images/grass-textures/), the same forced-4-channel stb decode (its c-utils/image/Image.cpp has the identical pattern), and the same 0.5 alpha test (azgaar_props.frag `stochasticAlphaDiscard`). So if the assets are byte-identical, the old engine would have rendered the same solid cards; the new pak's PNG copies are the one unverified link (likely flattened during pak migration — or the "expected" look was the design intent, not the old engine).
6. **The "teal" tint is explained by background × biome tint.** Per-instance tint = biome grass colour (`.map` biome table via `azgaarWorldBiomeColor`) × patch noise × jitter (propsVegetationColor). A white PNG background × a dry-biome desaturated green reads as cyan/teal; the sparse dry-grass blades over it read as "stripes." The dense dark green-grass variants × the same tint still read as tuft blobs, which is why the defect is "not always."

## Candidate approaches

A. **Asset audit + asset repair (recommended lead).** Inspect the 7 grass PNGs in the new pak AND the old engine's copies: IHDR width/height/bit-depth/color-type (byte 25), PLTE/tRNS chunk presence, and decoded min/max alpha. If the new pak's files (or all of them) lack a real alpha cutout, restore the old files (if alpha'd) or re-export the PNGs with transparency, rebuild, re-run the headless screenshot. Main risk: old copies may also lack alpha (then it's an asset-authoring gap, not a regression — fix by re-exporting with a transparent background or white-keyed alpha, which is a content task, not a code task). Effort: small (audit) + small-medium (repair + verify).

B. **Runtime alpha readback / debug material (fallback if assets are clean).** If the audit shows all PNGs carry proper alpha, find where it's lost: temporarily emit the sampled `cardTex` alpha as the fragment colour in props.mat (debug parameter), or read back one uploaded Filament texture, run headless at the repro camera, screenshot. Also explicitly test the single-level texture + `LINEAR_MIPMAP_LINEAR` sampler combination (the material's own comment warns mips would produce exactly this symptom; verify Filament truly clamps to level 0 here). Main risk: material surgery touches the shared props material — keep it behind a debug flag and revert. Effort: small-medium.

C. **Pipeline hardening (do regardless of A/B outcome).** In `PropsRenderFilament::loadGrassTexture`: compute min/max alpha over the decoded pixels and `utils::warn` when minAlpha==255 ("opaque grass card texture — no alpha cutout, card will render as a solid rectangle"); replace the dead `channels != 4` guard with a real check; switch the `cardTex` sampler from `samplerLinearMipmapClamp` to `samplerNearestClamp` (the material already intends level-0 sampling for a 1-level texture — removes the mip-sampler ambiguity entirely); log which variant texture resolved (or fell back to white) per tile build. Main risk: none really — pure diagnostics/robustness; the NEAREST switch changes nothing for a 1-level texture. Effort: small.

D. **Tint / colour-space side check (cheap, parallel).** Pull the grass biome's colour from the active `.map` file and confirm `propsVegetationColor` output magnitude/hue (sRGB-vs-linear handling of `biomes[].color`), to rule out an independent tint bug that would make even cutout dry-grass look teal. Main risk: low value if A already explains the teal (background × tint). Effort: trivial.

## Recommended approach

A + C (with D in passing). The observed defect — solid, un-cutout, tinted rectangles on exactly the variants whose PNGs carry a pale/opaque background — is fully explained by opaque card textures (lemmas 2–4), the geometry is proven innocent (lemma 1), and every code link after the PNG file is verified correct; the pak assets are the one unverified link in the chain. For A to work: the audit must show at least the dry-grass PNGs (and probably some green ones) lack a usable alpha channel in the new pak. If the audit instead shows all 7 PNGs carry proper cutout alpha, pivot to B to locate the alpha loss at runtime before touching assets. C should ship either way because today a missing-alpha asset fails with zero diagnostics (lemma 3).

## Proposed tasks

1. **Grass PNG alpha audit (no build).** For each of the 7 files in `c-game/data/pak_1/images/grass-textures/` and the matching files in `/home/enes/Projects/c/game-001-cpp/c-game/data/pak_1/images/grass-textures/`, report: IHDR width/height/bit-depth/color-type, PLTE/tRNS presence, decoded min/max alpha, and file size. Deliverable: a per-file table (new vs old). Success = a definitive yes/no per file on "has a real alpha cutout."
2. **Branch on task 1:** if new-pak PNGs lack alpha and old ones have it → copy the old files into the new pak; if neither has it → re-export the 7 PNGs with a transparent background (white-keyed alpha is acceptable only as a stopgap — record it). Then `./scripts/build.sh` and run the plan's headless verify command; success = /tmp/verify_grass.jpg shows no solid rectangular grass patches (tuft silhouettes with visible ground behind them everywhere), good tufts unchanged, no new defects.
3. **Props pass hardening (independent of 1–2).** In `c-engine/renderer/filament/PropsRenderFilament.cpp::loadGrassTexture`: min/max-alpha validation with a loud `utils::warn` for opaque card textures; replace the dead `channels != 4` guard; use a NEAREST sampler for `cardTex`; log the resolved (or fallback) texture per variant at tile build. Verify: rebuild + one headless run; the warning lines identify exactly which variants are opaque.
4. **Only if task 1 shows assets have alpha:** runtime alpha readback — temporary props.mat debug parameter emitting sampled `cardTex` alpha as fragment colour (or a one-off Filament texture readback of one variant), headless run at the repro camera, screenshot; identify the loss point (upload format, mip sampler, or discard gating). Keep the debug hook behind an env var and revert it after diagnosis.

## round 1 — task 1: grass card PNG alpha audit

**Verdict: NO grass PNG lacks a real alpha cutout. All 7 files in BOTH engines are 8-bit RGBA (IHDR color-type 6) with minA=0/maxA=255, 44–70% fully-transparent pixels, and NO PLTE/tRNS (not applicable — ct=6). The old-engine and new-engine copies are BYTE-IDENTICAL (md5 match on all 7) → "old vs new asset" is a null comparison; the defect cannot be an asset regression from pak migration. Pivot to Approach B (runtime alpha loss) per plan; Approach A's repair step is unnecessary.**

Per-file table (new == old, byte-identical; decoded stats verified by two independent decoders — manual PNG unfilter and PIL 12.3.0, exact agreement):

| file | size B | dims | IHDR | PLTE/tRNS | minA/maxA | a=0 % | 0<a<255 % | a>=128 % (kept by 0.5 discard) |
|---|---|---|---|---|---|---|---|---|
| green-grass-1.png | 244853 | 500x373 | 8-bit RGBA (ct=6), non-interlaced | none | 0 / 255 | 65.9 | 19.0 | 23.2 |
| green-grass-2.png | 167704 | 500x268 | 8-bit RGBA (ct=6), non-interlaced | none | 0 / 255 | 65.0 | 26.7 | 27.3 |
| green-grass-3.png | 360964 | 500x457 | 8-bit RGBA (ct=6), non-interlaced | none | 0 / 255 | 44.2 | 17.1 | 47.2 |
| green-grass-4.png | 348730 | 500x671 | 8-bit RGBA (ct=6), non-interlaced | none | 0 / 255 | 70.4 | 23.4 | 19.4 |
| green-grass-with-plant.png | 382053 | 500x556 | 8-bit RGBA (ct=6), non-interlaced | none | 0 / 255 | 54.5 | 25.3 | 39.6 |
| dry-grass-1.png | 282389 | 500x477 | 8-bit RGBA (ct=6), non-interlaced | none | 0 / 255 | 57.7 | 35.9 | 33.3 |
| dry-grass-2.png | 264231 | 500x483 | 8-bit RGBA (ct=6), non-interlaced | none | 0 / 255 | 67.7 | 25.6 | 20.1 |

Notes for later workers:
- All files have real, wide-range alpha with heavy soft edges (20–36% of pixels at 0<a<255) — the cutout is a soft/feathered mask, not a hard stencil. A 0.5 discard test will render these as tufts with soft edges; it can NOT produce a solid rectangle.
- green-grass-3 is the densest card (47.2% of pixels above the keep threshold) — densest silhouette but still a real cutout; not the culprit by itself.
- Runtime loads the LOOSE files in c-game/data/pak_1/images/grass-textures/ (utils::dataManagerFileExists/imageLoad with pak-relative paths; no binary pak archive exists — pak_1 is a directory). So the audited files ARE the runtime assets.
- Consequence for task 3: "restore/re-export alpha-less PNGs" is a no-op — nothing to restore. If the flat teal patches persist after task 5's diagnosis, the fix is code-side (upload/sampler/discard-gating), not asset-side.
- Consequence for task 4: the min/max-alpha warn will (correctly) fire on NONE of the current 7 files; keep it for future-asset protection.

## round 2 — task 5: runtime grass-card alpha loss LOCATED

**Verdict: the alpha is lost in the RADV driver's LINEAR minification filter for the sRGB card texture.** On RADV Mesa 26.2.1 (NAVI31 / RX 7900 XTX), a minified (screen size < texture size) sample of a `SRGB8_A8` (VK_FORMAT_R8G8B8A8_SRGB) texture with a LINEAR *min* filter comes back with an OPAQUE alpha (~1.0), so the fragment's `t.a < 0.5` discard never fires and every minified grass card renders as a solid, striped, biome-tinted rectangle. Magnified cards (LOD ≤ 0, mag filter) and NEAREST-filtered cards are correct — that is exactly why the defect is "not always" (mid-distance cards solid, close-up tufts fine, tiny distant dots too small to notice).

### A/B matrix (headless, ENGINE_SCREENSHOT_FRAME=400, identical camera; all 7 card textures, full scatter)

| cardTex upload | sampler min/mag | result |
|---|---|---|
| SRGB8_A8 (current default) | NEAREST / NEAREST | ✅ correct cutout everywhere |
| SRGB8_A8 | LINEAR / LINEAR (no mip) | ❌ solid patches |
| SRGB8_A8 | LINEAR_MIPMAP_LINEAR / LINEAR (current default) | ❌ solid patches (baseline) |
| SRGB8_A8 | NEAREST min / LINEAR mag | ✅ correct cutout everywhere |
| RGBA8 linear (same pixels) | LINEAR / LINEAR | ✅ cutout (RGB slightly washed out) |
| RGBA8 linear (same pixels) | LINEAR_MIPMAP_LINEAR / LINEAR | ✅ cutout |

⇒ The loss is NOT the mipmap dimension (LINEAR_MIPMAP_LINEAR on a 1-level texture is fine on a linear-format image), NOT the upload, NOT the discard gating. It is specifically the **LINEAR minification filter of the sRGB-format image**: RADV's sRGB minification path returns a corrupted (opaque) A channel. Magnification (mag) is unaffected.

### Ruled out with direct evidence

1. **Assets**: round 1 — all 7 PNGs are 8-bit RGBA with real cutout alpha (minA=0/maxA=255), new==old byte-identical.
2. **Upload**: `filament::Texture::Builder(SRGB8_A8).build()` + `setImage(RGBA, UBYTE)` takes the *direct* `vkCmdCopyBufferToImage` path in this Filament build (VulkanTexture.cpp `updateImage`: `getVkFormat(host)` == `getVkFormatLinear(deviceFormat)` for R8G8B8A8_SRGB, so no blit/conversion); byte-exact copy. Corroborated at runtime: the NEAREST run's cutout matches the PNGs exactly, i.e. the stored GPU texels carry the correct alpha.
3. **meshB.w gating**: disassembled the compiled `props.filamat` (DIC_SPIR chunk = smolv-compressed SPIR-V; decoded 56 blobs with a smolv tool, spirv-dis). The fragment stage does exactly: `flags = int(meshB.w + 0.5)`, `mod(flags,2) > 0.5` → `OpImageSampleImplicitLod(cardTex, uv, Bias)` → `a < 0.5 → OpKill`, bit2 radial test second. The Bias operand is the per-view `lodBias` uniform (TAA option) — the game enables no TAA/DSR, so bias = 0.0. Grass tufts get flags = ALPHA_TEST|DOUBLE_SIDED = 3 (AzgaarProps.cpp `azgaarPropsSpeciesRenderFlags`). Gating is ON and correct.
4. **flipUV/UV**: material sets `flipUV:false`; the NEAREST run's cutout silhouette is upright and correctly placed — UV path is fine.

### Driver mapping detail (this Filament build, Vulkan backend)

`fvkutils::getFilter` (Conversion.cpp) maps Filament min filters to: NEAREST→VK_FILTER_NEAREST, LINEAR→VK_FILTER_LINEAR, LINEAR_MIPMAP_LINEAR→VK_FILTER_LINEAR; `getMipmapMode`: LINEAR_MIPMAP_LINEAR→MIPMAP_MODE_LINEAR, others→NEAREST; `getMaxLod`: mipmap filters→CLAMP_NONE, non-mip→0.25. So "linear" (min LINEAR, mipMode NEAREST) and "mipmap" (min LINEAR, mipMode LINEAR) both use VK_FILTER_LINEAR for minification — both corrupt; only VK_FILTER_NEAREST minification is clean. (Also explains why Filament's "0.25 maxLod disables mips" trick is irrelevant: the filter itself is the trigger.)

### Fix options for task 3 (all verified by the runs above)

- **Cheapest, verified, zero color change**: bind `cardTex` with the existing `samplerNearestClamp` (NEAREST/NEAREST). Correct cutout + unchanged color (sRGB decode still applied at fetch). Cost: slightly aliased card edges when a card magnifies past 500 px on screen (rare; cards are small). The material comment already asserts level-0/nearest is the intended look.
- **Best quality**: convert the PNG sRGB→linear at load and upload as linear `RGBA8` (then any LINEAR sampler is safe, edges smooth, colors correct for the PBR lit model). Note: uploading the raw sRGB bytes as RGBA8 *without* conversion gives a washed-out look (visible in /tmp/grass_b_rgba8_linear.jpg) — the conversion step is what makes it right.
- Long-term: file a Mesa RADV bug (linear minification of R8G8B8A8_SRGB returns opaque alpha). GitHub search API turned up no existing issue (0 results for "radv srgb alpha filter" / "radv srgb8_alpha8") — none filed, needs a proper report with this matrix.

### Debug hooks left in place (inert by default, useful for task 3 verification)

`c-engine/renderer/filament/PropsRenderFilament.cpp`:
- `ENGINE_PROPS_CARD_SAMPLER=nearest|linear|maglin` — selects the cardTex sampler (default = current LINEAR_MIPMAP_LINEAR); logs the choice once.
- `ENGINE_PROPS_CARD_FORMAT=linear` — uploads the cards as RGBA8 instead of SRGB8_A8 (default unchanged); logs the choice once.

Reference screenshots in /tmp: `grass_a_mipmap.jpg` (baseline defect), `grass_a_nearest.jpg`, `grass_a_linear.jpg`, `grass_b_rgba8_linear.jpg`, `grass_b_rgba8_mipmap.jpg`, `grass_c_maglin.jpg` (all non-mipmap-sampler + linear-format variants clean).

Misc: the filamat binary in pak_1 is smolv-compressed SPIR-V (DIC_TEXT + DIC_SPIR chunks; writer = libs/filamat DictionarySpirvChunk, reader = libs/filaflat DictionaryReader + smolv). Extract/disassemble recipe worked: parse chunks (u64 type + u32 size), decode each DIC_SPIR blob with `smolv::GetDecodedBufferSize/Decode`, then `spirv-dis`. The runtime material is PRECOMPILED — editing `c-engine/renderer/filament/materials/props.mat` does nothing until the .filamat is rebuilt with matc (available at cpp-thirdparty/filament/git/build-linux/tools/matc). Task 3's fix does not require material surgery (C++ side only).

## round 3 — task 3 (+6): grass-card alpha fix APPLIED and VERIFIED

**Verdict: done.** cardTex is now bound with the verified-clean **LINEAR mag + NEAREST min** sampler (the preferred config), hardcoded; the round-2 env diagnosis hooks were **STRIPPED** (task 6 decision: both `ENGINE_PROPS_CARD_SAMPLER` and `ENGINE_PROPS_CARD_FORMAT` removed — the diagnosis is complete, the A/B matrix is preserved here, and shipping a knob that can silently re-select the broken LINEAR-min config is a liability).

### Changes

- `c-engine/renderer/filament/PropsRenderFilament.cpp`:
  - New `samplerCardClamp` (MinFilter NEAREST, MagFilter LINEAR, CLAMP_TO_EDGE) bound to `cardTex` in `buildTile`; comment explains the RADV bug + pointer to lessons/ledger.
  - Removed `cardTexSampler()` + `ENGINE_PROPS_CARD_SAMPLER`, `ENGINE_PROPS_CARD_FORMAT` hook code and its one-shot log lines; removed now-unused `samplerLinearClamp`/`samplerLinearMipmapClamp` constants (`HeightmapTerrainFilament.cpp` has its own static `samplerLinearClamp` for mipmapped terrain textures — untouched, correct there).
  - `loadGrassTexture` always uploads SRGB8_A8 level-1 again (format hook gone).
  - `propsRenderFilamentStats` GPU accounting fixed: cards are 1-level → width*height*4 bytes (was x4/3 assuming a mip chain — stale since the levels(1) revert).
- `docs/lessons.md`: new dated entry "RADV LINEAR minification of sRGB8_A8 returns OPAQUE alpha" (rule first, incident, wiring).
- Rebuild: `./scripts/build.sh` clean (only pre-existing third-party header warnings).

### Verification (both captures, frame 400, no env sampler/format vars — default path)

1. **Default view** `/tmp/verify_grass_default.jpg`: diffed against round-2 refs. vs `grass_a_mipmap.jpg` (broken config): global mean 1.43, with the top diff blocks at EXACTLY the defect coordinates — (416,416) 169.2, (448,416) 158.3, (1984,416) 153.7, (1792,416) 150.7, (1952,800/832) ~149, (2496,512) 148.2 — i.e. the mid-distance solid-teal patches are the only significant changes (they now render as sparse cutout speckle, ground visible through them). vs `grass_c_maglin.jpg` (the verified-clean config): global mean 0.71 = pure wind noise. `repro_baseline.jpg` ≈ `grass_a_mipmap.jpg` (0.50) — confirmed the ledger baseline IS the broken config.
2. **propsground eye-level close-up** `/tmp/verify_grass_propsground.jpg`: true vantage (HUD 28464, 14.29, -10436 — the densest-props point at 28 km). Near-field tufts: proper cutout, smooth edges (LINEAR mag), no black faces; mid-ground: sparse level-0 speckle with ground visible — no solid cards anywhere.

### Gotcha found: persisted camera DB clobbers ENGINE_CAMERA framing

`ENGINE_CAMERA=propsground` initially did NOTHING visible: `FlyingCameraSystem::added()` restores the last saved free-camera view from the SQLite `camera` table (`build/c-game/data/db/db.db`) AFTER loadWorld applies the scripted framing, and the player orbit derives from the same row. The persisted row (-104.7, 1.7, -101.2) made both runs render the same orbit camera. Fix for verification only: backed up db.db, `DELETE FROM camera` (python sqlite3), ran the capture (log showed `window @ tile(13,-6) anchor(28464,-10436)`, 25 tiles built), restored the original db.db. **For future headless camera-framed captures: expect the saved camera row to override ENGINE_CAMERA unless the camera table is cleared** (or add an env to skip the restore).

### Remaining for other workers

- Task 4 (loadGrassTexture hardening: min/max-alpha warn, dead channels!=4 guard, per-variant resolve log) still pending — independent of this fix; note the loader comment block above `loadGrassTexture` is still accurate.
- Long-term: file a Mesa RADV bug report (LINEAR minification of R8G8B8A8_SRGB returns opaque alpha); no existing issue found. The A/B matrix + driver mapping detail is in the round-2 section above.
