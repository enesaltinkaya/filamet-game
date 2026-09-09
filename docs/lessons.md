# Lessons

Index of hard-won debugging knowledge — one entry per incident, rule first. Full entries: `lessons/<date>.md` (active engine) and `lessons-filament-archive.md` (Filament-API-specific, backend removed 2026-09-05).

New entries go into the dated file, kept lean: rule + diagnostic fingerprint (VUID id, error string, measured signature) + one-line incident.

## 2026-09-10 — [entries](lessons/2026-09-10.md)

- A pass that runs its VS work but rasterizes zero primitives is a CLIP-TRANSFORM problem, not a "draw missing" one: measure (RasterizedPrimitives + post-VS clip data), don't re-derive the math on paper — the PBR lit pass clips the character within ~20 m of the camera (rpr=0, VSInvocations normal) while the same mesh rasterizes at 60 m+ and in the shadow pass at every distance; found verifying the azgaar removal, cause unresolved — don't build close-range validation vantages on the lit pass until understood
- Removing a world render pass also removes hidden consumers of its state: the shadow module's per-cascade cbuffer + ENGINE_SHADOW_NO_* flags existed only for the world shadow draws, but the PBR receiver cascade pick read the player position from the props pass' wind-state getter — re-source player state from engine::playerGetFootPos

## 2026-09-09 — [entries](lessons/2026-09-09.md)

- treegen CONES conifer default renders a flat umbrella, not a spruce: needs `coneCount >= 3` (stacked mini-pyramid) + `tipConeHeight >= tipConeRadius` + low `startFrac` (~0.18) + upright `angleSpread` (~0.55); stock config (flat discs, splayed branches, top-half `startFrac 0.5`) reads as a "mushroom on a stick" — fingerprint: wide flat disc on a bare trunk in side-view OBJ dump; incident: "trees around the character look weird" (player in conifer-only Taiga, every near tree was the malformed conifer) — fixed by retuning `configConifer`/`configConiferFar`

