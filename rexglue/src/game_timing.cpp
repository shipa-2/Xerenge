// The game's clock at frame rates above sixty.
//
// Burnout runs its logic in fixed steps of a sixtieth of a second, counted by
// a timer object that sub_82363E68 advances once per frame. It returns how
// many steps the frame should run, and the next frame runs them (the loop at
// 0x8212021C, which also polls the pad once per step). Its layout:
//
//   +0x00  mode: 0 = frame-locked, otherwise wall-clock
//   +0x08  last QueryPerformanceCounter value
//   +0x10  time accumulated since the start
//   +0x18  lag threshold
//   +0x20  step length, in timebase ticks
//   +0x28  steps returned last time (folded into +0x2C on the next call)
//   +0x2C  steps run in total
//   +0x30  extra steps requested
//   +0x34  paused
//
// In the wall-clock mode it returns min + ceil((lag - threshold) / step),
// where lag is the time accumulated minus the steps already run, capped at
// max. In the frame-locked mode it keeps that account but returns min
// regardless - and the caller (sub_8211FA28) passes 1, so every frame is one
// step. That is what this title runs in: on a console locked to sixty frames
// a second it is the same thing, but rendered faster the whole game ran
// faster, 1.2x at 75 frames a second on the race timer against the wall clock.
//
// With the frame rate unlocked the timer is switched to its wall-clock mode
// with a minimum of 0: a frame that comes before the next step is due runs no
// logic, a late one catches up (up to the caller's max), and the world
// advances sixty steps a second whatever the frame rate. At the ordinary
// sixty frames a second the title's own settings are left alone.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "frame_clock_provider.h"
#include "guest_memory.h"
#include "widescreen.h"

namespace {
// What the timer returned last (the steps the coming frame runs), and the one
// before (the steps the current frame has run). The timer is called before
// the frame is drawn, so by the time the interface is drawn the first is
// already the next frame's.
uint32_t g_steps_next_frame = 1;
uint32_t g_steps_this_frame = 1;

// What the renderer interpolates by (see xerenge_frame_clock).
struct FrameClock {
  uint64_t frame = 0;           // drawn frames so far
  uint32_t steps_this_frame = 0;
  uint32_t epoch = 0;           // changes when the timer starts over
  double state_steps = 0.0;     // logic steps the drawn state is at
  double real_steps = 0.0;      // wall time, in steps, on the same clock
  uint32_t valid = 0;
  // Vblanks the title means each frame to last: the minimum of steps it asks
  // the timer for - 1, or 2 in Crash mode, which runs at thirty frames a
  // second with its logic still at sixty. The renderer paces swaps by it.
  uint32_t vblanks_per_frame = 1;
};
FrameClock g_clock;
uint32_t g_clock_timer = 0;

// How long the drawn frame took, as a multiple of a logic step, and whether a
// function drawing it (or otherwise running once per drawn frame) is asking.
double g_frame_scale = 1.0;
int g_per_frame_depth = 0;

bool LogicFollowsWallClock() {
  static const bool enabled = std::getenv("XERENGE_UNLOCK_FPS") != nullptr ||
                              std::getenv("XERENGE_NO_VBLANK_PACE") != nullptr ||
                              std::getenv("XERENGE_WALL_CLOCK_LOGIC") != nullptr;
  return enabled;
}

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  return xerenge::LoadGuestU32(base, address);
}

uint64_t LoadU64(const uint8_t* base, uint32_t address) {
  return xerenge::LoadGuestU64(base, address);
}
}  // namespace

extern "C" void __imp__sub_82363E68(PPCContext& __restrict, uint8_t*);


// sub_82363E68(timer, min_steps, max_steps) -> steps to run next frame.
REX_HOOK_RAW(sub_82363E68) {
  const uint32_t timer = ctx.r3.u32;
  g_clock.vblanks_per_frame = ctx.r4.u32 == 2 ? 2u : 1u;
  if (LogicFollowsWallClock() && timer >= 0x10000 && timer < 0xA0000000u) {
    const uint32_t mode = LoadU32(base, timer);
    static bool shown = false;
    if (!shown) {
      shown = true;
      REXLOG_INFO("game timing: timer {:08X} mode {} step {} ticks, threshold {}, min {} max {}"
                  " - switching to wall-clock steps",
                  timer, mode, LoadU64(base, timer + 0x20), LoadU64(base, timer + 0x18),
                  ctx.r4.u32, ctx.r5.u32);
    }
    if (mode == 0) {
      xerenge::StoreGuestU32(base, timer, 1u);
    }
    // Not before it has started: until its step count is positive the timer
    // only takes the time and hands back the minimum, and with a minimum of 0
    // the count never became positive - the logic never ran a step at all.
    const int32_t started = int32_t(LoadU32(base, timer + 0x2C)) +
                            int32_t(LoadU32(base, timer + 0x28));
    if (started > 0) {
      ctx.r4.u64 = 0;
    }
  }
  __imp__sub_82363E68(ctx, base);
  {
    // Once per drawn frame: the wall time since the last one, in steps.
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    g_frame_scale = std::clamp(seconds * 60.0, 0.0, 4.0);
  }
  g_steps_this_frame = g_steps_next_frame;
  g_steps_next_frame = ctx.r3.u32;
  if (LogicFollowsWallClock() && timer >= 0x10000 && timer < 0xA0000000u) {
    // After the call: +0x2C holds every step run so far, this frame's
    // included, and +0x10 the time accumulated up to now.
    const double period = double(LoadU64(base, timer + 0x20));
    const double state = double(int32_t(LoadU32(base, timer + 0x2C)));
    const double real = period > 0.0 ? double(int64_t(LoadU64(base, timer + 0x10))) / period : 0.0;
    if (timer != g_clock_timer || state < g_clock.state_steps) {
      ++g_clock.epoch;
      g_clock_timer = timer;
    }
    ++g_clock.frame;
    g_clock.steps_this_frame = g_steps_this_frame;
    g_clock.state_steps = state;
    g_clock.real_steps = real;
    g_clock.valid = period > 0.0 && LoadU32(base, timer) != 0 ? 1u : 0u;
  }
  if (LogicFollowsWallClock()) {
    static uint64_t calls = 0, steps = 0;
    ++calls;
    steps += ctx.r3.u32;
    if (calls % 2000 == 0) {
      REXLOG_INFO("game timing: {} frames, {} steps so far (mode now {}, lag base {} / {} run)",
                  calls, steps, LoadU32(base, timer), LoadU64(base, timer + 0x10),
                  LoadU32(base, timer + 0x2C));
    }
  }
}

