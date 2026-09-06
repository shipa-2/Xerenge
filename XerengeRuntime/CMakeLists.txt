cmake_minimum_required(VERSION 3.20)

project(XerengeRuntime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)

add_executable(xerenge-runtime
    src/main.cpp
)

target_link_libraries(xerenge-runtime PRIVATE Vulkan::Vulkan PkgConfig::GLFW3)
target_compile_definitions(xerenge-runtime PRIVATE GLFW_INCLUDE_NONE)

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME validate_minimal_xex
        COMMAND ${CMAKE_COMMAND}
            -DXERENGE_RUNTIME=$<TARGET_FILE:xerenge-runtime>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_minimal_xex.cmake
    )
endif()
