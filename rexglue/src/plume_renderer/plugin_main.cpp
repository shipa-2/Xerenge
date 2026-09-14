/**
 * @file        plume_renderer/plugin_main.cpp
 * @brief       rexgpu-plume plugin entry points (same ABI as rexgpu-xenos).
 */
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "plume_renderer/plume_graphics_system.h"

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-plume: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-plume: invalid GpuCreateInfo");
    return nullptr;
  }

  std::string_view backend = info->backend ? info->backend : "any";
  if (backend != "any" && backend != "vulkan") {
    REXLOG_ERROR("rexgpu-plume: requested backend '{}' is not supported (use vulkan)", backend);
    return nullptr;
  }

  return new rex::plume_renderer::PlumeGraphicsSystem();
}