- Porting ez-tree presets to `treegen`: convert units, don't transcribe (twist is per-section; gnarliness divisor differs between ez's `max(1, 1/sqrt(r_meters))` and treegen's `g / max(0.15, sqrt(r_unit))`; baseRadius = preset radius / preset total height). Fingerprint of an unconverted port: stocky trunk, helical twist, "candelabra with pom-poms" crown. Incident: "deciduous trees don't look like ez-tree default ash medium" — fixed by converting the preset numbers + measuring card size from the reference GLB
- `treegen` `maxTris` must cover branches + tips×cards×8 or `Gen::room()` silently drops the last branches' leaf cards — fingerprint: build log shows the variant at exactly `maxTris` (`deciduous/N=20000` after a card-count bump that was 19384 before)
- Fast tree A/B: `ENGINE_AZGAAR_PROPS_MESH_DUMP=1` + reference GLB (`tree_LOD0.glb`) in headless Blender; `import_scene.gltf` already converts to Z-up (don't add another 90° X-rotation), normalize both to unit height, measure card size from leaf-mesh triangles (median longest edge ≈ size×√2)
- `treegen` final-level `continuation` spines are sub-pixel-radius waste that still carry full parent-resolution bands + their own cards (near-LOD: 7,296 of 28,344 tris/variant, zero silhouette) — turn them off before any card cut; fingerprint: tri-count harness A/B full vs `lev[levels-2].continuation=false` (the `--spine` flag), a ~26 % block invisible in screenshots
- Leaf-card levers: `cardCount`/tip = canopy SOLIDITY, `cardSize` = AREA — at ≤3 cards/tip the clump goes visibly open even at per-tip-area parity (count×size²); take the minimum card count that passes the A/B density at 100-200 m (far: 3 failed, 4 @ 0.071 held; near: 6 @ 0.055 rejected as too sparse, 9 @ 0.070 held)
- TAA A/B noise floor: two identical-geometry runs differ by ~1.8 % px Δ>30 — always run a same-build control pair and only count pixel-diff regions above that floor as real (heatmaps localize the change rows)
- RADV per-event GPU durations overlap (per-pass event sum ≫ wall-frame GPU time; ±25 % replay jitter even on bit-identical workloads): across captures compare pass SHARES and same-method A/B deltas, use RasterizedPrimitives/SamplesPassed for draw-level conclusions — never absolute ms

## 2026-09-08 — [entries](lessons/2026-09-08.md)

- `ShaderCreateInfo.Source` is inline source, the stream-factory path is `FilePath` — a filename in `Source` compiles as code (":1: 'declaration' : Expected")
- FX explicit sampler variables are named `<texture>_sampler` — `AddImmutableSampler` with the bare texture name leaves `g_EnvironmentMap_sampler` unbound ("No resource is assigned to static shader variable ... Implicit signature" + VUID-08114 per draw)
- Envmap prefilter cubemap mips = full chain for the dim (256² → 9; 16 trips VUID-mipLevels-00958/02255 and the roughness→mip ramp)
- IBL sheen too strong: the port dropped the old engine's specular-only IBL damp (0.5) — the prefilter pass now reads a spec-scaled env copy (`envSpecTex`); and the HDRI's own sun disk double-counts the analytic sun (aligned ~8° apart): the spec env copy is luminance-clamped (`ENGINE_IBL_SPEC_CLAMP`, default 100) so the disk no longer prefilters into a sheen spike (4.7× the sans-disk value at roughness 0.54, lobe center). Tune with `ENGINE_IBL_SPEC_INTENSITY` / `ENGINE_IBL_INTENSITY` / DebugGui IBL intensity

## 2026-09-07 — [entries](lessons/2026-09-07.md)

- Camera-anchored casters must rebuild their placement with THIS frame's anchor before the shadow pass (`poseRebuild()` in `gltfDiligentShadowDraw`): the shadow pass runs before `worldDraw`, so the player caster was pinned to last frame's anchor — displaced by dEye (m/s during an orbit drag), and TAA's history makes the detached shadow converge for ~0.5 s after the drag stops
- RenderDoc capture on Wayland: the capture layer filters `VK_KHR_wayland_surface` from the instance extensions (local RenderDoc build is wayland-OFF), but Diligent's Linux build requires it unconditionally — segfault at `CreateDeviceAndContextsVk` with `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` only; fixed by dropping `VK_USE_PLATFORM_WAYLAND_KHR` from the local Diligent build + `SDL_VIDEO_BACKEND=x11` in the renderdoc run path
- Pass labels: wrap engine passes in `Diligent::ScopedDebugGroup` (→ `vkCmdBeginDebugUtilsLabelEXT`) for `rdc.py` by-name dumps; `GetOutputTargets()` reports color attachments only, so depth-only passes (shadow atlas) look empty there — use qrenderdoc
- Porting DiligentFX shadows: ShadowAttribs.fFixedDepthBias is not in cascade NDC depth units — normalize it by the cascade's light-space z scale (set after DistributeCascades) or the PCF bias exceeds the whole depth band and every receiver renders lit with a healthy depth pass (fingerprint: light amount 1.0 everywhere + good cascade-0 readback; found via ENGINE_SHADOW_DEBUG_VIS ladder 8→9/10→2/1→11)
- Runtime-compiled HLSL loads from the packed build/c-game/data/pak_1.pak, not the loose materials tree — verify with `unzip -p ... | grep <marker>` before trusting a negative result from a shader edit
- Depth-texture readback must transition from RESOURCE_STATE_DEPTH_WRITE (the state the shadow pass left it in), not DEPTH_READ — else VUID-VkImageMemoryBarrier-oldLayout-01197 every readback frame
- Behind-the-camera casters are clipped by the near cascades' light cubes (StabilizeExtents builds a light-space CUBE from the frustum bounding sphere that barely reaches past the eye): the tree-behind-the-camera shadow rendered as two hard-edged cascade bands — fixed by raising the caster FOV pad (1.35 → 2.8, `ENGINE_SHADOW_CASTER_PAD` for A/B) at the cost of ~2× coarser effective shadow texel density
- A CSM tier's "shadow distance" is only the cascade near/far split, not the coverage: the light cube (bounding sphere of the 2.8× padded frustum, ~132 m at the lowest tier) still samples shadows ~3× past it — enforce the tier distance with a receiver-side fade (PS lerps fLightAmount→lit over the last 25 % of the distance, `f4ShadowFade` cbuffer tail, `ENGINE_SHADOW_FADE=0` to A/B), and never shrink the cube (lower pad) to shorten the reach: casters outside the small cube stop writing depth and the shadow terminates at a projected cube face — the hard-polygon "cutout"

## 2026-09-06 — [entries](lessons/2026-09-06.md)

- "Shimmer on distant terrain with TAA": the TAA chain was fine — the input image re-rolls sub-pixel noise per jitter phase, and (1−taaWeight) of it leaks through every frame
- RCAS (CAS) pass: a static cbuffer set on the SRB is silently ignored (VUID-08114 every frame) — statics bind on the PSO/PRS, and `ShaderResourceVariableDesc` has no resource-type argument
- graphics settings wiring: an rmlui decorator referencing a missing pak asset terminate()s the whole app at document load, and `!charFlag` is an int, not a bool — overloaded persist helpers silently routed every toggle into settingsSetInt's assert
- File-backed textures upload as BCn blocks: libktx picks SRGB block variants from the DFD (albedo → 146/BC7_SRGB, normal → 145/BC7_UNORM, verified per asset) but the RGBA32 transcode target is always UNORM (37) regardless of DFD — and utils::Image.mipSizes holds byte OFFSETS, not sizes
- Main-menu logo washed out (stored 13 → displayed 62): rmlui file textures were plain RGBA8_UNORM sampled raw and written into the RGBA8_UNORM_SRGB swapchain, which encoded the already-encoded values a second time; the old engine's BC7_SRGB transcode target decoded on sample and round-tripped exactly
- "enter world" spams VUID-vkCmdBindDescriptorSets-00358/00359: the rmlui pass' zero-vertex scope-commit draw ran with the last WORLD pass' descriptor sets still stored in BindInfo.SetInfo[0] — CommitShaderResources only stores sets per binding index, the actual vkCmdBindDescriptorSets fires at DRAW time against the ACTIVE pipeline's layout, and release Diligent skips the DEV-only cross-check
- Prop grass rendered half in near-black shadow: the ported `Nlight` (unflipped normal for thin double-sided vegetation) was computed in the props PS but never used — the flipped back-face normal went to the sun and the IBL, so NdotL == 0 on every face turned away from the sun
- KTX-Software relinked after the filament removal: c-utils Image decodes ktx2 again (old engine verbatim), cursors back on .png.ktx2

## 2026-09-05 — [entries](lessons/2026-09-05.md)

- Mouse look ~N× slower at unlimited FPS: `input.mouseDx/Dy` was reset in every rendered frame's poll, but consumed only by the fixed 60 Hz simulation tick — every delta that landed on a non-tick frame was wiped
- Player moved ~2x faster at a 120fps cap than at 60: `utils::timer.dt` is the fixed 1/UPS tick, not the frame's dt, but the ported `ecsUpdate()` ran every system once per rendered frame
- audio settings gui flush in `removed()` segfaulted in `SoLoud::play` at shutdown: gui `removed()` runs after lower-priority systems are destroyed
- RMLUI gui queue: `guiManagerAddGuiNextFrame` called from inside a gui's `added()` (menu ENTER WORLD runs inside the queue-application loop) reallocated `pendingAdds` under the range-for's captured `end()` — dangling iterator, SEGV on `gui->name`; and the post-loop `clear()` would have silently dropped the queued items
- FPS GUI first line garbled + terrain flickering dark navy, both only with RMLUI enabled: `UpdateBuffer` on a VIRTUAL dynamic buffer blind-copies cbuffer data into dynamic-heap OFFSET 0 — an untracked, non-exclusive region that collides with whatever ring allocation parks there (the RMLUI vbo's, in steady state)
- RmlUi static text turning BLACK after ~1 s (showFps gui): the geometry pool's deferred-release queue stored SLOT INDICES but the consumer subtracted 1 again — every ReleaseGeometry(H) silently freed the NEIGHBOUR's slot (handle H−1); plus "static text goes black" = the font-effect glow layer still rendering with the base layer's geometry dropped
- Porting the old engine's RmlUi GUI (crmlui C wrapper) onto the Diligent render path: a stack-local `RmlParams` SIGSEGV'd inside `rmlRenderVulkan`, and a "Vulkan NDC" ortho rendered every document upside-down
- The props (vegetation) pass on Diligent: frustum planes extracted from the WRONG matrix orientation culled every tile (0 draws, no error), and a zeroed `DrawIndexedAttribs.FirstIndexLocation` silently drew range 0's sub-mesh for every other range — giant grass-cards + conifer geometry rendered under the deciduous range's instance data
- A single-key glTF channel whose value differs from the node's static TRS is a legit POSE HOLD (eve's run clip holds curled fingers one-key-per-bone) — flattening constants to rest (the 2026-09-04 singlekey-fix) straightened the hands, and the "0.01 head scale in the asset" was never in the source: it was gltf-standardize.py writing a Hips accessor IN PLACE that the exporter had shared with Head/LeftHand/\*4 channels
- the Diligent backend never ported the 2026-09-04 relative-to-anchor rework: at Azgaar cell 0 (x/z ~ 39 km) the f32 absolute camera + placement shimmers the character animation by up to 4 px — fixed with camera-anchored rendering (rotation-only view, f64 anchor-relative placement, split-anchor terrain)
- The Diligent gltf port regressed the 2026-09-04 pivot rule: `placementRootMatrix` pinned the AABB MIN CORNER (`T(-minc) * R(yaw) * T(pos - minc)`), swinging eve's visible body 0.65 m around the orbit target and rendering the player ~10% off-centre of screen
- gltfpack output is only partially compatible with Diligent's GLTF loader: meshopt-compressed buffers AND int16 rotation keys both load as garbage — the character renders invisible/NaN
- Automated-run env pitfalls: ENGINE_LOG_TIMEOUT is MILLISECONDS, and piping the game through `head` SIGPIPE-kills it mid-run
- Runtime-compiled HLSL (glslang) consumes cbuffer matrices TRANSPOSED relative to Diligent's row-major math — and a second cbuffer in the same PSO bound ambiguously
- Pose/placement matrices built in the update phase lag the aim by a frame: the character sits off-centre while the orbit camera rotates
- Making the glTF character a CSM caster via `GLTF_PBR_Renderer`: dynamic-buffer maps WITHOUT `MAP_FLAG_DISCARD` leave the draw reading last frame's region, and the PBR row-major shaders need the UNtransposed light matrix — transposed = clipped to nothing, silently

## 2026-09-04 — [entries](lessons/2026-09-04.md)

- settings.json type validation silently REWRITES the file: one wrong-typed key (e.g. `"upscalerMode": 2` int vs "double" template) nukes every user setting via writeDefault()
- `Texture::setImage` is zero-copy too: a `PixelBufferDescriptor` with a `nullptr` callback must be backed by storage that outlives the command (freed/stack source = garbage texture, different every launch)
- gltfio skips single-keyframe animation channels, so a joint's transform from the PREVIOUS clip survives forever; constant channels that don't match the node's static TRS then corrupt the pose
- A yaw-pivot matrix must anchor the point that should stay fixed: gltfPlaceAtFacingFilament pivoted on the AABB corner, swinging the visible model ~1.3 m off the camera target when the facing yaw differed from the camera's
- eve's front is local +Z and mat4f::rotation(r, +Y) maps +Z → (sin r, 0, cos r): gltfPlaceAtFacingFilament must use R(+yaw)
- Pre-multiplying a hierarchy transform only preserves world poses for DIRECT children: skinning collapses to a point when applied to every joint
- "Flying doesn't look like flying": verify the world scrolls, then blame the content
- One binary, one BasisU: two transcoder copies fail UASTC at transcode time
- 600-frame screenshots are not md5-stable (wind runs on real dt)
- Sourcing missing UI art: old-engine KTX2s and the shipped VF cover the gaps
- RADV LINEAR minification of sRGB8_A8 returns OPAQUE alpha: the grass-card alpha discard stops firing and mid-distance tufts render as solid tinted rectangles

## 2026-09 (undated) — [entries](lessons/2026-09.md)

- Diligent dynamic buffers are per-frame scratch, not storage
- lossy KTX2 on splat weight maps is bigger AND visibly worse than PNG
- World-tiling terrain textures need mipmaps + anisotropy, or distance is aliasing soup

## Filament era (archive) — [entries](lessons-filament-archive.md)

- (2026-09-04) Filament `doubleSided` flips the normal on back faces, blacking out thin up-normal vegetation (grass cards); and never mipmap sparse alpha-cutout grass textures
- (2026-09-04) camera_at_origin does NOT fix f32 world state: at 39 km every f32 position sits on a 3.9 mm grid and the character/ground shimmer by ~4 px
- (2026-09-04) Filament materials flip UV.y by default (`flipUV`): texture-sampling materials must set `flipUV : false` when mesh UVs are authored in image-row order
- (2026-09-04) Fragment `getWorldPosition()` is ALSO camera-shifted: world-anchored shading must use `getUserWorldPosition()`
- (2026-09) Filament buffer uploads are zero-copy: the source storage must outlive the command
- (2026-09) Filament emissive is photometric (nits): 0..1 debug colours are invisible
- (2026-09-04) Filament material.worldPosition is camera-shifted (camera_at_origin): never write absolute world coords
