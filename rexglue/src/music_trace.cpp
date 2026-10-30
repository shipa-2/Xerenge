// The soundtrack's track skip, for finding why it is ignored in some modes
// (Crash Mode). Every frame sub_822D0138 runs the music update sub_822CB4F0:
//   - the XMP state comes from XMP notifications (sub_822AD638);
//   - the skip control is sub_8210F940 on the action at 0x82846670, true when
//     a pad of the mode's active players (game state bytes +2441..+2444) has
//     it pressed;
//   - pressed, with +784 clear: +784 set, XMPStop (sub_822AD778); the stop's
//     notification plays the next song; +784 clears when XMP says "playing".
// In Crash mode the press was lost (two logic steps, two pad polls, a frame;
// the "just pressed" edge gone before the once-a-frame music update looked):
// the hook on sub_8210F940 also takes the press as frames see it. Logged only
// when something changes: whether the press is seen, the action's pad
// binding, and the update's fields.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>

#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "guest_memory.h"

extern "C" void __imp__sub_8210F940(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822CB4F0(PPCContext& __restrict, uint8_t*);

namespace {

constexpr uint32_t kSkipAction = 0x82846670;

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t raw = 0;
  std::memcpy(&raw, xerenge::GuestPointer(base, address), sizeof(raw));
  return __builtin_bswap32(raw);
}

uint8_t LoadU8(uint8_t* base, uint32_t address) {
  return *xerenge::GuestPointer(base, address);
}

// LB: the song before (GitHub #55). Set by the music update's hook, taken by
// the skip control's, which then answers pressed - the game's own skip, after
// the playing order's next place (+740) was moved back.
bool g_back_pending = false;

uint32_t g_lines = 0;
bool Room() {
  return g_lines < 400 && ++g_lines;
}

}  // namespace

REX_HOOK_RAW(sub_8210F940) {
  const uint32_t action = ctx.r3.u32;
  if (action == kSkipAction) {
    // The pads the action is bound to: up to two port indices (+2441, +2442),
    // each used only when its flag is set (+2443, +2444).
    static uint32_t last_binding = ~0u;
    const uint32_t binding = (uint32_t(LoadU8(base, action + 2441)) << 24) |
                             (uint32_t(LoadU8(base, action + 2442)) << 16) |
                             (uint32_t(LoadU8(base, action + 2443)) << 8) |
                             LoadU8(base, action + 2444);
    if (binding != last_binding && Room()) {
      REXLOG_INFO("music: track skip bound to port {} ({}) and port {} ({})",
                  int8_t(binding >> 24), (binding >> 8) & 0xFF ? "on" : "off",
                  int8_t(binding >> 16), binding & 0xFF ? "on" : "off");
    }
    last_binding = binding;
  }
  __imp__sub_8210F940(ctx, base);
  if (action == kSkipAction && g_back_pending) {
    g_back_pending = false;
    ctx.r3.u64 = 1;
    return;
  }
  if (action == kSkipAction) {
    // The press as a frame sees it. The control is "just pressed": button 20
    // down now (+37 + 20 of the pad) and not at the poll before (+9 + 20). The
    // pad is polled once per logic step, and Crash mode runs two a frame: by
    // the time the music update looks, once a frame, the second poll has made
    // "before" equal "now" and the press is gone - it only ever got through
    // when it fell between the two polls. Down now and not a frame ago, on a
    // pad the action is bound to, counts as well.
    static bool was_down = false;
    bool down = false;
    for (int i = 0; i < 2; ++i) {
      if (LoadU8(base, action + 2443 + i) == 0) {
        continue;
      }
      const int8_t port = int8_t(LoadU8(base, action + 2441 + i));
      if (port < 0 || port >= 4) {
        continue;
      }
      const uint32_t pad = LoadU32(base, action + 1000 + uint32_t(port) * 360 + 4);
      if (pad >= 0x10000 && LoadU8(base, pad + 8) != 0 && LoadU8(base, pad + 37 + 20) != 0) {
        down = true;
      }
    }
    if (down && !was_down) {
      ctx.r3.u64 = 1;
    }
    was_down = down;
    static int last = -1;
    const int pressed = (ctx.r3.u32 & 0xFF) != 0;
    if (pressed != last && Room()) {
      REXLOG_INFO("music: track skip control {}", pressed ? "pressed" : "released");
    }
    last = pressed;
  }
}

