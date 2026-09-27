// Where the intro movies stop, when they stop.
//
// They start sometimes and not others, with nothing in the log either way, so
// this reports the path itself. The addresses and what they mean were
// established while bringing this title up on the previous runtime:
//
//   0x821FEBC0  CB4VideoManager::PlayVideo - the descriptor names the clip
//   0x821FF558  CB4AptManager::RenderVideoComponent - drives the whole intro
//   0x8248D398  the movie player's status setter, reached through its vtable
//   0x82357CC0  the decoder's answer to "has this clip ended"
//
// RenderVideoComponent is the one that matters: every intro screen advances on
// the APT manager's video status, and that status is a five-way state machine
// driven only from there - 1 parses the descriptor and starts the clip, 2 waits
// for the player to reach state 0x1C, 3 renders a frame, and 0 and 4 stop the
// player. A run where the movie never appears will stop somewhere in that
// sequence, and this says where.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>

#include <rex/hook.h>
#include <rex/ppc.h>

namespace {

constexpr uint32_t kAptVideoStatus = 0x82A5900Cu;   // CB4AptManager + 0x574C
constexpr uint32_t kVideoPlayerState = 0x82A53360u;  // CB4VideoManager + 0x98

bool Tracing() {
  static const bool on = std::getenv("XERENGE_MOVIE_TRACE") != nullptr;
  return on;
}

// Wall-clock time in the SDK log's format, so a line here lines up with the
// log file and with a profile.
std::string Stamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count() % 1000;
  std::tm local{};
  localtime_r(&seconds, &local);
  char text[24];
  std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d ", local.tm_hour, local.tm_min,
                local.tm_sec, int(ms));
  return text;
}

uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value = 0;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

}  // namespace

