/**
 * @file        diagnostics.h
 * @brief       Whether the renderer's own diagnostics run (the installer's debug mode).
 */
#pragma once

#include <cstdlib>

namespace xerenge {

// The installer's debug mode (debug = true, which sets XERENGE_DEBUG), or the
// GPU trace: the renderer's own counting and timing, per draw and per frame.
// Off, none of it runs - reading the clock on every draw and every Direct3D
// call was over a tenth of the title's thread on the Pixel.
inline bool Diagnostics() {
  static const bool on =
      std::getenv("XERENGE_DEBUG") != nullptr || std::getenv("XERENGE_GPU_TRACE") != nullptr;
  return on;
}

}  // namespace xerenge
