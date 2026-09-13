// Optional render patches, after the Xenia patch set for this title by boma.
//
// Xenia applies them by writing over the guest's instructions. Nothing here can
// do that: the title's code is recompiled to native code ahead of time, so the
// bytes those patches overwrite are never executed. The same effect is had by
// intercepting the functions instead, which is what this does - and it is the
// more faithful translation, because each of those patches only ever made a
// function return early or return a different constant.
//
// The addresses were checked against this image rather than assumed: all three
// functions of the first patch exist here, and the byte of the second falls on
// `li r3,0` inside sub_821436D0, which the patch turns into `li r3,1`.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/ppc.h>

namespace {
bool SkipSkyBloom() {
  static const bool enabled = std::getenv("XERENGE_NO_BLOOM") != nullptr;
  return enabled;
}

bool SkipMotionBlur() {
  static const bool enabled = std::getenv("XERENGE_NO_MOTION_BLUR") != nullptr;
  return enabled;
}
}  // namespace

extern "C" void __imp__sub_821439E8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82173190(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8215B068(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821436D0(PPCContext& __restrict, uint8_t*);

// Sky bloom and the low-resolution gaussian blur. Xenia writes `blr` over the
// first instruction of each, so the function returns without doing anything;
// not calling the original is the same thing.
REX_HOOK_RAW(sub_821439E8) {
  if (SkipSkyBloom()) {
    return;
  }
  __imp__sub_821439E8(ctx, base);
}

REX_HOOK_RAW(sub_82173190) {
  if (SkipSkyBloom()) {
    return;
  }
  __imp__sub_82173190(ctx, base);
}

REX_HOOK_RAW(sub_8215B068) {
  if (SkipSkyBloom()) {
    return;
  }
  __imp__sub_8215B068(ctx, base);
}

// Motion blur and radial blur. The function reads a halfword through its
// argument and returns 0 when it is not positive; the patch makes that path
// return 1 instead. The other path is left alone, so the condition is evaluated
// here and the original is still called whenever it would be taken.
REX_HOOK_RAW(sub_821436D0) {
  if (SkipMotionBlur()) {
    uint16_t raw = 0;
    std::memcpy(&raw, base + ctx.r3.u32, sizeof(raw));
    const int16_t value = int16_t(__builtin_bswap16(raw));
    if (value <= 0) {
      ctx.r3.u64 = 1;
      return;
    }
  }
  __imp__sub_821436D0(ctx, base);
}
