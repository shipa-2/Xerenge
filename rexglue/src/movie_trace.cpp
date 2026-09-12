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
#include <iostream>

#include <rex/hook.h>
#include <rex/ppc.h>

namespace {

constexpr uint32_t kAptVideoStatus = 0x82A5900Cu;   // CB4AptManager + 0x574C
constexpr uint32_t kVideoPlayerState = 0x82A53360u;  // CB4VideoManager + 0x98

bool Tracing() {
  static const bool on = std::getenv("XERENGE_MOVIE_TRACE") != nullptr;
  return on;
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

REX_HOOK_RAW(sub_821FEBC0) {
  if (Tracing()) {
    const uint32_t descriptor = ctx.r4.u32;
    const uint32_t name = descriptor != 0 ? LoadGuestU32(base, descriptor) : 0;
    std::cerr << "movie: clip requested: "
              << (name != 0 ? reinterpret_cast<const char*>(base + name) : "(none)") << '\n';
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
    std::cerr << "movie: apt status " << status_before << " -> " << status_after << "  player "
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
      std::cerr << "movie: apt status held at " << status_after << ", player " << player_after
                << " (" << n << " calls with no change)\n";
    }
  }
}

REX_HOOK_RAW(sub_8248D398) {
  if (Tracing()) {
    const uint32_t previous = LoadGuestU32(base, ctx.r3.u32 + 0xD8u);
    if (previous != ctx.r4.u32) {
      std::cerr << "movie: player status " << previous << " -> " << ctx.r4.u32 << '\n';
    }
  }
  __imp__sub_8248D398(ctx, base);
}

REX_HOOK_RAW(sub_82357CC0) {
  __imp__sub_82357CC0(ctx, base);
  if (Tracing()) {
    static std::atomic<uint32_t> last{0xFFFFFFFFu};
    const uint32_t answer = ctx.r3.u32 & 0xFFu;
    if (last.exchange(answer, std::memory_order_relaxed) != answer) {
      std::cerr << "movie: decoder says finished = " << answer << '\n';
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
    std::cerr << "apt: " << what << " x" << n << '\n';
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
    std::cerr << "apt: screen requested: " << (ctx.r4.u32 & 0xFFu) << '\n';
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
