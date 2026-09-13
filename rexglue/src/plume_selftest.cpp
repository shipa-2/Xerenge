// A self-test for the render interface, ahead of the renderer that will use it.
//
// re:Blue, the mature project on this SDK, does not use the Xenia-derived GPU
// plugin at all: it hooks the guest's D3D calls and drives plume directly. That
// is the direction this is the first step of, so before writing any of the
// translation layer, prove the interface comes up inside this process: an
// instance, the devices it can see, and a device created on the preferred one.
#include <cstdlib>
#include <iostream>
#include <memory>

#include "plume_vulkan.h"

namespace burnout {

void PlumeSelfTest() {
  if (std::getenv("XERENGE_PLUME_SELFTEST") == nullptr) {
    return;
  }
  plume::VulkanInterface interface;
  if (!interface.isValid()) {
    std::cerr << "plume: the Vulkan interface did not come up\n";
    return;
  }
  const auto& names = interface.getDeviceNames();
  std::cerr << "plume: interface up, " << names.size() << " device(s)\n";
  for (const std::string& name : names) {
    std::cerr << "plume:   " << name << '\n';
  }
  std::unique_ptr<plume::RenderDevice> device = interface.createDevice("");
  std::cerr << "plume: device " << (device ? "created" : "NOT created") << '\n';
}

}  // namespace burnout
