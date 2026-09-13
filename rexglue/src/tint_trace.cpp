// The frontend's tint, at its source.
//
// The 2D layer's colour comes from one shader constant, c2, which the frontend's
// vertex shader passes out and its pixel shader multiplies the texture by. The
// whole GPU chain below that has been verified faithful - the uniform buffer
// matches the register file component for component - so the wrong value
// arrives already wrong, computed by the guest.
//
// AptCIH::render (0x8247BDD8) takes the clip transform, which is where a Flash
// clip's colour transform lives before it becomes that constant. Dumping it
// here, on both this runtime and the one that renders these colours correctly,
// puts the divergence somewhere specific instead of somewhere in "the guest".
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <rex/hook.h>
#include <rex/ppc.h>

extern "C" void __imp__sub_8247BDD8(PPCContext& __restrict, uint8_t*);

REX_HOOK_RAW(sub_8247BDD8) {
  static const bool trace = std::getenv("XERENGE_TINT_TRACE") != nullptr;
  if (trace) {
    // The root clip's transform is the identity and says nothing; only the ones
    // that actually tint something are worth reporting, and only once each.
    const uint32_t transform = ctx.r4.u32;
    float words[12];
    for (uint32_t word = 0; word < 12; ++word) {
      uint32_t raw = 0;
      std::memcpy(&raw, base + transform + word * 4, sizeof(raw));
      raw = __builtin_bswap32(raw);
      std::memcpy(&words[word], &raw, sizeof(words[word]));
    }
    const bool identity = words[0] == 1.0f && words[1] == 1.0f && words[2] == 1.0f &&
                          words[3] == 1.0f && words[4] == 0.0f && words[5] == 0.0f &&
                          words[6] == 0.0f && words[7] == 0.0f;
    if (!identity) {
      static std::atomic<uint32_t> n{0};
      if (n.fetch_add(1, std::memory_order_relaxed) < 14) {
        std::cerr << "tint: multiply (" << words[0] << ',' << words[1] << ',' << words[2] << ','
                  << words[3] << ")  add (" << words[4] << ',' << words[5] << ',' << words[6]
                  << ',' << words[7] << ")\n";
      }
    }
  }
  __imp__sub_8247BDD8(ctx, base);
}
