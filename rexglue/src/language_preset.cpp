// XERENGE_LANGUAGE=<n>: the title's language chosen ahead of time, so the
// language select screen does not come up at every start.
//
// CB4LanguageSelectState::Action (0x8220F660) asks
// CB4GamertagManager::CheckForStoredLanguage (0x8210D8F0 - the byte at +0x330)
// whether a language was stored, and if so hands the stored one (+0x334) to
// CB4LanguageManager::SetLanguage (0x8210FA20) and moves on without showing
// the screen. The answer is given here, with the language asked for.
//
// n is the title's EGtLanguage. From CGtSysConfig::GetLanguage: 0 English,
// 1 English (US), 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian,
// 7 Korean, 8 Chinese, 9 Portuguese. The PAL disc's others (Dutch, Swedish,
// Finnish) are not in that table; SetLanguage logs the value it is given, so
// choosing one on the screen once says its number.
#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "guest_memory.h"

extern "C" void __imp__sub_8210D8F0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8210FA20(PPCContext& __restrict, uint8_t*);

namespace {

// -1 when unset.
int PresetLanguage() {
  static const int language = [] {
    const char* text = std::getenv("XERENGE_LANGUAGE");
    if (text == nullptr || *text == '\0') {
      return -1;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    return (end != text && value >= 0 && value < 32) ? int(value) : -1;
  }();
  return language;
}

}  // namespace

// CB4GamertagManager::CheckForStoredLanguage(this) -> bool.
REX_HOOK_RAW(sub_8210D8F0) {
  const uint32_t manager = ctx.r3.u32;
  __imp__sub_8210D8F0(ctx, base);
  const int language = PresetLanguage();
  if (language < 0 || manager == 0) {
    return;
  }
  xerenge::StoreGuestU32(base, manager + 0x334, uint32_t(language));
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("language: preset {} (XERENGE_LANGUAGE); no language select", language);
  }
  ctx.r3.u64 = 1;
}

// CB4LanguageManager::SetLanguage(this, EGtLanguage).
REX_HOOK_RAW(sub_8210FA20) {
  static uint32_t last = ~0u;
  if (ctx.r4.u32 != last) {
    last = ctx.r4.u32;
    REXLOG_INFO("language: the title set language {}", last);
  }
  __imp__sub_8210FA20(ctx, base);
}
