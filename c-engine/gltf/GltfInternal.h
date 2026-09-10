
#include "Defines.h"

#include <cstddef>
#include <string>
#include <vector>

namespace engine::gltf {

// Shared packed-GLB access (zstd-aware): the pak models may be plain glb or
// zstd-compressed glb (sniffed by magic), and the file is a GLB chunk walk
// (header + JSON | BIN chunks). The splat terrain loader (SplatTerrainDiligent.cpp)
// reuses both to walk the packed GLB on the CPU.
bool gltfReadModelBytesDiligent(const char* path, std::vector<unsigned char>& data,
        std::string& error);
bool gltfGlbFindChunksDiligent(const std::vector<unsigned char>& bytes,
        const unsigned char** jsonPtr, size_t& jsonSize,
        const unsigned char** binPtr, size_t& binSize);

bool gltfInitDiligent(void);
bool gltfLoadDiligent(const char* pakPath);
bool gltfSceneLoadDiligent(const char* pakPath);
bool gltfSceneBoundingBoxDiligent(float min[3], float max[3]);
bool gltfSceneSurfaceHeightDiligent(float x, float z, float radius, float* outY);
bool gltfPlaceAtDiligent(double x, double y, double z);
bool gltfPlaceAtFacingDiligent(double x, double y, double z, f32 yaw);
void gltfUpdateDiligent(double elapsedSeconds);
void gltfIblUpdateDiligent(const f32 color[3], f32 intensity);
void gltfDestroyDiligent(void);
bool gltfBoundingBoxDiligent(float min[3], float max[3]);
bool gltfLocalBoundingBoxDiligent(float min[3], float max[3]);
size_t gltfDiligentMeshNodeCount(void);
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

}
