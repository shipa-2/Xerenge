// Android only: looks up the game's rex_frame_clock_provider in libmain.so by
// name (see frame_clock_provider.h) - the plugin's plain extern reference was
// not resolved against it there.
#ifdef __ANDROID__
#include "frame_clock_provider.h"

#include <atomic>

#include <dlfcn.h>

RexFrameClockProviderFn RexFrameClockProvider() {
  // Kept once found; looked for again until then, in case the renderer asks
  // before the game library is in.
  static std::atomic<RexFrameClockProviderFn*> slot{nullptr};
  RexFrameClockProviderFn* found = slot.load(std::memory_order_acquire);
  if (!found) {
    void* host = dlopen("libmain.so", RTLD_NOW | RTLD_NOLOAD);
    found = host ? static_cast<RexFrameClockProviderFn*>(dlsym(host, "rex_frame_clock_provider"))
                 : nullptr;
    if (!found) {
      found = static_cast<RexFrameClockProviderFn*>(dlsym(RTLD_DEFAULT, "rex_frame_clock_provider"));
    }
    if (!found) {
      return nullptr;
    }
    slot.store(found, std::memory_order_release);
  }
  return *found;
}
#endif
