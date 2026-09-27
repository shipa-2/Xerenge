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

#else

#define REX_HOST_EXPORT

extern "C" RexFrameClockProviderFn rex_frame_clock_provider;

inline RexFrameClockProviderFn RexFrameClockProvider() {
  return rex_frame_clock_provider;
}

#endif
