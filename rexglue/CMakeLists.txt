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
