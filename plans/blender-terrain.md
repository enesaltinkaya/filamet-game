# Blender terrain — replace the Azgaar map

Render the Blender-modeled Oghuzlands terrain (`oghuzlands.blend`) instead of
the Azgaar heightmap world. The offline asset pipeline is done; the engine
renders the model through the standard glTF PBR path until the splat pass
lands (phases 2-4).

Source of truth for the pipeline port: `/home/enes/Projects/c/game-001-cpp`
(`scripts/0-blender-terrain.py`, `tools/terrain-chunker`,
`tools/jolt-shape-builder`).

## Phase 1 — Asset pipeline ✅ (done)

`scripts/blender-terrain.py` (run from `scripts/build.sh`, mtime-stamped):

    oghuzlands.blend
      → blender export (scripts/blender-terrain.py -- inside Blender)
        - "export" collection, terrain object (242k verts / 484k tris)
        - splatInfo map on the node extras (splat UDIM image → per-channel
          detail texture names), rigid body extras
        - export_image_format=NONE: the GLB carries no image data
      → tools/terrain-chunker (4x4 grid, vertex-based triangle assignment —
        boundary triangles duplicate into both chunks, depth hides the
        overlap; carries the terrain node's splatInfo extra onto the first
        chunk node so the shipped model is the splat config)
      → gltfpack -vpf -vn 16 -vt 16 -kn -kv -ke
        NO -cc: Diligent's GLTF loader has no meshopt buffer support
        (reads EXT_meshopt_compression views as garbage); zstd keeps size
      → tools/jolt-shape-builder (per-chunk rigidBodyShape MESH → static
        mesh shapes, .jolt sidecar format in its README)
      → zstd -19

Shipped (c-game/data/pak_1):
    models/terrain/oghuzlands.zstd            4.3 MB (packed chunked glb)
    models/terrain/oghuzlands.jolt.zstd       5.0 MB (16 static mesh shapes)
    models/terrain/oghuzlands/grass1/*.ktx2   11 used UDIM weight tiles
    models/terrain/oghuzlands/roads1/*.ktx2    6 used UDIM weight tiles
    images/terrain/<detailName>/albedo.ktx2   1024² sRGB, raw+zstd, genmipmap
    images/terrain/<detailName>/normal.ktx2   1024² linear, same

Weight/detail ktx2 are raw RGBA8 + KTX_SS_ZSTD (toktx --zcmp 19) — NOT Basis:
lossy KTX2 on splat weight maps is bigger AND visibly worse (lessons 2026-09).
Noop weight tiles (solid 0,0,0,255, ≤ 20 800 B png) are skipped; stale ktx2
for them is removed.

Validation: `ENGINE_GLTF_MODEL=models/terrain/oghuzlands.zstd` loads the model
through the real Diligent PBR path — 16 chunk meshes, bounds match the Blender
source exactly, topdown screenshot shows the full landscape (untextured PBR
material until phase 2).

## Phase 2 — Splat terrain render pass (next)

New pass (sibling of `HeightmapTerrainDiligent`): stream the 16 chunk meshes
of `models/terrain/oghuzlands.zstd`, render with a splat UDIM material.

- Load the model with `gltfLoadDiligent`-style parsing (CPU-side cgltf/tinygltf
  or Diligent Model without the PBR renderer); scan the nodes for the
  `splatInfo` extra (carried onto `terrain_chunk_0_0` by the chunker): group →
  {red,green,blue,alpha} detail names. Splat UDIM tiles live at
  `models/terrain/<stem>/<group>/`, detail sets at
  `images/terrain/<detailName>/{albedo,normal}.ktx2`.
- Material: per splat group, sample the UDIM weight map (RGBA = 4 layer
  weights) with the mesh's TEXCOORD_0 (UDIM space, tiles 1001+), blend the 4
  detail albedos + normals per channel; groups mix (splat2 under splat1 in
  oghuzlands — see the blend's SplatGroup nodes, including the Mix input).
- Chunk streaming: 4x4 grid, camera window + frustum cull (the chunks are
  static; no LRU needed for a 16-chunk world, but keep the structure — a
  bigger map later wants streaming).
- UDIM sampling on Vulkan: the tiles are separate 2D textures (one ktx2 each);
  pick the tile from the UV (u//1, v//1 → UDIM number), or build an array
  texture. Missing (unused) tiles → sample black (weight 0).
- Lighting: same PBR frame attribs as the glTF pass (the heightmap pass'
  shader is the porting template: `heightmap_terrain_{vs,ps,shadow_vs}.hlsl`).

## Phase 3 — Physics (Jolt sidecar) ✅ loader done 2026-09-11

`PhysicsSystem` loads `models/terrain/oghuzlands.jolt.zstd` (JBVH v2 format,
see `tools/jolt-shape-builder/README.md`): the game registers the sidecar
from `GameSystem::loadWorld()` via `physicsTerrainSidecarSet()` (the physics
system is (re)added deferred, so its added() consumes the pending path once
joltInit has run) and restores all 16 static MESH shapes via
`joltCreateBodyFromShapeBlob` (identity body transforms — the chunk vertices
are in absolute coordinates; `JOLT_TERRAIN_USER_DATA` sentinel). The player
capsule stands/walks on the terrain from this.
Still pending: terrain in the CSM shadow pass (casters), self-shadowing.

## Phase 4 — World switch (azgaar removal done 2026-09-10, terrain world wired 2026-09-11)

- Azgaar map world removed: `c-game/game/azgaar/` + `loadingAzgaar/`,
  `c-engine/ecs/system/heightmap/`, the heightmap-terrain + props render
  passes, their .hlsl + pak assets, Game.cpp wiring, GUI cell/teleport, and
  the player's heightmap ground-snap gate.
- The terrain world now loads on ENTER WORLD: `gltfSceneLoad` (second static
  PBR model slot in the gltf module, drawn in a `terrain` debug group under
  the `player` group in the `world` pass) + the Phase 3 sidecar. Spawn is a
  CPU plane fit over the terrain vertices near the spawn xz
  (`gltfSceneSurfaceHeight`, 2 m above the surface so the capsule settles),
  default camera frames the spawn; `ENGINE_TELEPORT` still overrides.
- Remaining for the full world: props scattered on the splat surface
  (vegetation groups: the `grass*` splat groups — see the
  `.terrain-vegetation-groups.json` cache), water/river pass if wanted.
