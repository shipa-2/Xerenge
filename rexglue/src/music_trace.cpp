// The soundtrack's track skip, for finding why it is ignored in some modes
// (Crash Mode). Every frame sub_822D0138 runs the music update sub_822CB4F0:
//   - the XMP state comes from XMP notifications (sub_822AD638);
//   - the skip control is sub_8210F940 on the action at 0x82846670, true when
//     a pad of the mode's active players (game state bytes +2441..+2444) has
//     it pressed;
//   - pressed, with +784 clear: +784 set, XMPStop (sub_822AD778); the stop's
//     notification plays the next song; +784 clears when XMP says "playing".
// Logged only when something changes: whether the press is seen, and the
// update's fields.
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
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

uint32_t g_lines = 0;
bool Room() {
  return g_lines < 400 && ++g_lines;
}

}  // namespace

REX_HOOK_RAW(sub_8210F940) {
  const uint32_t action = ctx.r3.u32;
  __imp__sub_8210F940(ctx, base);
  if (action == kSkipAction) {
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
}