extern "C" void __imp__sub_824288F8(PPCContext& __restrict, uint8_t*);

// AptUpdate(milliseconds) - no symbol, but it is what CB4AptManager::Render
// (0x821FF308) hands the frame time to, and it runs the interface's movie
// clips, its linker and its garbage collector. Render is called once per drawn
// frame with a sixtieth of a second, so drawn at a thousand frames a second
// the menus played through in a fraction of one and took input as fast. It
// runs once per logic step instead - as it did on the console, where a step
// and a frame were the same thing: not at all on a frame without one, twice
// on a frame that caught up two.
REX_HOOK_RAW(sub_824288F8) {
  if (!LogicFollowsWallClock()) {
    __imp__sub_824288F8(ctx, base);
    return;
  }
  const uint64_t milliseconds = ctx.r3.u64;
  for (uint32_t i = 0; i < g_steps_this_frame; ++i) {
    ctx.r3.u64 = milliseconds;
    __imp__sub_824288F8(ctx, base);
  }
}

extern "C" void __imp__sub_821F5B10(PPCContext& __restrict, uint8_t*);

// The menus' input: CB4AptManager::Render calls this just before AptUpdate,
// and it reads the menu buttons (CB4InputManager::GetMenuButton, AnyInput)
// and hands them to the interface, repeats of a held direction included. Once
// per drawn frame that was hundreds of times a second. Once per frame that ran
// a logic step instead, and not at all on a frame that did not.
REX_HOOK_RAW(sub_821F5B10) {
  if (LogicFollowsWallClock() && g_steps_this_frame == 0) {
    return;
  }
  __imp__sub_821F5B10(ctx, base);
}

// The frame time the title's systems ask for (0x8210B3C8): a sixtieth of a
// second, the length of one step. Right inside the logic steps. Wrong for the
// ones that run once per drawn frame - the world's and the sparks' drawing,
// the post-processing, the sound manager - which with hundreds of frames a
// second advanced their fades and timers that many times too fast. Asked from
// inside one of those, it answers with the drawn frame's own length.
extern "C" void __imp__sub_8210B3C8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8210B3C8) {
  __imp__sub_8210B3C8(ctx, base);
  if (LogicFollowsWallClock() && g_per_frame_depth > 0) {
    ctx.f1.f64 = double(float(ctx.f1.f64 * g_frame_scale));
  }
}

#define PER_FRAME_HOOK(name)                                  \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*); \
  REX_HOOK_RAW(name) {                                        \
    ++g_per_frame_depth;                                      \
    __imp__##name(ctx, base);                                 \
    --g_per_frame_depth;                                      \
  }

// The same, for calls that draw interface: their draws also keep the 16:9 box
// on a wider screen (widescreen.cpp).
#define PER_FRAME_INTERFACE_HOOK(name)                             \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*); \
  REX_HOOK_RAW(name) {                                             \
    ++g_per_frame_depth;                                           \
    xerenge::EnterInterface();                                     \
    __imp__##name(ctx, base);                                      \
    xerenge::LeaveInterface();                                     \
    --g_per_frame_depth;                                           \
  }

PER_FRAME_HOOK(sub_820EE348)  // CB4World::Render
PER_FRAME_HOOK(sub_821632E0)  // CB4SparkArray::Render
PER_FRAME_HOOK(sub_82182438)  // CB4PostProcessRenderer::Render
PER_FRAME_INTERFACE_HOOK(sub_820B4BC0)  // CB4FlashManager::Render
PER_FRAME_INTERFACE_HOOK(sub_821FF008)  // CB4CustomRevengeMeter::Render
PER_FRAME_HOOK(sub_822AE800)  // CB4SoundManager::Update

// For the renderer (plume reads rex_frame_clock_provider): where the drawn state is
// in logic steps and where the wall clock is, so frames drawn between two
// steps can be drawn between the two states.
extern "C" REX_HOST_EXPORT RexFrameClockProviderFn rex_frame_clock_provider = nullptr;

namespace {
void CopyFrameClock(void* out, size_t size) {
  if (out && size >= sizeof(FrameClock)) {
    *static_cast<FrameClock*>(out) = g_clock;
  }
}
const bool g_clock_registered = [] {
  rex_frame_clock_provider = &CopyFrameClock;
  return true;
}();
}  // namespace
