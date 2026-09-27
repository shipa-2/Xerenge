// Retire the movie renderer's in-flight frames.
//
// CCalVideoRenderer counts the frames it has handed to the GPU in a field at
// renderer+368, bumping it per blit, and the only thing that drops it is the
// callback the console runs from the Xenos completion interrupt. Until it
// falls, Render answers "still pending", CCalMoviePlayer never advances, and
// CB4FrontEnd::Update - which calls into the player from the title's main
// thread - never returns. That is the freeze: not a video defect but the whole
// guest stopping, with the player's own worker threads waiting alongside it.
//
// That is what happened on this title's previous runtime, which had no
// interrupt-driven retire at all, and draining the counter by hand after each
// Render was the fix there.
//
// Xenos delivers that interrupt. Plume walks PM4_INTERRUPT but the movie
// player still blocks: after BG1_P is cancelled for EAHD the guest never
// reaches player 28→29, and FrontEnd::Update stops. Drain the counter on
// plume the same way the previous runtime did. Leave xenos alone — a second
// drain there underflows to 0xFFFFFFFF. XERENGE_FRAME_RETIRE forces it on
// either backend.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"

extern "C" void __imp__sub_82482680(PPCContext& __restrict, uint8_t*);  // Render
extern "C" void __imp__sub_824823F0(PPCContext& __restrict, uint8_t*);  // retire callback
extern "C" void __imp__sub_824822C8(PPCContext& __restrict, uint8_t*);  // Stop
extern "C" void __imp__sub_82482430(PPCContext& __restrict, uint8_t*);  // Close

namespace {

constexpr uint32_t kInFlightOffset = 368;

bool PlumePresentationActive() {
  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state || !kernel_state->emulator()) {
    return false;
  }
  auto* graphics = kernel_state->emulator()->graphics_system();
  return graphics && graphics->uses_direct_presentation();
}

bool Enabled() {
  static const bool forced = std::getenv("XERENGE_FRAME_RETIRE") != nullptr;
  return forced || PlumePresentationActive();
}

bool Tracing() {
  static const bool on = std::getenv("XERENGE_MOVIE_TRACE") != nullptr;
  return on;
}

uint32_t InFlight(const uint8_t* base, uint32_t renderer) {
  return xerenge::LoadGuestU32(base, renderer + kInFlightOffset);
}

// Bounded, so a counter that never falls - a renderer already torn down, or one
// misread here - cannot turn the drain into a hang of its own.
void Retire(PPCContext& ctx, uint8_t* base, uint32_t renderer, uint32_t leave_in_flight) {
  for (int guard = 0; guard < 8 && InFlight(base, renderer) > leave_in_flight; ++guard) {
    ctx.r3.u32 = renderer;
    __imp__sub_824823F0(ctx, base);
  }
}

}  // namespace

REX_HOOK_RAW(sub_82482680) {
  const uint32_t renderer = ctx.r3.u32;
  if (Enabled()) {
    // Render refuses the blit when 3 frames are already outstanding. If the
    // CP interrupt never retires them, the player retries forever on the
    // main thread and the title stops.
    if (InFlight(base, renderer) >= 3) {
      Retire(ctx, base, renderer, 2);
      ctx.r3.u32 = renderer;
    }
  }
  __imp__sub_82482680(ctx, base);
  if (!Enabled()) {
    return;
  }
  // Keep the guest-visible result: the retire callback writes its own return
  // value to r3, and leaking that makes the caller read a successful frame
  // submission as a failure.
  const uint64_t render_result = ctx.r3.u64;
  if (Tracing()) {
    static std::atomic<uint32_t> calls{0};
    const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      std::cerr << "movie: render #" << n << " in flight " << InFlight(base, renderer) << '\n';
    }
  }
  Retire(ctx, base, renderer, 1);
  ctx.r3.u64 = render_result;
}

REX_HOOK_RAW(sub_824822C8) {
  const uint32_t renderer = ctx.r3.u32;
  if (Enabled()) {
    Retire(ctx, base, renderer, 0);
    ctx.r3.u32 = renderer;
  }
  __imp__sub_824822C8(ctx, base);
}

REX_HOOK_RAW(sub_82482430) {
  const uint32_t renderer = ctx.r3.u32;
  if (Enabled()) {
    Retire(ctx, base, renderer, 0);
    ctx.r3.u32 = renderer;
  }
  __imp__sub_82482430(ctx, base);
}
