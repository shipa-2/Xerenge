// Windows only: looks up the host-exported rex_frame_clock_provider symbol
// by name, since a DLL cannot reference it directly (see
// frame_clock_provider.h) - lld-link refuses an unresolved symbol at link
// time, unlike POSIX shared objects, which resolve it lazily at load time.
#ifdef _WIN32
#include "frame_clock_provider.h"

#include <windows.h>

RexFrameClockProviderFn RexFrameClockProvider() {
  static RexFrameClockProviderFn* const slot = [] {
    HMODULE host = GetModuleHandleW(nullptr);
    return host ? reinterpret_cast<RexFrameClockProviderFn*>(
                      GetProcAddress(host, "rex_frame_clock_provider"))
                : nullptr;
  }();
  return slot ? *slot : nullptr;
}
#endif
