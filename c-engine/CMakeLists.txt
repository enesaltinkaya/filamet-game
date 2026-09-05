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

# zstd (model assets ship as zstd-compressed glb — decompressed in the gltf
# loader; libzstd.a is linked by c-game)
target_include_directories(c-engine PUBLIC ${thirdparty}/zstd/git/lib)

target_include_directories(c-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ── entt (header only) ──────────────────────────────────────────────────────
target_include_directories(c-engine PUBLIC ${thirdparty}/entt/git/single_include)

# ── SoLoud (audio, prebuilt — libsoloud.a linked by c-game) ─────────────────
target_include_directories(c-engine PUBLIC ${thirdparty}/soloud/git/include)

# ── imgui (GUI, prebuilt — see c-game link line) ─────────────────────────────
target_include_directories(c-engine PUBLIC ${thirdparty}/imgui/git)
target_include_directories(c-engine PUBLIC ${thirdparty}/imgui/git/backends)  # imgui_impl_vulkan

# ── Diligent Engine (the render backend) ─────────────────────────────────────
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
    ${diligent_git}/DiligentTools/TextureLoader/interface
    ${diligent_git}/DiligentFX
    ${diligent_git}/DiligentFX/PBR/interface
    ${diligent_git}/DiligentFX/Utilities/interface
    ${vulkan_sdk_include}
)

# prebuilt Diligent static libraries — linked by c-game (one --start-group on
# the exe)
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

# SDL3 (windowing; headers live under include/SDL3)
target_include_directories(c-engine PUBLIC ${thirdparty}/sdl/git/include/SDL3)

target_precompile_headers(c-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pch.h)

# the diligent TUs pull heavy Diligent/glslang headers that clash with the
# shared pch — skip the pch for them
set_source_files_properties(
    renderer/diligent/DiligentRenderer.cpp
    renderer/diligent/HeightmapTerrainDiligent.cpp
    renderer/diligent/PropsRenderDiligent.cpp
    gltf/GltfDiligent.cpp
    gui/GuiDiligent.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

# ── renderdoc in-app api (debug captures, LD_PRELOAD librenderdoc.so) ───────
target_include_directories(c-engine PUBLIC /home/enes/Apps/renderdoc/renderdoc/api/app)
target_link_libraries(c-engine PUBLIC ${CMAKE_DL_LIBS})

# ── heightmap terrain + props shaders (compiled at pass init) ────────────────
# The .hlsl sources are shipped to the pak so the passes can read them with
# dataManagerRead (same "source in-tree, artifact in pak" pattern as the
# other build steps).
set(HEIGHTMAP_TERRAIN_HLSL_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/diligent/shaders/heightmap_terrain_vs.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/diligent/shaders/heightmap_terrain_ps.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/diligent/shaders/props_vs.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/diligent/shaders/props_ps.hlsl
)
set(HEIGHTMAP_TERRAIN_HLSL_OUT
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/heightmap_terrain_vs.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/heightmap_terrain_ps.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/props_vs.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/props_ps.hlsl
)
add_custom_command(
    OUTPUT ${HEIGHTMAP_TERRAIN_HLSL_OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer/diligent/shaders/heightmap_terrain_vs.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/heightmap_terrain_vs.hlsl
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer/diligent/shaders/heightmap_terrain_ps.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/heightmap_terrain_ps.hlsl
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer/diligent/shaders/props_vs.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/props_vs.hlsl
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer/diligent/shaders/props_ps.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/props_ps.hlsl
    DEPENDS ${HEIGHTMAP_TERRAIN_HLSL_SRC}
    COMMENT "copy heightmap terrain + props .hlsl sources into pak_1"
    VERBATIM
)
add_custom_target(heightmap_terrain_hlsl DEPENDS ${HEIGHTMAP_TERRAIN_HLSL_OUT})
add_dependencies(c-engine heightmap_terrain_hlsl)
