# jolt-shape-builder

Standalone tool: reads a glTF/GLB, pre-bakes Jolt shape data for every
physics-enabled node, writes a `.jolt` sidecar next to the packed model.

A node qualifies if its glTF extras contain:

- `notexture: true` (physics-only collision mesh, always static)
- `rigidBodyShape: <shape>` (BOX, SPHERE, CAPSULE, CYLINDER, CONVEX_HULL, MESH)

`rigidBodyType` ACTIVE → dynamic, anything else → static. `rigidBodyMass`,
`rigidBodyFriction`, `rigidBodyRestitution` default to 1.0 / 0.5 / 0.0.

## Binary format (version 2)

    u8[4]   magic = "JBVH"
    u32     version = 2
    u32     entry_count
    per entry:
      u32     node_name_length
      char[]  node_name (NOT null-terminated)
      u8      shape_type  (0=NOTEXTURE 1=BOX 2=SPHERE 3=CAPSULE
                           4=CYLINDER 5=CONVEX_HULL 6=MESH)
      u8      motion_type (0=STATIC 1=DYNAMIC)
      float   mass
      float   friction
      float   restitution
      u32     shape_blob_size
      u8[]    shape_blob (Jolt SaveBinaryState output)

The engine loads this sidecar when parsing a model whose name (without the
final extension) has a sibling `<name>.jolt` / `<name>.jolt.zstd` entry in the
pak; the loader rejects version != 2.
