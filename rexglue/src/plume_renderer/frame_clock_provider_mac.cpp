// macOS only: looks up the game's rex_frame_clock_provider in the executable by
// name (see frame_clock_provider.h). The plugin's plain extern was bound to
// librexruntime's copy of the pointer, which nobody sets.
#ifdef __APPLE__
#include "frame_clock_provider.h"

#include <atomic>

#include <dlfcn.h>

RexFrameClockProviderFn RexFrameClockProvider() {
  // Kept once found; looked for again until then, the same as on Android.
  static std::atomic<RexFrameClockProviderFn*> slot{nullptr};
  RexFrameClockProviderFn* found = slot.load(std::memory_order_acquire);
  if (!found) {
    found = static_cast<RexFrameClockProviderFn*>(dlsym(RTLD_MAIN_ONLY, "rex_frame_clock_provider"));
    if (!found) {
      return nullptr;
    }
    slot.store(found, std::memory_order_release);
  }
  return *found;
}
#endif
