file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")
add_executable(c-game ${cSrc})

target_include_directories(c-game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/game)

# Linking c-engine transitively gives us:
#   - c-engine include dir (c-engine/) + filament/entt includes and PCH
#   - c-utils include dir and thirdparty dirs (via c-utils PUBLIC)
#   - filament static libs (via c-engine PUBLIC)
target_link_libraries(c-game PRIVATE
    -Wl,--start-group
    c-engine
    c-utils

    # c-utils deps
    ${thirdparty}/libzip/git/${platform_dir}/lib/libzip.a
    ${thirdparty}/bzip2/git/${platform_dir}/libbz2_static.a
    ${thirdparty}/zlib/git/${platform_dir}/libzs.a
    ${thirdparty}/zstd/git/${platform_dir}/lib/libzstd.a
    ${thirdparty}/sqlite/git/${platform_dir}/libsqlite3.a
    ${thirdparty}/jansson/git/${platform_dir}/lib/libjansson.a
    ${thirdparty}/thpool/${platform_dir}/libthpool.a

    # image: stb_image_resize2 (stb). NOTE: the KTX-Software bundle (libktx)
    # is deliberately NOT linked: it vendors its own BasisU copy with a
    # newer ABI than Filament's tnt basisu, and its basist:: definitions
    # winning the duplicates silently broke Filament's ktxreader (init and
    # start_transcoding succeeded, transcode_image_level failed for every
    # UASTC asset → white-fallback terrain). One binary, one BasisU copy:
    # Filament's (used by ktxreader for terrain + gltfio ktx2). KTX2 loads
    # that need CPU pixels go through c-utils Image (stb) or ktxreader.
    # NOTE: --allow-multiple-definition survives ONLY for the benign,
    # same-source stb duplicates (c-engine's inline STB_IMAGE_WRITE +
    # MainMenuGui's inline STB_IMAGE vs cpp-thirdparty's libstb.a src.o —
    # identical git revision, first definition wins). It must NEVER be
    # needed to silence a *BasisU* overlap again: two basisu copies in one
    # binary (KTX-Software's vs Filament's) made ktxreader transcode every
    # UASTC asset fail while init/start_transcoding succeeded (2026-09-04,
    # docs/lessons.md). Keep exactly one BasisU in the link: Filament's.
    -Wl,--allow-multiple-definition
    ${thirdparty}/stb/${platform_dir}/libstb.a

    # GUI (imgui rendered through filament via filagui; yoga for flexbox layout)
    ${thirdparty}/imgui/git/${platform_dir}/libimgui.a
    ${thirdparty}/filament/git/${platform_dir}/libs/filagui/libfilagui.a
    ${thirdparty}/yoga/git/${platform_dir}/yoga/libyogacore.a

    # filament + diligent (both render paths ship in one binary; the active
    # one is picked at startup) + entt windowing (paths from c-engine/CMakeLists.txt)
    ${FILAMENT_LIBS}
    ${DILIGENT_LIBS}

    # physics (Jolt, same prebuilt wrapper + core the old engine linked)
    ${thirdparty}/jolt/wrapper/${platform_dir}/libcjolt.a
    ${thirdparty}/jolt/git/${platform_dir}/libJolt.a
    -Wl,--end-group
)

if (WIN32)
    target_link_libraries(c-game PRIVATE bcrypt hid)
    target_link_libraries(c-game PRIVATE imm32 setupapi version winmm) # sdl3
    target_link_libraries(c-game PRIVATE stdc++)
    target_link_libraries(c-game PRIVATE -static pthread)
elseif (LINUX)
    target_link_libraries(c-game PRIVATE stdc++ pthread dl m)
endif ()
