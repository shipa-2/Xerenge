/**
 * @file        plume_renderer/plume_perf.h
 * @brief       When the title swapped, and when the host presented (the performance overlay).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::plume_renderer::perf {

// The overlay's two clocks. They differ whenever the renderer makes frames the
// title did not: frame generation, Crash mode's smoothing.
//
// The title's swaps are what it calls frames. The times are kept in a ring
// that the title's thread writes and the rendering thread reads, with no lock
// on either side, and the ring costs a store per swap whether the overlay is
// shown or not.

constexpr size_t kSwapHistory = 256;

// A swap of the title's, just now.
void NoteTitleSwap();
// The times, in nanoseconds on the steady clock, of the latest swaps: up to
// `capacity` of them, the oldest first. Returns how many. Times that were
// overwritten while it read are not given.
size_t TitleSwapTimes(uint64_t* out, size_t capacity);

// A frame handed to the display.
void NoteHostPresent();
// How many, since the start.
uint32_t HostPresents();

}  // namespace rex::plume_renderer::perf
