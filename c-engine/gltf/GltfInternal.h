// Internal split of the gltf module: dispatch (Gltf.cpp) + one implementation
// per render backend.

#include "Defines.h"

#include <cstddef>

namespace engine::gltf {

// ── filament (GltfFilament.cpp) ──
bool gltfInitFilament(void);
bool gltfLoadFilament(const char* pakPath);
bool gltfPlaceAtFilament(f32 x, f32 y, f32 z);
void gltfUpdateFilament(double elapsedSeconds);
void gltfDestroyFilament(void);
bool gltfBoundingBoxFilament(float min[3], float max[3]);

// ── diligent (GltfDiligent.cpp) ──
bool gltfInitDiligent(void);
bool gltfLoadDiligent(const char* pakPath);
bool gltfPlaceAtDiligent(f32 x, f32 y, f32 z);
void gltfUpdateDiligent(double elapsedSeconds);
void gltfIblUpdateDiligent(const f32 color[3], f32 intensity);
void gltfDestroyDiligent(void);
bool gltfBoundingBoxDiligent(float min[3], float max[3]);
size_t gltfDiligentMeshNodeCount(void);  // scene nodes carrying a mesh

// The PBR preintegrated GGX LUT SRV (owned by the GLTF_PBR_Renderer):
// returns the ITextureView* with the reference already incremented, or
// null while the renderer is uninitialized. The terrain pass borrows it
// for lighting parity; it must be destroyed before gltfDestroyDiligent.
void* gltfDiligentPreintegratedGGX(void);

}  // namespace engine::gltf
