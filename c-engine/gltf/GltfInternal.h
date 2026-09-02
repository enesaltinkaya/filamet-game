// Internal split of the gltf module: dispatch (Gltf.cpp) + one implementation
// per render backend. Handles are opaque u64s for the terrain paths.

#include "Defines.h"

#include <cstddef>

namespace engine::gltf {

// ── filament (GltfFilament.cpp) ──
bool gltfInitFilament(void);
bool gltfLoadFilament(const char* pakPath);
void gltfUpdateFilament(double elapsedSeconds);
void gltfDestroyFilament(void);
bool gltfBoundingBoxFilament(float min[3], float max[3]);
size_t gltfEntitiesNamedFilament(const char* prefix, u64* out, size_t cap);

// ── diligent (GltfDiligent.cpp) ──
bool gltfInitDiligent(void);
bool gltfLoadDiligent(const char* pakPath);
void gltfUpdateDiligent(double elapsedSeconds);
void gltfIblUpdateDiligent(const f32 color[3], f32 intensity);
void gltfDestroyDiligent(void);
bool gltfBoundingBoxDiligent(float min[3], float max[3]);
size_t gltfEntitiesNamedDiligent(const char* prefix, u64* out, size_t cap);
size_t gltfDiligentMeshNodeCount(void);  // scene nodes carrying a mesh

}  // namespace engine::gltf
