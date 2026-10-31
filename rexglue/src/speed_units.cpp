// Kilometres per hour in place of miles (GitHub #55): speed_units = kph in
// xerenge.conf.
//
// The game has both: the speedometer, its MPH/KPH word (the pointers at
// 0x825F3488) and the speeds in the menus are chosen by one test, made inline
// wherever they are drawn - the language the strings were loaded in
// (0x82825BCC, written by the string table's loader sub_8210FD88), 0 or 1 being
// English and so miles, any other kilometres. An English game always showed
// miles. The functions making that test are wrapped here, and while one runs
// the word reads as another language's; the strings, and the two functions that
// choose by the language itself (sub_821F6100, sub_821F6AF0), are left alone.
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "xerenge_settings.h"

namespace {

constexpr uint32_t kLanguage = 0x82825BCC;
constexpr uint32_t kNotEnglish = 2;

// speed_units in xerenge.conf: kph, mph, or the game's own choice (unset).
// Read at the start, then as Settings changes it (SetKilometres): the pause
// menu's SPEED row takes effect in the race at once.
int g_kph = -1;

bool Kilometres() {
  if (g_kph < 0) {
    g_kph = xerenge::settings::ConfString("speed_units", "") == "kph" ? 1 : 0;
    if (g_kph) {
      REXLOG_INFO("units: kilometres per hour (speed_units = kph)");
    }
  }
  return g_kph == 1;
}

// The word as the test sees it, for one call: an English game's 0 or 1 made
// another language's; restored after, whatever the call did.
struct Metric {
  uint8_t* base;
  uint32_t saved = 0;
  bool changed = false;
  explicit Metric(uint8_t* base_) : base(base_) {
    if (!Kilometres()) {
      return;
    }
    saved = xerenge::LoadGuestU32(base, kLanguage);
    if (saved <= 1) {
      xerenge::StoreGuestU32(base, kLanguage, kNotEnglish);
      changed = true;
    }
  }
  ~Metric() {
    if (changed) {
      xerenge::StoreGuestU32(base, kLanguage, saved);
    }
  }
};

}  // namespace

#define METRIC_HOOK(name)                                             \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*);   \
  REX_HOOK_RAW(name) {                                                \
    Metric metric(base);                                              \
    __imp__##name(ctx, base);                                         \
  }

// The speedometer and the HUD's units, the race results' speeds, the menus'.
METRIC_HOOK(sub_8219FAD0)
METRIC_HOOK(sub_821A06F8)
METRIC_HOOK(sub_821A1078)
METRIC_HOOK(sub_821A1520)
METRIC_HOOK(sub_821A38D8)
METRIC_HOOK(sub_821A4090)
METRIC_HOOK(sub_82188F98)
METRIC_HOOK(sub_821EA3E8)
METRIC_HOOK(sub_821EC110)
METRIC_HOOK(sub_821F8590)
METRIC_HOOK(sub_821F8650)
METRIC_HOOK(sub_821F8748)
METRIC_HOOK(sub_820AD618)

namespace xerenge::units {

void SetKilometres(bool kph) {
  g_kph = kph ? 1 : 0;
  // The number follows at once; the word beside it is the HUD's own copy,
  // made when it was built, and follows from the next event.
  REXLOG_INFO("units: {} per hour", kph ? "kilometres" : "miles");
}

bool KilometresShown() {
  return Kilometres();
}

}  // namespace xerenge::units
