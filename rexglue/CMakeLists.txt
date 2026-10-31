# burnout - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(burnout LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BURNOUT_SOURCES
    src/main.cpp
    src/movie_trace.cpp
    src/menu_trace.cpp
    src/xerenge_menu.cpp
    src/settings_movie.cpp
    src/xerenge_settings.cpp
    src/xerenge_restart.cpp
    src/music_trace.cpp
    src/speed_units.cpp
    src/custom_music.cpp
    src/game_font.cpp
    src/mods.cpp
    src/movie_frames.cpp
    src/tint_trace.cpp
    src/render_patches.cpp
    src/language_preset.cpp
    src/online_stub.cpp
    src/online_lobby.cpp
    src/game_timing.cpp
    src/plume_d3d_hooks.cpp
    src/widescreen.cpp
)

if(WIN32)
    add_executable(burnout WIN32 ${BURNOUT_SOURCES})
elseif(ANDROID)
    # SDLActivity loads the game and calls its SDL_main: libmain_eu.so, made
    # from the European release's default.xex, or libmain_us.so from the
    # American one (-DBURNOUT_ANDROID_LIBRARY=main_us), both packed into the
    # same APK; the app loads the one for the extracted disc.
    set(BURNOUT_ANDROID_LIBRARY main_eu CACHE STRING "The game's library name, without lib and .so")
    add_library(burnout SHARED ${BURNOUT_SOURCES} src/android_touch.cpp src/android_vibration.cpp)
    set_target_properties(burnout PROPERTIES OUTPUT_NAME ${BURNOUT_ANDROID_LIBRARY})
else()
    add_executable(burnout ${BURNOUT_SOURCES})
endif()

