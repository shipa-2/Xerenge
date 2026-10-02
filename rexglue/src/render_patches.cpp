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
#include <rex/logging.h>
#include <rex/ppc.h>
#include "guest_memory.h"

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
    std::memcpy(&raw, xerenge::GuestPointer(base, ctx.r3.u32), sizeof(raw));
    const int16_t value = int16_t(__builtin_bswap16(raw));
    if (value <= 0) {
      ctx.r3.u64 = 1;
      return;
    }
  }
  __imp__sub_821436D0(ctx, base);
}

// D3DX's CImage::LoadJPG (0x823C2CF0), on saving a Burnout clip. Its libjpeg
// reports a bad stream by longjmp-ing out of its error handler, which the
// recompiled code cannot do: decoding went on past the error and read through
// a null decompressor at +0x1A4 (sub_823D85F8, its marker reader) for good.
// Saying the image cannot be read is what the title is ready for - a failed
// D3DX call - and the clip saves without that picture.
extern "C" void __imp__sub_823C2CF0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_823C2CF0) {
  (void)base;
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_WARN("D3DX LoadJPG refused (libjpeg cannot unwind its errors here)");
  }
  ctx.r3.u64 = 0x80004005u;  // E_FAIL
}
