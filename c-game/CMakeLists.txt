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

    # image: KTX2/Basis transcode (ktx) + stb_image_resize2 (stb). The ktx
    # bundle vendors its own BasisU copy (newer ABI than Filament's tnt
    # basisu) and cannot be deduplicated against it, so the link must
    # tolerate the overlapping basist:: symbols; the ktx libs are listed
    # first, so its definitions win the duplicates
    -Wl,--allow-multiple-definition
    ${thirdparty}/ktx/git/${platform_dir}/libktx_read.a
    ${thirdparty}/ktx/git/${platform_dir}/libktx.a
    ${thirdparty}/ktx/git/${platform_dir}/interface/basisu_c_binding/libobj_basisu_cbind.a
    ${thirdparty}/ktx/git/${platform_dir}/external/astc-encoder/Source/libastcenc-avx2-static.a
    ${thirdparty}/stb/${platform_dir}/libstb.a

    # GUI (imgui rendered through filament via filagui; yoga for flexbox layout)
    ${thirdparty}/imgui/git/${platform_dir}/libimgui.a
    ${thirdparty}/filament/git/${platform_dir}/libs/filagui/libfilagui.a
    ${thirdparty}/yoga/git/${platform_dir}/yoga/libyogacore.a

    # filament + diligent (both render paths ship in one binary; the active
    # one is picked at startup) + entt windowing (paths from c-engine/CMakeLists.txt)
    ${FILAMENT_LIBS}
    ${DILIGENT_LIBS}
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