# plume renderer plugin - volk is confined to this shared library so it cannot
# shadow the loader's Vulkan entry points in the host executable.
set(PLUME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../plume")
if(NOT EXISTS "${PLUME_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "plume not found at ${PLUME_ROOT}; clone it beside Xerenge")
endif()

# On Android plume makes the surface from the ANativeWindow itself (see
# NativeRenderWindow in plume_swapchain.cpp); its SDL path is for desktops.
if(ANDROID)
    set(PLUME_SDL_VULKAN_ENABLED OFF CACHE BOOL "" FORCE)
else()
    set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
endif()
set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${PLUME_ROOT}" plume)
# Static plume linked into a shared plugin must be PIC, and must not inherit
# the host's -mcmodel=large (TLS local-exec breaks in .so files).
set_target_properties(plume PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(XENON_RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../XenosRecomp")
set(XERENGE_SHADER_CACHE_CPP "" CACHE FILEPATH
    "Generated XenosRecomp shader_cache.cpp for plume backend")
set(XERENGE_SHADER_CACHE_EXTRA_CPP "" CACHE FILEPATH
    "Optional auxiliary XenosRecomp cache (prepare_aux_shader_cache)")
set(XERENGE_SHADER_CACHE_BOOTSTRAP_CPP "" CACHE FILEPATH
    "Optional bootstrap XenosRecomp cache (fetch-patched runtime VS)")
if(NOT XERENGE_SHADER_CACHE_CPP)
    set(_generated_shader_cache
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/shader_cache.cpp")
    set(_bootstrap_shader_cache
        "${CMAKE_CURRENT_SOURCE_DIR}/../../xerenge-release/shader-cache-src/shader_cache.cpp")
    if(EXISTS "${_generated_shader_cache}")
        file(SIZE "${_generated_shader_cache}" _generated_shader_cache_size)
        if(_generated_shader_cache_size GREATER 10000)
            set(XERENGE_SHADER_CACHE_CPP "${_generated_shader_cache}")
        endif()
    endif()
    if(NOT XERENGE_SHADER_CACHE_CPP AND EXISTS "${_bootstrap_shader_cache}")
        set(XERENGE_SHADER_CACHE_CPP "${_bootstrap_shader_cache}")
    endif()
endif()
# The old hand-made extra cache (xerenge-release/shader-cache-src) is no longer
# picked up by itself: its shaders read constants through buffer addresses, and
# the pipelines now bind them as uniform buffers. Name one with
# -DXERENGE_SHADER_CACHE_EXTRA_CPP= only if it was translated the current way.
if(NOT XERENGE_SHADER_CACHE_BOOTSTRAP_CPP)
    # The runtime-assembled shaders: translate_runtime_shaders.sh's output,
    # which supersedes the older hand-wrapped bootstrap cache.
    foreach(_candidate shader_cache_runtime.cpp shader_cache_bootstrap.cpp)
        if(NOT XERENGE_SHADER_CACHE_BOOTSTRAP_CPP AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/${_candidate}")
            set(XERENGE_SHADER_CACHE_BOOTSTRAP_CPP
                "${CMAKE_CURRENT_SOURCE_DIR}/generated/${_candidate}")
        endif()
    endforeach()
endif()

set(REXGPU_PLUME_SOURCES
    src/plume_renderer/plume_graphics_system.cpp
    src/plume_renderer/plume_shader_cache.cpp
    src/plume_renderer/plume_swapchain.cpp
    src/plume_renderer/plume_postfx.cpp
    src/plume_renderer/plume_overlay.cpp
    src/plume_renderer/plume_draw.cpp
    src/plume_renderer/plume_etc2_cache.cpp
    src/plume_renderer/plume_interp.cpp
    src/plume_renderer/plume_framegen.cpp
    src/plume_renderer/plume_settings.cpp
    src/plume_renderer/plume_colour_table.cpp
    src/plume_renderer/plugin_main.cpp
    src/plume_renderer/shader_source_info.cpp
    "${XENON_RECOMP_ROOT}/thirdparty/smol-v/source/smolv.cpp"
)
if(WIN32)
    # A Windows DLL can't leave rex_frame_clock_provider unresolved at link
    # time the way a POSIX shared object can; see frame_clock_provider.h.
    list(APPEND REXGPU_PLUME_SOURCES src/plume_renderer/frame_clock_provider_win.cpp)
elseif(ANDROID)
    # Nor, in practice, Android's: the game library is loaded apart from it.
    list(APPEND REXGPU_PLUME_SOURCES src/plume_renderer/frame_clock_provider_android.cpp)
elseif(APPLE)
    # Nor macOS's, where the plain extern binds to librexruntime's copy.
    list(APPEND REXGPU_PLUME_SOURCES src/plume_renderer/frame_clock_provider_mac.cpp)
endif()

# What the renderer needs from each translated shader's HLSL, read at build
# time into a table (shader_source_info.h), so the sources are not shipped.
add_executable(shader_source_info_gen
    tools/shader_source_info_gen.cpp
    src/plume_renderer/shader_source_info.cpp
)
target_include_directories(shader_source_info_gen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(GLOB XERENGE_TRANSLATED_HLSL CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/xenos-hlsl/*.hlsl")
set(XERENGE_SHADER_SOURCE_INFO_CPP "${CMAKE_CURRENT_BINARY_DIR}/shader_source_info_table.cpp")
# It runs during the build, so a cross build without an emulator (Android)
# needs it for the build machine: compiled there by the host's own compiler.
set(XERENGE_SHADER_SOURCE_INFO_GEN shader_source_info_gen)
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    find_program(XERENGE_HOST_CXX NAMES c++ clang++ g++ NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(XERENGE_SHADER_SOURCE_INFO_GEN "${CMAKE_CURRENT_BINARY_DIR}/host/shader_source_info_gen")
    add_custom_command(
        OUTPUT "${XERENGE_SHADER_SOURCE_INFO_GEN}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/host"
        COMMAND "${XERENGE_HOST_CXX}" -std=c++20 -O2 -I "${CMAKE_CURRENT_SOURCE_DIR}/src"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_source_info_gen.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/plume_renderer/shader_source_info.cpp"
                -o "${XERENGE_SHADER_SOURCE_INFO_GEN}"
        DEPENDS tools/shader_source_info_gen.cpp src/plume_renderer/shader_source_info.cpp
                src/plume_renderer/shader_source_info.h
        COMMENT "Building shader_source_info_gen for the build machine"
        VERBATIM)
endif()
add_custom_command(
    OUTPUT "${XERENGE_SHADER_SOURCE_INFO_CPP}"
    COMMAND ${XERENGE_SHADER_SOURCE_INFO_GEN} "${CMAKE_CURRENT_SOURCE_DIR}/generated/xenos-hlsl"
            "${XERENGE_SHADER_SOURCE_INFO_CPP}"
    DEPENDS ${XERENGE_SHADER_SOURCE_INFO_GEN} ${XERENGE_TRANSLATED_HLSL}
    COMMENT "Reading the translated shaders into shader_source_info_table.cpp"
    VERBATIM)
list(APPEND REXGPU_PLUME_SOURCES "${XERENGE_SHADER_SOURCE_INFO_CPP}")
if(XERENGE_SHADER_CACHE_CPP)
    if(NOT EXISTS "${XERENGE_SHADER_CACHE_CPP}")
        message(FATAL_ERROR "XERENGE_SHADER_CACHE_CPP does not exist: ${XERENGE_SHADER_CACHE_CPP}")
    endif()
    list(APPEND REXGPU_PLUME_SOURCES "${XERENGE_SHADER_CACHE_CPP}")
    message(STATUS "rexgpu-plume: linking XenosRecomp shader cache ${XERENGE_SHADER_CACHE_CPP}")
endif()
if(XERENGE_SHADER_CACHE_EXTRA_CPP)
    if(NOT EXISTS "${XERENGE_SHADER_CACHE_EXTRA_CPP}")
        message(FATAL_ERROR "XERENGE_SHADER_CACHE_EXTRA_CPP does not exist: ${XERENGE_SHADER_CACHE_EXTRA_CPP}")
    endif()
    list(APPEND REXGPU_PLUME_SOURCES "${XERENGE_SHADER_CACHE_EXTRA_CPP}")
    message(STATUS "rexgpu-plume: linking extra shader cache ${XERENGE_SHADER_CACHE_EXTRA_CPP}")
endif()
if(XERENGE_SHADER_CACHE_BOOTSTRAP_CPP)
    if(NOT EXISTS "${XERENGE_SHADER_CACHE_BOOTSTRAP_CPP}")
        message(FATAL_ERROR "XERENGE_SHADER_CACHE_BOOTSTRAP_CPP does not exist: ${XERENGE_SHADER_CACHE_BOOTSTRAP_CPP}")
    endif()
    list(APPEND REXGPU_PLUME_SOURCES "${XERENGE_SHADER_CACHE_BOOTSTRAP_CPP}")
    message(STATUS "rexgpu-plume: linking bootstrap shader cache ${XERENGE_SHADER_CACHE_BOOTSTRAP_CPP}")
endif()

add_library(rexgpu-plume SHARED ${REXGPU_PLUME_SOURCES})
set_target_properties(rexgpu-plume PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(rexgpu-plume PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PLUME_ROOT}"
    "${XENON_RECOMP_ROOT}/thirdparty/smol-v/source"
    # rex/ui/renderdoc_api.h includes <renderdoc_app.h>; rex::runtime does not
    # export that path the way rexui does.
    "${REXSDK_DIR}/thirdparty/renderdoc"
    # SMAA's reference implementation: its lookup tables (AreaTex.h,
    # SearchTex.h) for plume_postfx.cpp. The shader, SMAA.hlsl, is compiled
    # ahead of time with smaa.frag (plume_postfx_spirv.h).
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/smaa"
    # stb_image, for the button atlases below.
    "${REXSDK_DIR}/thirdparty/stb"
)
# The button icons of other controllers (assets/input_prompts/replace/<set>/
# <hash>.png: a picture for the game's texture of that content hash, of its
# size), as bytes in the plugin: plume_prompts.cpp puts the chosen set's in place.
set(PROMPT_ATLAS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/prompt_atlases.cpp")
file(GLOB PROMPT_ATLASES "${CMAKE_CURRENT_SOURCE_DIR}/assets/input_prompts/replace/*/*.png")
set(prompt_atlas_source "#include <cstddef>\n#include <cstdint>\nstruct PromptAtlas { const char* set; uint64_t hash; const unsigned char* png; size_t size; };\n")
set(prompt_atlas_table "")
set(prompt_atlas_index 0)
foreach(atlas IN LISTS PROMPT_ATLASES)
    get_filename_component(atlas_hash "${atlas}" NAME_WE)
    get_filename_component(atlas_dir "${atlas}" DIRECTORY)
    get_filename_component(atlas_set "${atlas_dir}" NAME)
    file(READ "${atlas}" atlas_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," atlas_bytes "${atlas_hex}")
    string(APPEND prompt_atlas_source "static const unsigned char k_${prompt_atlas_index}[] = {${atlas_bytes}};\n")
    string(APPEND prompt_atlas_table "{\"${atlas_set}\", 0x${atlas_hash}ull, k_${prompt_atlas_index}, sizeof(k_${prompt_atlas_index})},")
    math(EXPR prompt_atlas_index "${prompt_atlas_index} + 1")
endforeach()
string(APPEND prompt_atlas_source "extern const PromptAtlas kPromptAtlases[] = {${prompt_atlas_table}{nullptr, 0, nullptr, 0}};\n")
file(WRITE "${PROMPT_ATLAS_CPP}" "${prompt_atlas_source}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROMPT_ATLASES})
target_sources(rexgpu-plume PRIVATE src/plume_renderer/plume_prompts.cpp "${PROMPT_ATLAS_CPP}")
if(NOT TARGET xxHash::xxhash)
    add_subdirectory("${XENON_RECOMP_ROOT}/thirdparty/xxHash/cmake_unofficial" xenos-xxhash)
endif()
if(NOT TARGET libzstd)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory("${XENON_RECOMP_ROOT}/thirdparty/zstd/build/cmake" xenos-zstd)
endif()
target_link_libraries(rexgpu-plume PRIVATE plume rex::runtime libzstd xxHash::xxhash)
if(APPLE)
    # The *Extra and *Bootstrap shader-cache tables are weak and only some
    # builds have them. ld64 wants every undefined symbol resolved at link time;
    # an ELF linker leaves these to load time, where a missing one reads as null.
    target_link_options(rexgpu-plume PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()
# etcpak's ETC2 encoder: on a GPU without BC textures (most phones) the title's
# DXT textures are re-encoded to ETC2 rather than left four to eight times
# larger as RGBA8. Just the encoder, not its tool.
add_library(xerenge-etcpak STATIC
    thirdparty/etcpak/ProcessRGB.cpp
    thirdparty/etcpak/Tables.cpp
    thirdparty/etcpak/Dither.cpp)
set_target_properties(xerenge-etcpak PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(xerenge-etcpak PUBLIC thirdparty/etcpak)
if(WIN32)
    # ProcessRGB.cpp includes <Windows.h> under _MSC_VER: its min/max macros
    # break std::numeric_limits<int>::max().
    target_compile_definitions(xerenge-etcpak PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(xerenge-etcpak PRIVATE -msse4.1)
endif()
target_link_libraries(rexgpu-plume PRIVATE xerenge-etcpak)
# zlib for the ETC2 preparation (xerenge_prepare_etc2): the installer's textures as ETC2.
if(ANDROID OR (UNIX AND NOT APPLE))
    target_link_libraries(rexgpu-plume PRIVATE z)
endif()
if(ANDROID)
    # libadrenotools opens a Vulkan driver the app ships (Mesa Turnip) in place
    # of the system one; its hook libraries go into the APK beside it.
    add_subdirectory(android/thirdparty/libadrenotools adrenotools EXCLUDE_FROM_ALL)
    target_link_libraries(rexgpu-plume PRIVATE adrenotools)
    add_dependencies(rexgpu-plume main_hook file_redirect_hook gsl_alloc_hook)

    # The installer's native half: the disc image check and extraction, with
    # the desktop extract-image's own code.
    add_library(xerenge_installer SHARED
        android/jni/installer_jni.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/cxx/xdvdfs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/cxx/crypto.cpp)
    target_include_directories(xerenge_installer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/cxx)
    add_dependencies(burnout xerenge_installer)
endif()
if(XERENGE_SHADER_CACHE_CPP)
    target_compile_definitions(rexgpu-plume PRIVATE XERENGE_HAS_SHADER_CACHE=1)
endif()

rexglue_setup_target(burnout GPU_PLUGINS xenos plume)

# rex/rex_app.h includes <imgui.h> publicly, but rex::runtime does not carry
# imgui's include directory to consumers, so a project that uses the app
# framework has to name it itself.
target_include_directories(burnout PRIVATE "${REXSDK_DIR}/thirdparty/imgui")

# The release and the commit the game was built from, for its log's first lines.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" XERENGE_VERSION LIMIT_COUNT 1)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE XERENGE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
target_compile_definitions(burnout PRIVATE XERENGE_VERSION="${XERENGE_VERSION}"
                                           XERENGE_COMMIT="${XERENGE_COMMIT}")

# The achievement notice's font (achievement_toast.cpp), Roboto Medium (Apache
# 2.0) from imgui's fonts, as bytes in the game: one file on every system.
set(TOAST_FONT "${REXSDK_DIR}/thirdparty/imgui/misc/fonts/Roboto-Medium.ttf")
set(TOAST_FONT_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/toast_font.cpp")
if(NOT EXISTS "${TOAST_FONT_CPP}" OR "${TOAST_FONT}" IS_NEWER_THAN "${TOAST_FONT_CPP}")
    file(READ "${TOAST_FONT}" toast_font_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," toast_font_bytes "${toast_font_hex}")
    file(WRITE "${TOAST_FONT_CPP}"
        "#include <cstddef>\nextern const unsigned char kToastFont[] = {${toast_font_bytes}};\n"
        "extern const size_t kToastFontSize = sizeof(kToastFont);\n")
endif()
target_sources(burnout PRIVATE src/achievement_toast.cpp "${TOAST_FONT_CPP}")
target_include_directories(burnout PRIVATE "${REXSDK_DIR}/thirdparty/stb" "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/stb")

# plume is built above and linked only into librexgpu-plume.so (see
# src/plume_renderer/). The host executable never links plume directly.
# The lobby server standing in for EA's (src/ealobby): a library, so the game
# can run one itself for players on a LAN, and a standalone binary for a
# server of your own.
find_package(Threads REQUIRED)
add_library(ealobby-core STATIC src/ealobby/aries.cpp src/ealobby/locker.cpp src/ealobby/news.cpp src/ealobby/server.cpp)
target_include_directories(ealobby-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ealobby-core PUBLIC cxx_std_17)
set_target_properties(ealobby-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ealobby-core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(ealobby-core PUBLIC ws2_32)
endif()
add_executable(ealobby tools/ealobby/main.cpp)
target_link_libraries(ealobby PRIVATE ealobby-core)
# The game hosts a lobby itself when it finds none on the LAN (online_lobby.cpp).
target_link_libraries(burnout PRIVATE ealobby-core)

# xdis: a development disassembler for the guest image (tools/xdis), built
# only where capstone is installed. The game does not need it.
find_library(CAPSTONE_LIBRARY capstone)
find_path(CAPSTONE_INCLUDE_DIR capstone/capstone.h)
if(CAPSTONE_LIBRARY AND CAPSTONE_INCLUDE_DIR)
    add_executable(xdis tools/xdis/main.cpp)
    target_include_directories(xdis PRIVATE "${CAPSTONE_INCLUDE_DIR}")
    target_link_libraries(xdis PRIVATE "${CAPSTONE_LIBRARY}")
endif()
