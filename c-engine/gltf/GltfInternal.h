// Internal split of the gltf module: public dispatch (Gltf.cpp) + the
// Diligent implementation (GltfDiligent.cpp).

#include "Defines.h"

#include <cstddef>

namespace engine::gltf {

// ── diligent (GltfDiligent.cpp) ──
bool gltfInitDiligent(void);
bool gltfLoadDiligent(const char* pakPath);
bool gltfPlaceAtDiligent(double x, double y, double z);
bool gltfPlaceAtFacingDiligent(double x, double y, double z, f32 yaw);
void gltfUpdateDiligent(double elapsedSeconds);
void gltfIblUpdateDiligent(const f32 color[3], f32 intensity);
void gltfDestroyDiligent(void);
bool gltfBoundingBoxDiligent(float min[3], float max[3]);
bool gltfLocalBoundingBoxDiligent(float min[3], float max[3]);
size_t gltfDiligentMeshNodeCount(void);  // scene nodes carrying a mesh
bool gltfLoadAnimationsDiligent(const char* pakPath);
u32 gltfAnimationCountDiligent(void);
const char* gltfAnimationNameDiligent(u32 index);
f32 gltfAnimationDurationDiligent(u32 index);
bool gltfPlayAnimationDiligent(const char* name, f32 speed, bool loop);
bool gltfPlayAnimationBlendedDiligent(const char* name, f32 speed, bool loop, f32 blendSeconds);
void gltfStopAnimationDiligent(void);

// The PBR preintegrated GGX LUT SRV (owned by the GLTF_PBR_Renderer):
// returns the ITextureView* with the reference already incremented, or
// null while the renderer is uninitialized. The terrain pass borrows it
// for lighting parity; it must be destroyed before gltfDestroyDiligent.
void* gltfDiligentPreintegratedGGX(void);

}  // namespace engine::gltf
