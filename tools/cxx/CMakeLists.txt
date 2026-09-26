cmake_minimum_required(VERSION 3.20)
project(xerenge-tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# pkg-config / stale cache can pick multilib (/usr/lib32) on Arch; tools are 64-bit only.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_xerenge_openssl_libdir "/usr/lib")
  if(EXISTS "${_xerenge_openssl_libdir}/libcrypto.so")
    set(OPENSSL_ROOT_DIR "/usr" CACHE PATH "OpenSSL root" FORCE)
    set(OPENSSL_INCLUDE_DIR "/usr/include" CACHE PATH "" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${_xerenge_openssl_libdir}/libcrypto.so" CACHE FILEPATH "" FORCE)
    set(OPENSSL_SSL_LIBRARY "${_xerenge_openssl_libdir}/libssl.so" CACHE FILEPATH "" FORCE)
  endif()
  unset(_xerenge_openssl_libdir)
endif()

find_package(OpenSSL REQUIRED)

add_library(xerenge_tools_lib STATIC
    file_util.cpp
    sha256.cpp
    xdvdfs.cpp
    dump_xex.cpp
    shader_containers.cpp
    runtime_shader_recipe.cpp
    wrap_runtime_pixel_shader.cpp
    wrap_runtime_vertex_shader.cpp
)
target_include_directories(xerenge_tools_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xerenge_tools_lib PUBLIC OpenSSL::Crypto)

function(xerenge_tool output_name target_name main_src)
    add_executable(${target_name} ${main_src})
    set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${output_name})
    target_link_libraries(${target_name} PRIVATE xerenge_tools_lib)
    install(TARGETS ${target_name} RUNTIME DESTINATION bin)
endfunction()

# Names match the former *.py tools (without the .py suffix).
xerenge_tool(extract-image xerenge_tool_extract_image extract_image_main.cpp)
xerenge_tool(patch_burnout_manifest xerenge_tool_patch_manifest patch_manifest_main.cpp)
xerenge_tool(dump_xex_image xerenge_tool_dump_xex dump_xex_image_main.cpp)
xerenge_tool(rehost_runtime_shader xerenge_tool_rehost rehost_runtime_shader_main.cpp)
xerenge_tool(wrap_runtime_pixel_shader xerenge_tool_wrap_ps wrap_runtime_pixel_shader_main.cpp)
xerenge_tool(wrap_runtime_vertex_shader xerenge_tool_wrap_vs wrap_runtime_vertex_shader_main.cpp)
xerenge_tool(prepare_aux_shader_cache xerenge_tool_prepare_aux prepare_aux_shader_cache_main.cpp)
xerenge_tool(build_runtime_shaders xerenge_tool_build_runtime build_runtime_shaders_main.cpp)
xerenge_tool(make_runtime_shader_recipe xerenge_tool_make_recipe make_runtime_shader_recipe_main.cpp)

install(TARGETS xerenge_tools_lib ARCHIVE DESTINATION lib)
