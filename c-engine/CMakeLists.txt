file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")

# imgui's vulkan backend compiles into c-engine for the diligent gui path; it
# resolves vk* loaders through volk (initialized by the diligent factory)
set(imgui_vulkan_backend ${thirdparty}/imgui/git/backends/imgui_impl_vulkan.cpp)
list(APPEND cSrc ${imgui_vulkan_backend})

add_library(c-engine STATIC ${cSrc})

set_source_files_properties(${imgui_vulkan_backend} PROPERTIES
    COMPILE_DEFINITIONS "IMGUI_IMPL_VULKAN_USE_VOLK"
    SKIP_PRECOMPILE_HEADERS ON
)

# c-utils is PUBLIC: its include dirs and PCH propagate to anything linking c-engine
target_link_libraries(c-engine PUBLIC c-utils)

target_include_directories(c-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ── entt (header only) ──────────────────────────────────────────────────────
target_include_directories(c-engine PUBLIC ${thirdparty}/entt/git/single_include)

# ── imgui + filagui (GUI, prebuilt — see c-game link line) ────────────────────
target_include_directories(c-engine PUBLIC ${thirdparty}/imgui/git)
target_include_directories(c-engine PUBLIC ${thirdparty}/imgui/git/backends)  # imgui_impl_vulkan
target_include_directories(c-engine PUBLIC ${thirdparty}/filament/git/libs/filagui/include)

# ── filament public headers ─────────────────────────────────────────────────
set(filament_git ${thirdparty}/filament/git)
set(filament_root ${filament_git}/${platform_dir})
target_include_directories(c-engine PUBLIC
    ${filament_git}
    ${filament_git}/filament/include
    ${filament_git}/filament/backend/include
    ${filament_git}/libs/filabridge/include
    ${filament_git}/libs/filaflat/include
    ${filament_git}/libs/gltfio/include
    ${filament_git}/libs/ktxreader/include
    ${filament_git}/libs/math/include
    ${filament_git}/libs/utils/include
    # SDL3 (our own prebuilt, cpp-thirdparty/sdl) — headers live under include/SDL3
    ${thirdparty}/sdl/git/include/SDL3
    ${filament_git}/third_party/robin-map/include  # tsl (NameComponentManager)
    ${filament_git}/third_party/basisu/transcoder   # basisu_transcoder.h (ktx2 → BC7)

    # generated gltfio resources (materials/uberarchive.h)
    ${filament_root}/libs/gltfio
)

# filament's public headers (BlueVK.h, WebGPUPlatform.h) gate their Linux
# X11/XCB/Xlib paths on these defines; must match how the prebuilt filament
# libs were compiled
target_compile_definitions(c-engine PUBLIC
    FILAMENT_SUPPORTS_X11=1
    FILAMENT_SUPPORTS_XCB=1
    FILAMENT_SUPPORTS_XLIB=1
)

# gltfio_core was built with draco support enabled
target_compile_definitions(c-engine PUBLIC GLTFIO_DRACO_SUPPORTED=1)

# ── Diligent Engine (second render path, selected by ENGINE_RENDERER/setting) ──
set(diligent_git ${thirdparty}/diligent/git)
set(diligent_root ${diligent_git}/${platform_dir})

if (LINUX)
    set(vulkan_sdk_include /var/home/enes/Sdks/VulkanSDK-linux/1.4.350.1/x86_64/include)
elseif (WIN32)
    set(vulkan_sdk_include /var/home/enes/Sdks/VulkanSDK-win/1.4.313.0/Include)
endif ()

# Diligent's headers require their platform macros (normally exported by
# their CMake targets, which we bypass by linking the prebuilt archives)
if (LINUX)
    target_compile_definitions(c-engine PUBLIC PLATFORM_LINUX=1)
elseif (WIN32)
    target_compile_definitions(c-engine PUBLIC PLATFORM_WIN32=1)
endif ()

target_include_directories(c-engine PUBLIC
    ${diligent_git}/DiligentCore
    ${diligent_git}/DiligentCore/Common/interface
    ${diligent_git}/DiligentCore/Primitives/interface
    ${diligent_git}/DiligentCore/Platforms/interface
    ${diligent_git}/DiligentCore/Platforms/Basic/interface
    ${diligent_git}/DiligentCore/Platforms/Linux/interface
    ${diligent_git}/DiligentCore/Graphics/GraphicsEngine/interface
    ${diligent_git}/DiligentCore/Graphics/GraphicsEngineNextGenBase/interface
    ${diligent_git}/DiligentCore/Graphics/GraphicsTools/interface
    ${diligent_git}/DiligentCore/Graphics/GraphicsEngineVulkan/interface
    ${diligent_git}/DiligentCore/ThirdParty/volk
    ${diligent_git}/DiligentTools/AssetLoader/interface
    ${diligent_git}/DiligentFX
    ${diligent_git}/DiligentFX/PBR/interface
    ${diligent_git}/DiligentFX/Utilities/interface
    ${vulkan_sdk_include}
)

# prebuilt Diligent static libraries — linked by c-game (one --start-group on
# the exe, same approach as FILAMENT_LIBS)
set(DILIGENT_LIBS
    ${diligent_root}/DiligentFX/libDiligentFX.a
    ${diligent_root}/DiligentTools/libDiligentTools.a
    ${diligent_root}/DiligentTools/AssetLoader/libDiligent-AssetLoader.a

    ${diligent_root}/DiligentCore/Graphics/GraphicsEngineVulkan/libDiligent-GraphicsEngineVk-static.a
    ${diligent_root}/DiligentCore/Graphics/GraphicsEngine/libDiligent-GraphicsEngine.a
    ${diligent_root}/DiligentCore/Graphics/GraphicsEngineNextGenBase/libDiligent-GraphicsEngineNextGenBase.a
    ${diligent_root}/DiligentCore/Graphics/GraphicsTools/libDiligent-GraphicsTools.a
    ${diligent_root}/DiligentCore/Graphics/GraphicsAccessories/libDiligent-GraphicsAccessories.a
    ${diligent_root}/DiligentCore/Graphics/ShaderTools/libDiligent-ShaderTools.a
    ${diligent_root}/DiligentCore/Graphics/Archiver/libDiligent-Archiver-static.a
    ${diligent_root}/DiligentCore/Graphics/HLSL2GLSLConverterLib/libDiligent-HLSL2GLSLConverterLib.a
    ${diligent_root}/DiligentCore/Common/libDiligent-Common.a
    ${diligent_root}/DiligentCore/Primitives/libDiligent-Primitives.a
    ${diligent_root}/DiligentCore/Platforms/Basic/libDiligent-BasicPlatform.a
    ${diligent_root}/DiligentCore/Platforms/Linux/libDiligent-LinuxPlatform.a

    # shader compiler chain (runtime HLSL → SPIRV via Diligent ShaderTools)
    ${diligent_root}/DiligentCore/ThirdParty/glslang/glslang/libglslang.a
    ${diligent_root}/DiligentCore/ThirdParty/glslang/glslang/libMachineIndependent.a
    ${diligent_root}/DiligentCore/ThirdParty/glslang/glslang/libGenericCodeGen.a
    ${diligent_root}/DiligentCore/ThirdParty/glslang/glslang/libglslang-default-resource-limits.a
    $<$<PLATFORM_ID:Linux>:${diligent_root}/DiligentCore/ThirdParty/glslang/glslang/OSDependent/Unix/libOSDependent.a>
    $<$<PLATFORM_ID:Windows>:${diligent_root}/DiligentCore/ThirdParty/glslang/glslang/OSDependent/Windows/libOSDependent.a>
    ${diligent_root}/DiligentCore/ThirdParty/glslang/SPIRV/libSPIRV.a
    ${diligent_root}/DiligentCore/ThirdParty/SPIRV-Cross/libspirv-cross-core.a
    ${diligent_root}/DiligentCore/ThirdParty/SPIRV-Tools/source/libSPIRV-Tools.a
    ${diligent_root}/DiligentCore/ThirdParty/SPIRV-Tools/source/opt/libSPIRV-Tools-opt.a

    ${diligent_root}/DiligentCore/ThirdParty/volk/libvolk.a
    ${diligent_root}/DiligentCore/ThirdParty/xxHash/build/cmake/libxxhash.a
    PARENT_SCOPE
)

# ── filament prebuilt static libraries ──────────────────────────────────────
# linked by the c-game executable (see c-game/CMakeLists.txt) — a static lib
# can't link other static libs, and keeping one --start-group on the exe keeps
# the linker happy
set(FILAMENT_LIBS
    ${filament_root}/filament/libfilament.a
    ${filament_root}/filament/backend/libbackend.a
    ${filament_root}/libs/filabridge/libfilabridge.a
    ${filament_root}/libs/filaflat/libfilaflat.a
    ${filament_root}/libs/math/libmath.a
    ${filament_root}/libs/utils/libutils.a
    ${filament_root}/libs/bluegl/libbluegl.a
    ${filament_root}/libs/bluevk/libbluevk.a

    # gltfio (glTF loading, ubershader materials — no filamat/JIT needed)
    ${filament_root}/libs/gltfio/libgltfio_core.a
    ${filament_root}/libs/gltfio/libuberarchive.a
    ${filament_root}/libs/geometry/libgeometry.a
    ${filament_root}/libs/ktxreader/libktxreader.a
    ${filament_root}/libs/image/libimage.a
    ${filament_root}/libs/uberz/libuberzlib.a
    ${filament_root}/third_party/draco/tnt/libdracodec.a
    ${filament_root}/third_party/meshoptimizer/tnt/libmeshoptimizer.a
    ${filament_root}/third_party/mikktspace/libmikktspace.a  # geometry: tangent-space generation
    ${filament_root}/third_party/basisu/tnt/libbasis_transcoder.a
    ${filament_root}/third_party/stb/tnt/libstb.a

    # filament third_party runtime deps
    ${filament_root}/third_party/smol-v/tnt/libsmol-v.a
    ${filament_root}/third_party/libz/tnt/libz.a
    ${filament_root}/third_party/zstd/tnt/libzstd.a

    # windowing (SDL3 dlopens X11/ALSA at runtime — no extra system libs needed)
    ${thirdparty}/sdl/git/${platform_dir}/libSDL3.a
    PARENT_SCOPE
)

target_precompile_headers(c-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pch.h)

# the diligent TUs pull heavy Diligent headers that clash with the filament
# pch's compilation cost — skip the shared pch for them
set_source_files_properties(
    renderer/diligent/DiligentRenderer.cpp
    gltf/GltfDiligent.cpp
    gui/GuiDiligent.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

# ── renderdoc in-app api (debug captures, LD_PRELOAD librenderdoc.so) ───────
target_include_directories(c-engine PUBLIC /home/enes/Apps/renderdoc/renderdoc/api/app)
target_link_libraries(c-engine PUBLIC ${CMAKE_DL_LIBS})

# ── heightmap terrain material (compiled with matc into the game data dir) ─
# The .filamat is committed (the pak pipeline ships it to pak_1); this step
# keeps it in sync with terrain.mat whenever it changes. Regeneration needs
# the prebuilt matc (build-linux/tools/matc); on platforms without it the
# committed blob is used as-is — re-generate on a machine that has it when
# the .mat changes.
set(MATC ${filament_root}/tools/matc/matc)
set(HEIGHTMAP_TERRAIN_MAT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/renderer/filament/materials/terrain.mat)
set(HEIGHTMAP_TERRAIN_MAT_OUT ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/heightmap_terrain.filamat)
if (EXISTS ${MATC})
    add_custom_command(
        OUTPUT ${HEIGHTMAP_TERRAIN_MAT_OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials
        COMMAND ${MATC} -a vulkan -l 2 -o ${HEIGHTMAP_TERRAIN_MAT_OUT} ${HEIGHTMAP_TERRAIN_MAT_SRC}
        DEPENDS ${HEIGHTMAP_TERRAIN_MAT_SRC} ${MATC}
        COMMENT "matc heightmap terrain.mat"
        VERBATIM
    )
    add_custom_target(heightmap_terrain_mat DEPENDS ${HEIGHTMAP_TERRAIN_MAT_OUT})
    add_dependencies(c-engine heightmap_terrain_mat)
else ()
    message(WARNING "matc not found at ${MATC}; using the committed heightmap_terrain.filamat")
endif ()

