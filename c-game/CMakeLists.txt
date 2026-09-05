file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")
add_executable(c-game ${cSrc})

target_include_directories(c-game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/game)

# Linking c-engine transitively gives us:
#   - c-engine include dir (c-engine/) + diligent/entt includes and PCH
#   - c-utils include dir and thirdparty dirs (via c-utils PUBLIC)
#   - diligent static libs (via c-engine PUBLIC)
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
    # is deliberately NOT linked (it duplicates BasisU functionality already
    # inside the prebuilt Diligent texture loader). KTX2 loads that need CPU
    # pixels go through c-utils Image (stb). The GPU-side KTX2 path is
    # Diligent's TextureLoader.
    # NOTE: --allow-multiple-definition survives ONLY for the benign,
    # same-source stb duplicates (c-engine's inline STB_IMAGE_WRITE +
    # MainMenuGui's inline STB_IMAGE vs cpp-thirdparty's libstb.a src.o —
    # identical git revision, first definition wins). It must never paper
    # over a real duplicate-symbol overlap: two BasisU copies in one binary
    # once made ktx2 transcode calls fail while init succeeded
    # (2026-09-04, docs/lessons.md).
    -Wl,--allow-multiple-definition
    ${thirdparty}/stb/${platform_dir}/libstb.a

    # GUI (imgui rendered through imgui_impl_vulkan; yoga for flexbox layout)
    ${thirdparty}/imgui/git/${platform_dir}/libimgui.a
    ${thirdparty}/yoga/git/${platform_dir}/yoga/libyogacore.a

    # RmlUi GUI (crmlui C wrapper, Vulkan callback path — the wrapper calls
    # back into the engine, see c-engine/gui rmlui layer; same lib set the old
    # c-game linked). librmlui.a's font interface is FreeType-based → freetype
    # is required once any rmlui object is pulled in; freetype's own objects
    # (autofit/sfnt) pull in harfbuzz (shaping) + brotli (WOFF2 sfnt) — the old
    # c-game linked all of them (task 2's "no hb_* symbols" check only saw
    # librmlui.a's undefineds, not freetype's).
    ${thirdparty}/rmlui/wrapper/${platform_dir}/libcrmlui.a
    ${thirdparty}/rmlui/git/${platform_dir}/librmlui_lua.a
    ${thirdparty}/rmlui/git/${platform_dir}/librmlui_debugger.a
    ${thirdparty}/rmlui/git/${platform_dir}/librmlui.a
    ${thirdparty}/lua/git/${platform_dir}/liblua.a
    ${thirdparty}/freetype/git/${platform_dir}/libfreetype.a
    ${thirdparty}/harfbuzz/git/${platform_dir}/libharfbuzz.a
    ${thirdparty}/brotli/git/${platform_dir}/libbrotlidec.a
    ${thirdparty}/brotli/git/${platform_dir}/libbrotlicommon.a

    # diligent render backend + SDL3 (paths from c-engine/CMakeLists.txt)
    ${DILIGENT_LIBS}
    ${thirdparty}/sdl/git/${platform_dir}/libSDL3.a

    # physics (Jolt, same prebuilt wrapper + core the old engine linked)
    ${thirdparty}/jolt/wrapper/${platform_dir}/libcjolt.a
    ${thirdparty}/jolt/git/${platform_dir}/libJolt.a

    # audio (SoLoud — must stay inside the --start-group: its symbols
    # reference each other and SDL/X11 symbols resolved by the group)
    ${thirdparty}/soloud/git/${platform_dir}/libsoloud.a
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