REX_HOOK_RAW(sub_822CB4F0) {
  const uint32_t music = ctx.r3.u32;
  // XERENGE_DUMP_IMAGE=<file>: the title's loaded image (82000000-82DA0000) written once,
  // for looking up its strings and data offline.
  static bool dumped = false;
  if (!dumped) {
    dumped = true;
    if (const char* path = std::getenv("XERENGE_DUMP_IMAGE")) {
      if (FILE* file = std::fopen(path, "wb")) {
        std::fwrite(xerenge::GuestPointer(base, 0x82000000), 1, 0xDA0000, file);
        std::fclose(file);
        REXLOG_INFO("music: image dumped to {}", path);
      }
    }
  }
  // LB: back to the song's start, or - within its first three seconds - to the
  // song before (GitHub #55). The song is started again by the game's own skip
  // (RB's): the playing order's next place (+740, an index into the order at
  // +192 of the +736 songs on) moved back one place - the song playing - or two
  // - the one before it.
  {
    using Clock = std::chrono::steady_clock;
    static uint32_t song = ~0u;
    static Clock::time_point started = Clock::now();
    static bool lb_was = false;
    const uint32_t playing = LoadU32(base, music + 748);
    if (playing != song) {
      song = playing;
      started = Clock::now();
    }
    auto* kernel_state = REX_KERNEL_STATE();
    auto* input = kernel_state ? static_cast<rex::input::InputSystem*>(kernel_state->emulator()->input_system())
                               : nullptr;
    rex::input::X_INPUT_STATE state{};
    const bool lb = input && input->GetState(0, &state) == 0 && (uint16_t(state.gamepad.buttons) & 0x0100);
    const uint32_t count = LoadU32(base, music + 736);
    if (lb && !lb_was && count != 0 && LoadU32(base, music + 784) == 0 && LoadU32(base, music + 20) != 2) {
      const bool restart = Clock::now() - started > std::chrono::seconds(3);
      const uint32_t back = restart || count == 1 ? 1 : 2;
      const uint32_t next = LoadU32(base, music + 740) % count;
      const uint32_t place = (next + count * 2 - back) % count;
      xerenge::StoreGuestU32(base, music + 740, place);
      g_back_pending = true;
      started = Clock::now();
      REXLOG_INFO("music: LB - {}", restart ? "the song from its start" : "the song before");
    }
    lb_was = lb;
  }
  __imp__sub_822CB4F0(ctx, base);
  struct Fields {
    uint32_t mode, next_song, skipping;
    uint8_t playing, blocked;
    bool operator!=(const Fields& o) const {
      return mode != o.mode || next_song != o.next_song || skipping != o.skipping ||
             playing != o.playing || blocked != o.blocked;
    }
  };
  const Fields now{LoadU32(base, music + 20), LoadU32(base, music + 736),
                   LoadU32(base, music + 784), LoadU8(base, music + 24), LoadU8(base, music + 25)};
  static Fields last{~0u, ~0u, ~0u, 0xFF, 0xFF};
  if (now != last && Room()) {
    REXLOG_INFO("music: update {:08X}: mode {} (+20), next {:08X} (+736), skipping {} (+784), "
                "+24 {}, blocked {} (+25)",
                music, now.mode, now.next_song, now.skipping, now.playing, now.blocked);
  }
  last = now;
  // What sub_822ADEE8 (called from this update) waits for before resuming the
  // soundtrack after a movie: this word at 4.
  static uint32_t last_gate = ~0u;
  const uint32_t gate = LoadU32(base, 0x82A538C0 + 22348);
  if (gate != last_gate && Room()) {
    REXLOG_INFO("music: resume gate {:08X} now {} (resumes at 4)", 0x82A538C0 + 22348, gate);
  }
  last_gate = gate;
}

// EA Trax going quiet after a movie (GitHub #29): the soundtrack is paused for
// a movie (sub_822ADD90) and resumed after it (sub_822ADE70), both on the
// music object, by its state at +776 (1 the title's playlist, 2 the player's
// own music, 3 resumed). Logged with who asked, to find the path a skipped
// movie takes without resuming.
extern "C" void __imp__sub_822ADD90(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822ADE70(PPCContext& __restrict, uint8_t*);

REX_HOOK_RAW(sub_822ADD90) {
  const uint32_t music = ctx.r3.u32;
  const uint32_t caller = ctx.lr;
  const uint32_t state = LoadU32(base, music + 776);
  __imp__sub_822ADD90(ctx, base);
  REXLOG_INFO("music: paused for a movie by {:08X}: state {} -> {}, +764 {}, +25 {}", caller, state,
              LoadU32(base, music + 776), LoadU8(base, music + 764), LoadU8(base, music + 25));
}

REX_HOOK_RAW(sub_822ADE70) {
  const uint32_t music = ctx.r3.u32;
  const uint32_t caller = ctx.lr;
  const uint32_t state = LoadU32(base, music + 776);
  __imp__sub_822ADE70(ctx, base);
  REXLOG_INFO("music: resumed after a movie by {:08X}: state {} -> {}, +764 {}, +25 {}", caller, state,
              LoadU32(base, music + 776), LoadU8(base, music + 764), LoadU8(base, music + 25));
}

// The other callers of XMP's pause (0x8259B7A8) and resume (0x8259B758): the
// soundtrack came back after a skipped movie through none of the two above.
extern "C" void __imp__sub_822AE9B8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822AE9F0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822CB488(PPCContext& __restrict, uint8_t*);

REX_HOOK_RAW(sub_822AE9B8) {
  REXLOG_INFO("music: sub_822AE9B8 (pauses) called by {:08X}", uint32_t(ctx.lr));
  __imp__sub_822AE9B8(ctx, base);
}

REX_HOOK_RAW(sub_822AE9F0) {
  REXLOG_INFO("music: sub_822AE9F0 (resumes) called by {:08X}", uint32_t(ctx.lr));
  __imp__sub_822AE9F0(ctx, base);
}

REX_HOOK_RAW(sub_822CB488) {
  static uint32_t last_caller = 0;
  static uint32_t calls = 0;
  ++calls;
  if (ctx.lr != last_caller || calls % 600 == 1) {
    REXLOG_INFO("music: sub_822CB488 (resumes) called by {:08X} ({} call(s))", uint32_t(ctx.lr), calls);
  }
  last_caller = ctx.lr;
  __imp__sub_822CB488(ctx, base);
}

