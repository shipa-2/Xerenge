#pragma once

#include <cstddef>

// The host executable (game_timing.cpp) owns the game's step/wall clock and
// publishes it through this pointer; plume, a GPU plugin loaded into the
// same process (see plume_renderer/plugin_main.cpp), reads it to pace and
// interpolate frames between logic steps.
//
// On POSIX a plain extern works: the dynamic linker resolves the plugin's
// undefined reference against the symbol the host exports. A Windows DLL
// cannot leave a symbol unresolved at link time, so there the pointer is
// instead looked up by name in the host module once it is running
// (RexFrameClockProvider()), and the host exports it explicitly for that.
using RexFrameClockProviderFn = void (*)(void*, size_t);

#if defined(_WIN32)

#define REX_HOST_EXPORT __declspec(dllexport)

RexFrameClockProviderFn RexFrameClockProvider();

#elif defined(__ANDROID__)

// Android as well: the game is libmain.so, which Java loads into the app's own
// namespace, and the plugin's plain extern came out null there - Crash mode's
// and the 30 fps lock's pacing never reached the renderer. Looked up in
// libmain.so by name instead (frame_clock_provider_android.cpp).
#define REX_HOST_EXPORT __attribute__((visibility("default")))

RexFrameClockProviderFn RexFrameClockProvider();

#elif defined(__APPLE__)

// macOS as well, for another reason: the SDK's librexruntime defines a pointer
// of the same name, and with two-level namespaces the plugin's plain extern was
// bound to that copy, which nobody sets. The log said "no frame clock from the
// game" and the renderer paced and interpolated without it. Looked up in the
// executable by name instead (frame_clock_provider_mac.cpp).
#define REX_HOST_EXPORT __attribute__((visibility("default")))

RexFrameClockProviderFn RexFrameClockProvider();

#else

#define REX_HOST_EXPORT

extern "C" RexFrameClockProviderFn rex_frame_clock_provider;

inline RexFrameClockProviderFn RexFrameClockProvider() {
  return rex_frame_clock_provider;
}

#endif
