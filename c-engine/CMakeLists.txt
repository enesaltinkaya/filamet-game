file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")
add_library(c-engine STATIC ${cSrc})

# c-utils is PUBLIC: its include dirs and PCH propagate to anything linking c-engine
target_link_libraries(c-engine PUBLIC c-utils)

target_include_directories(c-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ── entt (header only) ──────────────────────────────────────────────────────
target_include_directories(c-engine PUBLIC ${thirdparty}/entt/git/single_include)

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
    ${filament_git}/libs/math/include
    ${filament_git}/libs/utils/include
    ${filament_git}/third_party/libsdl2/include
    ${filament_git}/third_party/robin-map/include  # tsl (NameComponentManager)
    ${filament_git}/third_party/basisu/transcoder   # basisu_transcoder.h (ktx2 → BC7)

    # generated gltfio resources (materials/uberarchive.h)
    ${filament_root}/libs/gltfio
)

# the bundled SDL headers select their platform config (X11 video etc.) from
# these defines; must match how libsdl2.a was built (filament's build defines
# them project-wide, we only need them where SDL is included)
target_compile_definitions(c-engine PUBLIC
    FILAMENT_SUPPORTS_X11=1
    FILAMENT_SUPPORTS_XCB=1
    FILAMENT_SUPPORTS_XLIB=1
)

# gltfio_core was built with draco support enabled
target_compile_definitions(c-engine PUBLIC GLTFIO_DRACO_SUPPORTED=1)

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

    # windowing (the SDL2 copy filament itself is built against)
    ${filament_root}/third_party/libsdl2/tnt/libsdl2.a
    PARENT_SCOPE
)

target_precompile_headers(c-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pch.h)

# ── renderdoc in-app api (debug captures, LD_PRELOAD librenderdoc.so) ───────
target_include_directories(c-engine PUBLIC /home/enes/Apps/renderdoc/renderdoc/api/app)
target_link_libraries(c-engine PUBLIC ${CMAKE_DL_LIBS})

# ── terrain splat material (compiled with matc into the game data dir) ─────
set(MATC ${filament_root}/tools/matc/matc)
set(TERRAIN_MAT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain.mat)
set(TERRAIN_MAT_OUT ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials/terrain.filamat)
add_custom_command(
    OUTPUT ${TERRAIN_MAT_OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/../c-game/data/pak_1/materials
    COMMAND ${MATC} -a vulkan -l 2 -o ${TERRAIN_MAT_OUT} ${TERRAIN_MAT_SRC}
    DEPENDS ${TERRAIN_MAT_SRC}
    COMMENT "matc terrain.mat"
    VERBATIM
)
add_custom_target(terrain_mat DEPENDS ${TERRAIN_MAT_OUT})
add_dependencies(c-engine terrain_mat)
