// --online: take the title through its Xbox Live login to the online menus.
//
// The SDK half (xam_net.cpp, xam_user.cpp, xlivebase_app.cpp) answers as a
// console signed in to Live with a working connection. That is enough for
// EA's DirtySock to report the connection as up ('onln'), but the title then
// logs in to EA's own lobby servers, which are long gone. This half stands in
// for them at the few places the login waits on a server's answer.
//
// The login is CB4XenonLoginMenuState, a state machine in +0x140:
//   0 waiting for the menu movie      5 connecting to Live (NetConnStatus 'onln')
//   1 preparing the network manager   6 connecting to the lobby server
//   2 preparing the player manager    7 logging in to the lobby
//   3 preparing the network lobby     8 downloading the terms of service
//   4 gamertag selection (sign-in)    10 fetching configuration, then
//   11 done, or failed                   CB4PostLoginManager
// Every change of state is logged, so a run shows where the login stops.
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, online);
// Off, the lobby is not stood in for: the title talks to a lobby server (ours,
// tools/ealobby) and hears its real answers. On until that server does the job.
REXCVAR_DEFINE_BOOL(online_fake_lobby, true, "Network",
                    "With --online, stand in for EA's lobby servers inside the title (off: talk to "
                    "a real lobby server)");

extern "C" void __imp__sub_82217590(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82366C90(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82368410(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82227F98(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8220D100(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82229FA8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8222FB00(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82230B70(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8222BFD8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8211F448(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821E9638(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821E95A8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82224DD0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8222CE98(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82589140(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825893B0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82589448(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82589578(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825895B8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82589630(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82589658(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82589708(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8258B610(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822037C0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8240A8F0(PPCContext& __restrict, uint8_t*);

namespace {

bool Online() {
  return REXCVAR_GET(online);
}

// The stand-ins for EA's servers; the Voip guards below hold regardless.
bool Faking() {
  return REXCVAR_GET(online) && REXCVAR_GET(online_fake_lobby);
}

// Where the login has got to. The lobby is only answered for once it exists:
// code that runs from boot asks too, and a lobby that claims to be connected
// before it is prepared sends it into structures that are not there yet.
bool g_lobby_connecting = false;  // login reached state 6: the lobby is prepared
bool g_lobby_logged_in = false;   // login reached state 10: logged in
int g_frames_connecting = 0;

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

const char* LoginStateName(uint32_t state) {
  switch (state) {
    case 0: return "waiting for the menu movie";
    case 1: return "preparing the network manager";
    case 2: return "preparing the player manager";
    case 3: return "preparing the network lobby";
    case 4: return "gamertag selection";
    case 5: return "connecting to Live";
    case 6: return "connecting to the lobby server";
    case 7: return "logging in to the lobby";
    case 8: return "downloading the terms of service";
    case 10: return "fetching configuration";
    case 11: return "done / failed";
    default: return "?";
  }
}

std::string FourCC(uint32_t v) {
  std::string s;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const char c = char((v >> shift) & 0xFF);
    s += (c >= 0x20 && c < 0x7F) ? c : '.';
  }
  return s;
}

}  // namespace

// CB4XenonLoginMenuState::Action - runs the state machine each frame.
REX_HOOK_RAW(sub_82217590) {
  if (!Online()) {
    __imp__sub_82217590(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32;
  const uint32_t before = LoadU32(base, self + 0x140);
  __imp__sub_82217590(ctx, base);
  const uint32_t after = LoadU32(base, self + 0x140);
  if (after != before) {
    REXLOG_INFO("--online: login state {} ({}) -> {} ({})", before, LoginStateName(before), after,
                LoginStateName(after));
    if (after == 6) {
      // Entering state 6: start forcing GetLobbyStatus=2 after a few frames.
      g_lobby_connecting = true;
      g_frames_connecting = 0;
    } else if (after == 7 || after == 8) {
      // States 7 (logging in to lobby) and 8 (downloading ToS) also poll
      // GetLobbyStatus. Keep g_lobby_connecting so the hook keeps forcing 2.
      // Don't reset g_frames_connecting — already past the threshold.
      g_lobby_connecting = true;
    } else {
      g_lobby_connecting = false;
      if (after == 10) {
        g_lobby_logged_in = true;
      }
    }
  }
}

// CGtLobbyDirtySock::GetLobbyStatus (the lobby's vtable slot 3, reached
// directly and through CB4NetworkManager): 0 while busy, 1 on failure, 2 once
// connected to EA's lobby server.
REX_HOOK_RAW(sub_82366C90) {
  const uint32_t lobby = ctx.r3.u32;
  __imp__sub_82366C90(ctx, base);
  if (!Online()) return;

  // Log the lobby's own state whenever it changes (a request in flight (+0xc),
  // busy (+0x10), done (+0x11), failed (+0x12)) and what GetLobbyStatus said.
  const uint32_t pending = LoadU32(base, lobby + 0xC);
  const uint32_t flags = (uint32_t(base[lobby + 0x10]) << 16) |
                         (uint32_t(base[lobby + 0x11]) << 8) | base[lobby + 0x12];
  static uint64_t last_state = ~0ull;
  const uint64_t now = (uint64_t(pending) << 32) ^ (uint64_t(flags) << 8) ^ ctx.r3.u32;
  if (now != last_state) {
    last_state = now;
    REXLOG_INFO("--online: lobby status {} (in-flight {:08X}, busy {} done {} failed {})",
                ctx.r3.u32, pending, flags >> 16, (flags >> 8) & 0xFF, flags & 0xFF);
  }

  if (Faking()) {
    // Stand-in mode: always report connected.
    static uint32_t last_fake = 0xFFFFFFFF;
    if (ctx.r3.u32 != last_fake) {
      last_fake = ctx.r3.u32;
      REXLOG_INFO("--online: fake lobby: status {} -> 2 (connected)", last_fake);
    }
    ctx.r3.u64 = 2;
  } else {
    // Real server mode: force 2 (connected) whenever DirtySock didn't report
    // an actual failure (1). DirtySock may sit "busy" indefinitely while it
    // processes auth tokens; the title's state machine needs to see 2 to
    // advance through states 6, 7, and 8.
    if (ctx.r3.u32 != 1) {
      static bool forced_once = false;
      if (!forced_once) {
        forced_once = true;
        REXLOG_INFO(
            "--online: forcing GetLobbyStatus 2 (was {}, busy={} done={} failed={})",
            ctx.r3.u32, flags >> 16, (flags >> 8) & 0xFF, flags & 0xFF);
      }
      ctx.r3.u64 = 2;
    }
  }
}


// CGtLobbyDirtySock::IsLoggedIntoLobby - it is.
REX_HOOK_RAW(sub_82368410) {
  __imp__sub_82368410(ctx, base);
  if (Online() && g_lobby_logged_in) {
    ctx.r3.u64 = 1;
  }
}

// CB4NetworkManager::Release - the lobby goes away with it, and so does the
// answering for it.
REX_HOOK_RAW(sub_82229FA8) {
  if (Online() && (g_lobby_connecting || g_lobby_logged_in)) {
    REXLOG_INFO("--online: network manager released (type {}, from {:08X}); lobby no longer "
                "answered for",
                ctx.r4.u32, ctx.lr);
  }
  g_lobby_connecting = false;
  g_lobby_logged_in = false;
  __imp__sub_82229FA8(ctx, base);
}

// CB4NetworkRevengeRivals::DownloadRivalsTable - the player's Revenge rivals
// live in EA's Locker storage, on the same servers. Nothing is fetched; the
// table stays empty.
REX_HOOK_RAW(sub_8222FB00) {
  if (Online()) {
    REXLOG_INFO("--online: Revenge rivals table not downloaded (no Locker server); left empty");
    return;
  }
  __imp__sub_8222FB00(ctx, base);
}

// CB4PostLoginManager::Update. State 3 waits for the rivals download above to
// call back, which it never will: answer for it here, once per login, as if the
// download had finished. (Not from DownloadRivalsTable itself - its caller sets
// state 3 after it returns, over whatever the callback set.)
REX_HOOK_RAW(sub_82230B70) {
  const uint32_t self = ctx.r3.u32;
  if (Online() && LoadU32(base, self) == 3) {
    REXLOG_INFO("--online: rivals download reported finished");
    ctx.r3.u64 = 1;
    ctx.r4.u64 = self;
    sub_8222BFD8(ctx, base);  // CB4PostLoginManager::RivalsDownloadedCallback
    ctx.r3.u64 = self;
  }
  __imp__sub_82230B70(ctx, base);
}

// The replay manager's login step (Burnout Clips sharing, also on EA's Locker),
// queued by RivalsDownloadedCallback with ReplayManagerOnLoginCallback, which
// only sets the post-login manager (r6) to state 5. Not queued; state 5 set.
REX_HOOK_RAW(sub_8211F448) {
  if (Online()) {
    REXLOG_INFO("--online: replay manager login skipped (no Locker server)");
    const uint32_t value = __builtin_bswap32(5u);
    std::memcpy(base + ctx.r6.u32, &value, sizeof(value));
    return;
  }
  __imp__sub_8211F448(ctx, base);
}

// CB4NetworkingService::PrepareDownloadNews - the lobby menus fetch EA's news
// ticker over HTTPS from a URL the lobby server handed out at login
// (CGtLobbyDirtySock::GetNewsUrl, which reads the server's login reply and
// faults without one). There is no news: nothing is fetched, and
// UpdateNewsTos stays idle.
REX_HOOK_RAW(sub_821E9638) {
  if (Online()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("--online: no news (nothing downloaded)");
    }
    return;
  }
  __imp__sub_821E9638(ctx, base);
}

// CB4NetworkManager::ConnectionDropped, the callback the lobby calls when its
// connection to EA's server goes. The lobby really is trying to reach the
// service address, where nothing listens, and gives up after 15 s (error
// -12); the title then drops out of its online menus. While the lobby is
// being answered for, the drop is not passed on.
REX_HOOK_RAW(sub_8222CE98) {
  if (Faking() && (g_lobby_connecting || g_lobby_logged_in)) {
    REXLOG_INFO("--online: lobby reported its connection dropped; ignored");
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8222CE98(ctx, base);
}

// CB4NetworkManager::ShowNetworkError(error, ...) - logged with its caller, to
// tell which check gave up on the connection.
REX_HOOK_RAW(sub_82224DD0) {
  if (Online()) {
    REXLOG_INFO("--online: network error {} shown (from {:08X})", int32_t(ctx.r4.u32), ctx.lr);
  }
  __imp__sub_82224DD0(ctx, base);
}

// CB4NetworkingService::PrepareDownloadAgreement - the other half of the same
// ticker: EA's terms of service, also over HTTPS from the lobby server. Left
// undone, the download state points at an HTTP transfer that was never
// created, and ProtoHttpUpdate faults on it. No terms either.
REX_HOOK_RAW(sub_821E95A8) {
  if (Online()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("--online: no terms of service (nothing downloaded)");
    }
    return;
  }
  __imp__sub_821E95A8(ctx, base);
}

// CB4PostLoginManager::UnsuspendCompleted(bool ok, ...) and
// CB4XenonLoginMenuState::PostLoginManagerCallback(bool ok, ...): the two
// verdicts after the login. Logged, to see which step a failure comes from.
REX_HOOK_RAW(sub_82227F98) {
  if (Online()) {
    REXLOG_INFO("--online: suspension manager unsuspended: {}", (ctx.r3.u32 & 0xFF) ? "ok" : "failed");
  }
  __imp__sub_82227F98(ctx, base);
}

REX_HOOK_RAW(sub_8220D100) {
  if (Online()) {
    REXLOG_INFO("--online: post-login finished: {}", (ctx.r3.u32 & 0xFF) ? "ok" : "failed");
  }
  __imp__sub_8220D100(ctx, base);
}

// The lobby login's status (LobbyLoginGetStatus): 6 is logged in. The login
// object talks to a server that no longer exists; it is logged in.
REX_HOOK_RAW(sub_822037C0) {
  __imp__sub_822037C0(ctx, base);
  if (Online()) {
    static uint32_t last = 0xFFFFFFFF;
    if (ctx.r3.u32 != last) {
      last = ctx.r3.u32;
      if (Faking()) {
        REXLOG_INFO("--online: lobby login status {} from DirtySock, answered 6 (logged in)", last);
        ctx.r3.u64 = 6;
      } else {
        REXLOG_INFO("--online: lobby login status {} from DirtySock (real server)", last);
      }
    } else if (Faking()) {
      ctx.r3.u64 = 6;
    }
  }
}

// NetConnStatus(selector, ...): logged whenever what it answers for a selector
// changes, so a run shows DirtySock's view of the connection ('conn', 'onln').
REX_HOOK_RAW(sub_8240A8F0) {
  const uint32_t selector = ctx.r3.u32;
  __imp__sub_8240A8F0(ctx, base);
  if (!Online()) {
    return;
  }
  static uint32_t selectors[16] = {};
  static uint32_t answers[16] = {};
  for (int i = 0; i < 16; ++i) {
    if (selectors[i] == selector || selectors[i] == 0) {
      if (selectors[i] == 0 || answers[i] != ctx.r3.u32) {
        selectors[i] = selector;
        answers[i] = ctx.r3.u32;
        REXLOG_INFO("--online: NetConnStatus('{}') = {:08X} ('{}')", FourCC(selector), ctx.r3.u32,
                    FourCC(ctx.r3.u32));
      }
      break;
    }
  }
}

// Voice chat (DirtySock's Voip over XHV). When the voice module cannot start,
// VoipGetRef is null - and CGtVoIPManagerXenon::Prepare goes on to call
// VoipControl(null, 'port', ...) regardless, which reads 0x68 off null. On a
// console the module always starts, so the title never checks. Every Voip
// call given no module does nothing and returns 0: voice is off, the rest
// carries on. This holds with or without --online; null is never valid here.
namespace {
void VoipWithoutModule(const char* name) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_WARN("{} called without a voice module (VoipStartup failed); voice chat is off", name);
  }
}
}  // namespace

REX_HOOK_RAW(sub_82589140) {  // VoipStartup
  __imp__sub_82589140(ctx, base);
  REXLOG_INFO("VoipStartup -> {:08X}{}", ctx.r3.u32, ctx.r3.u32 ? "" : " (voice module did not start)");
}

REX_HOOK_RAW(sub_8258B610) {  // XHVCreateEngine
  __imp__sub_8258B610(ctx, base);
  REXLOG_INFO("XHVCreateEngine -> {:08X}", ctx.r3.u32);
}

REX_HOOK_RAW(sub_825893B0) {  // VoipSetLocalUser
  if (ctx.r3.u32 == 0) {
    VoipWithoutModule("VoipSetLocalUser");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_825893B0(ctx, base);
}

REX_HOOK_RAW(sub_82589448) {  // VoipConnect2
  if (ctx.r3.u32 == 0) {
    VoipWithoutModule("VoipConnect2");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82589448(ctx, base);
}

REX_HOOK_RAW(sub_82589578) {  // VoipDisconnect
  if (ctx.r3.u32 == 0) {
    VoipWithoutModule("VoipDisconnect");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82589578(ctx, base);
}

REX_HOOK_RAW(sub_825895B8) {  // VoipRemote
  if (ctx.r3.u32 == 0) {
    VoipWithoutModule("VoipRemote");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_825895B8(ctx, base);
}

REX_HOOK_RAW(sub_82589630) {  // VoipLocal
  if (ctx.r3.u32 == 0) {
    VoipWithoutModule("VoipLocal");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82589630(ctx, base);
}

REX_HOOK_RAW(sub_82589658) {  // VoipStatus2
  if (ctx.r3.u32 == 0) {
    VoipWithoutModule("VoipStatus2");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82589658(ctx, base);
}

REX_HOOK_RAW(sub_82589708) {  // VoipControl
  if (ctx.r3.u32 == 0) {
    VoipWithoutModule("VoipControl");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82589708(ctx, base);
}
