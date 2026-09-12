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
    src/plume_selftest.cpp
)

if(WIN32)
    add_executable(burnout WIN32 ${BURNOUT_SOURCES})
else()
    add_executable(burnout ${BURNOUT_SOURCES})
endif()

rexglue_setup_target(burnout GPU_PLUGINS xenos)

# rex/rex_app.h includes <imgui.h> publicly, but rex::runtime does not carry
# imgui's include directory to consumers, so a project that uses the app
# framework has to name it itself.
target_include_directories(burnout PRIVATE "${REXSDK_DIR}/thirdparty/imgui")

# plume - the render interface re:Blue uses, added ahead of the renderer that
# will drive it. Vulkan only here; its SDL integration needs SDL3, which the
# system provides and the SDK also builds.
set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Xerenge/plume" plume EXCLUDE_FROM_ALL)
target_link_libraries(burnout PRIVATE plume)
