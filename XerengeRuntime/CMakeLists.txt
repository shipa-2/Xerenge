cmake_minimum_required(VERSION 3.20)

project(XerengeRuntime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(OpenGL REQUIRED)
find_path(XERENGE_OPENSSL_INCLUDE_DIR openssl/evp.h REQUIRED
    PATHS /usr/include /usr/local/include
    NO_DEFAULT_PATH)
find_library(XERENGE_CRYPTO_LIBRARY NAMES crypto REQUIRED
    PATHS /usr/lib /usr/lib64 /lib /lib64
    NO_DEFAULT_PATH)
add_library(Xerenge::Crypto UNKNOWN IMPORTED)
set_target_properties(Xerenge::Crypto PROPERTIES
    IMPORTED_LOCATION "${XERENGE_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${XERENGE_OPENSSL_INCLUDE_DIR}")
find_library(XERENGE_ZSTD_LIBRARY NAMES zstd REQUIRED
    PATHS /usr/lib /usr/lib64 /lib /lib64
    NO_DEFAULT_PATH)
add_library(Xerenge::Zstd UNKNOWN IMPORTED)
set_target_properties(Xerenge::Zstd PROPERTIES IMPORTED_LOCATION "${XERENGE_ZSTD_LIBRARY}")
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)

add_executable(xerenge-runtime
    src/main.cpp
    src/xenos_gpu.cpp
    src/xbox_media.cpp
    src/shader_cache_runtime.cpp
    ../XenosRecomp/thirdparty/smol-v/source/smolv.cpp
)

target_link_libraries(xerenge-runtime PRIVATE Vulkan::Vulkan OpenGL::GL Xerenge::Crypto Xerenge::Zstd
    PkgConfig::GLFW3 ${CMAKE_DL_LIBS})
target_compile_definitions(xerenge-runtime PRIVATE GLFW_INCLUDE_NONE)
target_include_directories(xerenge-runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../XenosRecomp/thirdparty/xxHash"
    "${CMAKE_CURRENT_SOURCE_DIR}/../XenosRecomp/thirdparty/smol-v/source")

# XenonRecomp output is intentionally generated outside the repository.  When
# supplied, build it as a separate library so the runtime can be linked to the
# recompiled game without checking game code into source control.
set(XERENGE_PPC_DIRECTORY "" CACHE PATH "Directory containing XenonRecomp PPC output")
set(XERENGE_SHADER_CACHE_CPP "" CACHE FILEPATH "Generated XenosRecomp shader cache source")
if(XERENGE_SHADER_CACHE_CPP)
    if(NOT EXISTS "${XERENGE_SHADER_CACHE_CPP}")
        message(FATAL_ERROR "XERENGE_SHADER_CACHE_CPP does not exist")
    endif()
    target_sources(xerenge-runtime PRIVATE "${XERENGE_SHADER_CACHE_CPP}")
    target_compile_definitions(xerenge-runtime PRIVATE XERENGE_HAS_SHADER_CACHE=1)
    message(STATUS "Building with generated XenosRecomp shader cache: ${XERENGE_SHADER_CACHE_CPP}")
endif()
if(XERENGE_PPC_DIRECTORY)
    if(NOT EXISTS "${XERENGE_PPC_DIRECTORY}/ppc_recomp_shared.h")
        message(FATAL_ERROR "XERENGE_PPC_DIRECTORY does not contain XenonRecomp output")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "XenonRecomp PPC output requires Clang; configure with clang++")
    endif()

    file(GLOB XERENGE_PPC_SOURCES CONFIGURE_DEPENDS
        "${XERENGE_PPC_DIRECTORY}/ppc_recomp.*.cpp"
        "${XERENGE_PPC_DIRECTORY}/ppc_func_mapping.cpp"
        "${XERENGE_PPC_DIRECTORY}/ppc_import_stubs.cpp")
    add_library(xerenge-ppc STATIC ${XERENGE_PPC_SOURCES})
    target_compile_features(xerenge-ppc PRIVATE cxx_std_20)
    target_include_directories(xerenge-ppc PRIVATE
        "${XERENGE_PPC_DIRECTORY}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../XenonRecomp/thirdparty/simde")
    # XenonRecomp's generated translation units only include ppc_recomp_shared.h
    # (ppc_config.h + the generic tool's ppc_context.h), so the Burnout-specific
    # mid-asm hook prototypes they call into (see work/B4_pdb.toml) are force-
    # included here instead of living in the generic tool's own headers.
    target_compile_options(xerenge-ppc PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/src/ppc_hooks.h")
    add_library(Xerenge::Ppc ALIAS xerenge-ppc)
    target_link_libraries(xerenge-runtime PRIVATE Xerenge::Ppc)
    target_include_directories(xerenge-runtime PRIVATE
        "${XERENGE_PPC_DIRECTORY}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../XenonRecomp/thirdparty/simde")
    target_compile_definitions(xerenge-runtime PRIVATE XERENGE_HAS_PPC=1)
    message(STATUS "Building generated PPC module from ${XERENGE_PPC_DIRECTORY}")
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME validate_minimal_xex
        COMMAND ${CMAKE_COMMAND}
            -DXERENGE_RUNTIME=$<TARGET_FILE:xerenge-runtime>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_minimal_xex.cmake
    )
endif()
