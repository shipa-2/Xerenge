/**
 * @file        plume_renderer/plume_perf.cpp
 * @brief       When the title swapped, and when the host presented (the performance overlay).
 */
#include "plume_renderer/plume_perf.h"

#include <atomic>
#include <chrono>

namespace rex::plume_renderer::perf {

namespace {

std::atomic<uint64_t> g_swap_times[kSwapHistory];
// Swaps seen since the start; the next one goes to slot count % kSwapHistory.
std::atomic<uint64_t> g_swap_count{0};
std::atomic<uint32_t> g_host_presents{0};

}  // namespace

void NoteTitleSwap() {
  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
  // fetch_add, so that a second thread that swaps (the vsync worker pumps the
  // ring too) cannot take the same slot.
  const uint64_t index = g_swap_count.fetch_add(1, std::memory_order_acq_rel);
  g_swap_times[index % kSwapHistory].store(now, std::memory_order_release);
}

size_t TitleSwapTimes(uint64_t* out, size_t capacity) {
  if (capacity > kSwapHistory) {
    capacity = kSwapHistory;
  }
  const uint64_t before = g_swap_count.load(std::memory_order_acquire);
  const uint64_t first = before > capacity ? before - capacity : 0;
  size_t n = 0;
  for (uint64_t i = first; i < before; ++i) {
    out[n++] = g_swap_times[i % kSwapHistory].load(std::memory_order_acquire);
  }
  // The title kept swapping while this read. A slot it wrote over holds a
  // newer time than the ones after it, and a slot it had claimed but not yet
  // stored holds an older one, or none: either would show as a negative or an
  // enormous interval. So whatever is out of order, and everything before it,
  // is left out.
  size_t from = 0;
  for (size_t i = 0; i < n; ++i) {
    if (out[i] == 0 || (i > 0 && out[i] < out[i - 1])) {
      from = i + (out[i] == 0 ? 1 : 0);
    }
  }
  if (from > 0) {
    for (size_t i = from; i < n; ++i) {
      out[i - from] = out[i];
    }
    n -= from;
  }
  return n;
}

void NoteHostPresent() {
  g_host_presents.fetch_add(1, std::memory_order_relaxed);
}

uint32_t HostPresents() {
  return g_host_presents.load(std::memory_order_relaxed);
}

}  // namespace rex::plume_renderer::perf