extern "C" void __imp__sub_821FEBC0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821FF558(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8248D398(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82357CC0(PPCContext& __restrict, uint8_t*);

namespace {

// Whether the clip now playing is one of the publisher/developer logos shown
// before the title gets going. The game lets the last one be skipped but not
// these, which is a long wait to sit through on every launch while working on
// the title.
std::atomic<bool> g_playing_logo{false};
std::atomic<uint32_t> g_logo_calls{0};

bool SkipLogos() {
  static const bool on = std::getenv("XERENGE_SKIP_LOGOS") != nullptr;
  return on;
}

bool NameIsStartupLogo(const char* name) {
  if (!name) {
    return false;
  }
  // The menu background must keep playing - it is the backdrop the interface
  // is drawn over, not an intro. The attract clip is skippable in-game, but
  // skipping it by hand every launch is the same waste of time as the logos.
  static const char* const kLogos[] = {"EAHD_E_P", "EA_E_P", "CRRW_E_P", "ATTR_P"};
  for (const char* logo : kLogos) {
    if (std::strcmp(name, logo) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

REX_HOOK_RAW(sub_821FEBC0) {
  const uint32_t descriptor = ctx.r4.u32;
  const uint32_t name = descriptor != 0 ? LoadGuestU32(base, descriptor) : 0;
  const char* name_text = name != 0 ? reinterpret_cast<const char*>(base + name) : nullptr;
  if (Tracing()) {
    std::cerr << Stamp() << "movie: clip requested: " << (name_text ? name_text : "(none)") << '\n';
  }
  const bool is_logo = SkipLogos() && NameIsStartupLogo(name_text);
  g_playing_logo.store(is_logo, std::memory_order_relaxed);
  g_logo_calls.store(0, std::memory_order_relaxed);
  if (is_logo) {
    std::cerr << Stamp() << "movie: skipping logo clip " << name_text << " (XERENGE_SKIP_LOGOS)\n";
  }
  __imp__sub_821FEBC0(ctx, base);
}

REX_HOOK_RAW(sub_821FF558) {
  if (!Tracing()) {
    __imp__sub_821FF558(ctx, base);
    return;
  }
  const uint32_t status_before = LoadGuestU32(base, kAptVideoStatus);
  const uint32_t player_before = LoadGuestU32(base, kVideoPlayerState);
  const bool wanted = (ctx.r5.u32 & 0xFFu) != 0;
  __imp__sub_821FF558(ctx, base);
  const uint32_t status_after = LoadGuestU32(base, kAptVideoStatus);
  const uint32_t player_after = LoadGuestU32(base, kVideoPlayerState);
  if (status_after != status_before || player_after != player_before) {
    std::cerr << Stamp() << "movie: apt status " << status_before << " -> " << status_after << "  player "
              << player_before << " -> " << player_after << "  wanted=" << wanted << '\n';
  } else {
    // A status that never moves is the failure, so report it on a clock rather
    // than on a call count: a short session should still show a stall, and a
    // count says nothing about how long it has been stuck.
    // Status 3 is a clip playing, and it holds there for the clip's whole
    // length - reporting that as a stall was wrong and only added noise. Only
    // the states that should move on report.
    static std::atomic<uint32_t> idle{0};
    const uint32_t n = idle.fetch_add(1, std::memory_order_relaxed) + 1;
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (status_after != 3 && now - window >= std::chrono::seconds(2)) {
      window = now;
      std::cerr << Stamp() << "movie: apt status held at " << status_after << ", player " << player_after
                << " (" << n << " calls with no change)\n";
    }
  }
}

REX_HOOK_RAW(sub_8248D398) {
  if (Tracing()) {
    const uint32_t previous = LoadGuestU32(base, ctx.r3.u32 + 0xD8u);
    if (previous != ctx.r4.u32) {
      std::cerr << Stamp() << "movie: player status " << previous << " -> " << ctx.r4.u32 << '\n';
    }
  }
  __imp__sub_8248D398(ctx, base);
}

REX_HOOK_RAW(sub_82357CC0) {
  __imp__sub_82357CC0(ctx, base);
  if (g_playing_logo.load(std::memory_order_relaxed)) {
    // Told "finished" rather than torn down: the player is left to run its own
    // end-of-clip path, which is what advances the title to the next screen.
    // A few calls are let through first so the clip is properly started before
    // it is declared over - answering on the very first call unbalances the
    // state machine.
    if (g_logo_calls.fetch_add(1, std::memory_order_relaxed) >= 2) {
      ctx.r3.u32 = (ctx.r3.u32 & ~0xFFu) | 1u;
    }
  }
  if (Tracing()) {
    static std::atomic<uint32_t> last{0xFFFFFFFFu};
    const uint32_t answer = ctx.r3.u32 & 0xFFu;
    if (last.exchange(answer, std::memory_order_relaxed) != answer) {
      std::cerr << Stamp() << "movie: decoder says finished = " << answer << '\n';
    }
  }
}

// The APT side, to tell one failure from another.
//
// When the intro stalls, RenderVideoComponent simply stops being called - the
// state machine is not stuck, nobody is driving it. That leaves two
// possibilities: the whole Flash movie stopped advancing, or it kept advancing
// and only the video component fell out of what it draws. These report the
// movie's own pulse, so the two can be told apart:
//
//   0x8247C6C0  AptCIH::tick - one clip instance advanced by one frame
//   0x8247ECD8  AptMovie::doFrameControls
//   0x8247EE88  AptMovie::runFrameActions
//   0x821FD498  CB4FlashMovieManager::PlayMovie - which screen was asked for
extern "C" void __imp__sub_8247C6C0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8247ECD8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8247EE88(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821FD498(PPCContext& __restrict, uint8_t*);

namespace {

// Reports a heartbeat at most every two seconds, so a movie that keeps
// advancing while the video component is gone is visible without flooding.
void Heartbeat(const char* what, std::atomic<uint32_t>& counter,
               std::chrono::steady_clock::time_point& window) {
  const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto now = std::chrono::steady_clock::now();
  if (now - window >= std::chrono::seconds(2)) {
    window = now;
    std::cerr << Stamp() << "apt: " << what << " x" << n << '\n';
  }
}

}  // namespace

REX_HOOK_RAW(sub_8247C6C0) {
  if (Tracing()) {
    static std::atomic<uint32_t> n{0};
    static auto window = std::chrono::steady_clock::now();
    Heartbeat("clip ticks", n, window);
  }
  __imp__sub_8247C6C0(ctx, base);
}

REX_HOOK_RAW(sub_8247ECD8) {
  if (Tracing()) {
    static std::atomic<uint32_t> n{0};
    static auto window = std::chrono::steady_clock::now();
    Heartbeat("frame controls", n, window);
  }
  __imp__sub_8247ECD8(ctx, base);
}

REX_HOOK_RAW(sub_8247EE88) {
  if (Tracing()) {
    static std::atomic<uint32_t> n{0};
    static auto window = std::chrono::steady_clock::now();
    Heartbeat("frame actions", n, window);
  }
  __imp__sub_8247EE88(ctx, base);
}

REX_HOOK_RAW(sub_821FD498) {
  if (Tracing()) {
    std::cerr << Stamp() << "apt: screen requested: " << (ctx.r4.u32 & 0xFFu) << '\n';
  }
  __imp__sub_821FD498(ctx, base);
}

// The title's own frame pulse (RwCameraShowRaster, 0x8234B520 - it is what
// main calls to show a frame). When the intro stalls, both the movie component
// and the Flash movie stop being driven; this says whether the game is still
// running frames at all while that happens, which separates "the frontend
// stopped being updated" from "the whole guest loop is blocked".
extern "C" void __imp__sub_8234B520(PPCContext& __restrict, uint8_t*);

REX_HOOK_RAW(sub_8234B520) {
  if (Tracing()) {
    static std::atomic<uint32_t> n{0};
    static auto window = std::chrono::steady_clock::now();
    Heartbeat("game frames", n, window);
  }
  __imp__sub_8234B520(ctx, base);
}
