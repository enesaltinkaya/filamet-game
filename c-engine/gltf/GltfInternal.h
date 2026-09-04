// Internal split of the gltf module: dispatch (Gltf.cpp) + one implementation
// per render backend.

#include "Defines.h"

#include <cstddef>

namespace engine::gltf {

// ── filament (GltfFilament.cpp) ──
bool gltfInitFilament(void);
bool gltfLoadFilament(const char* pakPath);
bool gltfPlaceAtFilament(f32 x, f32 y, f32 z);
bool gltfPlaceAtFacingFilament(f32 x, f32 y, f32 z, f32 yaw);
void gltfUpdateFilament(double elapsedSeconds);
void gltfDestroyFilament(void);
bool gltfBoundingBoxFilament(float min[3], float max[3]);
bool gltfLoadAnimationsFilament(const char* pakPath);
u32 gltfAnimationCountFilament(void);
const char* gltfAnimationNameFilament(u32 index);
f32 gltfAnimationDurationFilament(u32 index);
bool gltfPlayAnimationFilament(const char* name, f32 speed, bool loop);
bool gltfPlayAnimationBlendedFilament(const char* name, f32 speed, bool loop, f32 blendSeconds);
void gltfStopAnimationFilament(void);

// ── diligent (GltfDiligent.cpp) ──
bool gltfInitDiligent(void);
bool gltfLoadDiligent(const char* pakPath);
bool gltfPlaceAtDiligent(f32 x, f32 y, f32 z);
bool gltfPlaceAtFacingDiligent(f32 x, f32 y, f32 z, f32 yaw);
void gltfUpdateDiligent(double elapsedSeconds);
void gltfIblUpdateDiligent(const f32 color[3], f32 intensity);
void gltfDestroyDiligent(void);
bool gltfBoundingBoxDiligent(float min[3], float max[3]);
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
