// Internal split of the gltf module: dispatch (Gltf.cpp) + one implementation
// per render backend.

#include "Defines.h"

#include <cstddef>

namespace engine::gltf {

// ── filament (GltfFilament.cpp) ──
bool gltfInitFilament(void);
bool gltfLoadFilament(const char* pakPath);
void gltfUpdateFilament(double elapsedSeconds);
void gltfDestroyFilament(void);
bool gltfBoundingBoxFilament(float min[3], float max[3]);

// ── diligent (GltfDiligent.cpp) ──
bool gltfInitDiligent(void);
bool gltfLoadDiligent(const char* pakPath);
void gltfUpdateDiligent(double elapsedSeconds);
void gltfIblUpdateDiligent(const f32 color[3], f32 intensity);
void gltfDestroyDiligent(void);
bool gltfBoundingBoxDiligent(float min[3], float max[3]);
size_t gltfDiligentMeshNodeCount(void);  // scene nodes carrying a mesh

}  // namespace engine::gltf
