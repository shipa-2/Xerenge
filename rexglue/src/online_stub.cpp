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
#include "ealobby/aries.h"
#include "guest_memory.h"

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
extern "C" void __imp__sub_82408DF8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82370AA0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8240F650(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8240EAD8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_824095D8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8240D398(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8240C908(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82366CF8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82587A88(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825BE2F0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82415E48(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825BEB58(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82203D10(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82230C10(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822216C8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82368400(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82365758(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_823729E8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_823610F8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82371478(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82371300(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82370F60(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8222A508(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82365C40(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82370EA0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8221C6B8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8221C670(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8236C060(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_823648A8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8221C478(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82219488(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82224E78(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82404EF8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822302E8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82588030(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821F3390(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821EEA68(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82368590(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8236FED0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8222EAC8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8222E8F8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82367CF0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822185D8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82220EF8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_823667E0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8259D2A0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8221B010(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8221B1E8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8221A220(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8222A2C8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82372638(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82587570(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8221C328(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_822162D0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82214E88(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82218098(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82230800(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82365D60(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82365698(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8221BE38(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8236D920(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8222EAC8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8236DE80(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825BF2B8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825BF3E8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82366F78(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8236C700(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821E6928(PPCContext& __restrict, uint8_t*);

namespace {

bool Online() {
  return REXCVAR_GET(online);
}

// The stand-ins for EA's lobby server; the Voip guards below hold regardless.
// Locker (rivals, clips) and the news/terms downloads are skipped with any
// --online: no server of ours provides them.
bool Faking() {
  return REXCVAR_GET(online) && REXCVAR_GET(online_fake_lobby);
}

// Where the login has got to. The lobby is only answered for once it exists:
// code that runs from boot asks too, and a lobby that claims to be connected
// before it is prepared sends it into structures that are not there yet.
bool g_lobby_connecting = false;  // login reached state 6: the lobby is prepared
bool g_lobby_logged_in = false;   // login reached state 10: logged in

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  return xerenge::LoadGuestU32(base, address);
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
      g_lobby_connecting = true;
    } else if (after == 10) {
      g_lobby_logged_in = true;
    }
  }
}

// CGtLobbyDirtySock::GetLobbyStatus (the lobby's vtable slot 3, reached
// directly and through CB4NetworkManager): 0 while busy, 1 on failure, 2 once
// connected to EA's lobby server. There is no server, so it is connected.
REX_HOOK_RAW(sub_82366C90) {
  const uint32_t lobby = ctx.r3.u32;
  __imp__sub_82366C90(ctx, base);
  if (Online() && !Faking()) {
    // Talking to a real lobby server: log the lobby's own state whenever it
    // changes - a request in flight (+0xc), busy (+0x10), done (+0x11),
    // failed (+0x12) - and what GetLobbyStatus made of it.
    const uint32_t pending = LoadU32(base, lobby + 0xC);
    const uint32_t flags = (uint32_t(*xerenge::GuestPointer(base, lobby + 0x10)) << 16) |
                           (uint32_t(*xerenge::GuestPointer(base, lobby + 0x11)) << 8) | *xerenge::GuestPointer(base, lobby + 0x12);
    static uint64_t last = ~0ull;
    const uint64_t now = (uint64_t(pending) << 32) ^ (uint64_t(flags) << 8) ^ ctx.r3.u32;
    if (now != last) {
      last = now;
      REXLOG_INFO("--online: lobby status {} (request in flight {:08X}, busy {} done {} failed {})",
                  ctx.r3.u32, pending, flags >> 16, (flags >> 8) & 0xFF, flags & 0xFF);
    }
  }
  if (Faking() && g_lobby_connecting) {
    static uint32_t last = 0xFFFFFFFF;
    if (ctx.r3.u32 != last) {
      last = ctx.r3.u32;
      REXLOG_INFO("--online: lobby status {} from DirtySock, answered 2 (connected)", last);
    }
    ctx.r3.u64 = 2;
  }
}

// CGtLobbyDirtySock::IsLoggedIntoLobby - it is.
REX_HOOK_RAW(sub_82368410) {
  __imp__sub_82368410(ctx, base);
  if (Faking() && g_lobby_logged_in) {
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
    std::memcpy(xerenge::GuestPointer(base, ctx.r6.u32), &value, sizeof(value));
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
  if (Faking()) {
    static uint32_t last = 0xFFFFFFFF;
    if (ctx.r3.u32 != last) {
      last = ctx.r3.u32;
      REXLOG_INFO("--online: lobby login status {} from DirtySock, answered 6 (logged in)", last);
    }
    ctx.r3.u64 = 6;
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

// LobbyApi's login steps (connect, auth, ...) and the step callback
// dispatcher that reports them to the game: sub_82409808 queues a step with
// its callback in the login object (+112 step, +116 callback, +120 context);
// this runs the callback once the step's status (+92) is 3 (done) or 2
// (failed, reason in +96), or the object's step table (+88) has moved on.
// With a real lobby server the last run stopped after 'news' with the connect
// step never reported: log every change of these and of the connection's own
// state (the fourcc at LobbyApi+12: skey, idle, ...) to see which is stuck.
REX_HOOK_RAW(sub_82408DF8) {
  if (Online() && !Faking()) {
    const uint32_t login = ctx.r5.u32;
    if (login) {
      const uint32_t ref = LoadU32(base, login + 0);
      const uint32_t index = LoadU32(base, login + 88);
      const uint32_t expected = index < 64 ? LoadU32(base, login + (index + 6) * 4) : 0;
      const uint32_t status = LoadU32(base, login + 92);
      const uint32_t reason = LoadU32(base, login + 96);
      const uint32_t step = LoadU32(base, login + 112);
      const uint32_t callback = LoadU32(base, login + 116);
      const uint32_t connection = ref ? LoadU32(base, ref + 12) : 0;
      static uint32_t last[7] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
      const uint32_t now[7] = {index, expected, status, reason, step, callback, connection};
      if (std::memcmp(now, last, sizeof(now)) != 0) {
        std::memcpy(last, now, sizeof(now));
        REXLOG_INFO("--online: lobby login step table[{}]={} status {} reason {:08X}, waiting "
                    "step {} callback {:08X}; connection '{}'",
                    index, int32_t(expected), int32_t(status), reason, int32_t(step), callback,
                    FourCC(connection));
      }
    }
  }
  __imp__sub_82408DF8(ctx, base);
}

// CGtLobbyDirtySockXenon::ServerConnectCallback(ref, msg, status, self): on
// status 3 it sends 'sele' and the login goes on; anything else fails it.
REX_HOOK_RAW(sub_82370AA0) {
  if (Online()) {
    REXLOG_INFO("--online: lobby connect callback, status {}", int32_t(ctx.r5.u32));
  }
  __imp__sub_82370AA0(ctx, base);
}

// LobbyApiUpdate(ref): receives and dispatches the lobby's messages, then
// raises event 5 (an idle tick) to its listeners - the login steps among
// them. Logged when the connection's state (ref+12) changes, with how many
// updates ran before it.
REX_HOOK_RAW(sub_8240F650) {
  if (Online() && !Faking()) {
    const uint32_t ref = ctx.r3.u32;
    static uint32_t last_state = ~0u;
    static uint64_t calls = 0;
    ++calls;
    const uint32_t state = LoadU32(base, ref + 12);
    if (state != last_state) {
      last_state = state;
      REXLOG_INFO("--online: LobbyApiUpdate #{}: connection '{}'", calls, FourCC(state));
    }
  }
  __imp__sub_8240F650(ctx, base);
}

// The reply to the lobby's 'news NAME=7' request (ref, msg): keeps the body as
// the configuration when the reply's header code is 'new7', then raises event 3
// ('conn') - the lobby counts as connected from here.
REX_HOOK_RAW(sub_8240EAD8) {
  if (Online()) {
    const uint32_t msg = ctx.r4.u32;
    REXLOG_INFO("--online: lobby configuration reply, code '{}'",
                msg ? FourCC(LoadU32(base, msg + 12)) : std::string("-"));
  }
  __imp__sub_8240EAD8(ctx, base);
}

// The login's listener for event 3 (connection events): 'conn' should move
// the connect step on.
REX_HOOK_RAW(sub_824095D8) {
  if (Online()) {
    const uint32_t event = ctx.r4.u32;
    REXLOG_INFO("--online: lobby connection event {} '{}'",
                event ? int32_t(LoadU32(base, event + 4)) : -1,
                event ? FourCC(LoadU32(base, event + 8)) : std::string("-"));
  }
  __imp__sub_824095D8(ctx, base);
}

// ProtoMangleControl(ref, selector, value, value2, pointer): how the game's
// net layer tells DirtySock's session code about a game - 'host' (the host's
// XUID, 8 bytes at ref+0x70), 'suid', 'sess' (create the Xbox Live session)
// and more. Logged per selector, with the XUID where it is one.
REX_HOOK_RAW(sub_8240D398) {
  if (Online()) {
    const uint32_t selector = ctx.r4.u32;
    const uint32_t pointer = ctx.r7.u32;
    std::string name;
    for (int shift = 24; shift >= 0; shift -= 8) {
      const char c = char((selector >> shift) & 0xFF);
      name += (c >= 32 && c < 127) ? c : '.';
    }
    // 'host', 'self' and 'suid' take an XUID as "$" and hex; its text, as given.
    std::string text;
    if ((selector == 0x686F7374u || selector == 0x73656C66u || selector == 0x73756964u) &&
        pointer != 0) {
      for (uint32_t i = 0; i < 24; ++i) {
        const uint8_t c = *xerenge::GuestPointer(base, pointer + i);
        if (c == 0) {
          break;
        }
        text += (c >= 32 && c < 127) ? char(c) : '?';
      }
    }
    REXLOG_INFO("--online: ProtoMangleControl '{}' value {} value2 {} pointer {:08X}{}", name,
                int32_t(ctx.r5.u32), int32_t(ctx.r6.u32), pointer,
                text.empty() ? std::string() : " \"" + text + "\"");
  }
  __imp__sub_8240D398(ctx, base);
}

// ConnApi update (0x82587A88): logs when its state (+0x15c), the mangle state
// under it (+0x38 -> +0x6c), the client count (+0x160) or +0xa8 change.
REX_HOOK_RAW(sub_82587A88) {
  if (Online()) {
    static uint32_t last[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    const uint32_t api = ctx.r3.u32;
    const uint32_t mangle = xerenge::LoadGuestU32(base, api + 0x38);
    const uint32_t now[6] = {xerenge::LoadGuestU32(base, api + 0x15C),
                             mangle ? xerenge::LoadGuestU32(base, mangle + 0x6C) : ~0u,
                             xerenge::LoadGuestU32(base, api + 0x160),
                             xerenge::LoadGuestU32(base, api + 0xA8),
                             xerenge::LoadGuestU32(base, api + 0x1F0),
                             xerenge::LoadGuestU32(base, api + 0x2A0)};
    if (std::memcmp(now, last, sizeof(now)) != 0) {
      std::memcpy(last, now, sizeof(now));
      REXLOG_INFO("--online: ConnApi {:08X} state {} mangle state {} clients {} +a8 {} client0 {} client1 {} callback {:08X} user {:08X}", api,
                  int32_t(now[0]), int32_t(now[1]), int32_t(now[2]), int32_t(now[3]),
                  int32_t(now[4]), int32_t(now[5]), xerenge::LoadGuestU32(base, api), xerenge::LoadGuestU32(base, api + 4));
    }
  }
  __imp__sub_82587A88(ctx, base);
}

// CommUDP (the game's peer link): its idle callback, run by the socket layer,
// and Connect/Listen with the address string they are given.
REX_HOOK_RAW(sub_82230C10) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t before = Online() ? *xerenge::GuestPointer(base, self + 0xC) : 0;
  __imp__sub_82230C10(ctx, base);
  if (Online()) {
    {
      static std::string lastNat;
      const uint32_t nm = 0x82A66880;
      const int32_t count = int32_t(xerenge::LoadGuestU32(base, nm + 0x38));
      std::string s = "count " + std::to_string(count);
      for (int i = 0; i < count && i < 8; ++i) {
        PPCContext c = ctx;
        c.r3.u32 = nm + 0x18;
        c.r4.u32 = uint32_t(i);
        __imp__sub_82365758(c, base);
        const uint32_t p = c.r3.u32;
        s += " p" + std::to_string(i) + "=" + (p ? std::to_string(int32_t(xerenge::LoadGuestU32(base, p + 0x4B4))) : std::string("null"));
      }
      if (s != lastNat) {
        REXLOG_INFO("--online: NAT data: {}", s);
        lastNat = s;
      }
    }
    static uint32_t lastState = 0xFFFF, lastRet = 0xFFFF;
    const uint32_t after = *xerenge::GuestPointer(base, self + 0xC);
    if (before != lastState || ctx.r3.u32 != lastRet) {
      REXLOG_INFO("--online: PreLaunch update: state {} -> {}, returned {}", before, after, ctx.r3.u32);
      lastState = before;
      lastRet = ctx.r3.u32;
    }
  }
}

REX_HOOK_RAW(sub_822216C8) {
  __imp__sub_822216C8(ctx, base);
  if (Online()) {
    static int lastIdle = -1;
    const int now = ctx.r3.u32 & 0xFF;
    if (now != lastIdle) {
      REXLOG_INFO("--online: PreLaunch LobbyIdle -> {}", now);
      lastIdle = now;
    }
  }
}

REX_HOOK_RAW(sub_82368400) {
  __imp__sub_82368400(ctx, base);
  if (Online()) {
    static int lastFlag = -1;
    const int now = ctx.r3.u32 & 0xFF;
    if (now != lastFlag) {
      REXLOG_INFO("--online: lobby flag d09 -> {}", now);
      lastFlag = now;
    }
  }
}

REX_HOOK_RAW(sub_8236D920) {
  __imp__sub_8236D920(ctx, base);
  if (Online()) REXLOG_INFO("--online: DirtySock JoinGame returned {}", int32_t(ctx.r3.u32));
}

REX_HOOK_RAW(sub_8222EAC8) {
  if (Online()) REXLOG_INFO("--online: lobby event callback ok={} r4={:08X} event={} r6={:08X}", ctx.r3.u32 & 0xFF, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  if (Online()) REXLOG_INFO("--online: registered UI callback {:08X} ctx {:08X}", xerenge::LoadGuestU32(base, ctx.r4.u32 + 0x3CB20), xerenge::LoadGuestU32(base, ctx.r4.u32 + 0x3CB24));
  __imp__sub_8222EAC8(ctx, base);
}

REX_HOOK_RAW(sub_8221BE38) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8221BE38(ctx, base);
  if (Online()) {
    static int last = -99;
    const int now = int32_t(ctx.r3.u32);
    if (now != last) {
      REXLOG_INFO("--online: lobby status -> {} (from {:08X})", now, lr);
      last = now;
    }
  }
}

REX_HOOK_RAW(sub_82230800) {
  const uint32_t mm = ctx.r3.u32;
  const uint32_t before = *xerenge::GuestPointer(base, mm + 0x6C);
  __imp__sub_82230800(ctx, base);
  if (Online()) {
    static uint32_t lastB = 999, lastR = 999;
    const uint32_t ret = ctx.r3.u32;
    if (before != lastB || ret != lastR) {
      REXLOG_INFO("--online: MatchMaking update: state {} -> {}, returned {}, flag185f {}", before, *xerenge::GuestPointer(base, mm + 0x6C), ret, *xerenge::GuestPointer(base, 0x82A66880 + 0x185F));
      lastB = before; lastR = ret;
    }
  }
}

REX_HOOK_RAW(sub_82365D60) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82365D60(ctx, base);
  if (Online() && lr == 0x82230990) {
    static int last = -1; const int now = ctx.r3.u32 & 0xFF;
    if (now != last) { REXLOG_INFO("--online: MatchMaking check 82365D60 -> {}", now); last = now; }
  }
}

REX_HOOK_RAW(sub_82365698) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82365698(ctx, base);
  if (Online() && lr == 0x822309A4) {
    static int last = -99; const int now = int32_t(ctx.r3.u32);
    if (now != last) { REXLOG_INFO("--online: MatchMaking check 82365698 -> {}", now); last = now; }
  }
}

REX_HOOK_RAW(sub_82218098) {
  if (Online()) {
    const uint32_t pop = xerenge::LoadGuestU32(base, 0x82A59E98 + 4);
    const uint32_t hi = pop ? xerenge::LoadGuestU32(base, pop + 8) : 0;
    const uint32_t lo = pop ? xerenge::LoadGuestU32(base, pop + 12) : 0;
    REXLOG_INFO("--online: OnMatchmakingFinished ok={} state {:08X} popup {:08X} id {:08X}{:08X} (want 94413E560E367D57)", ctx.r3.u32 & 0xFF, ctx.r4.u32, pop, hi, lo);
  }
  __imp__sub_82218098(ctx, base);
}

REX_HOOK_RAW(sub_822162D0) {
  if (Online()) {
    const uint32_t s = ctx.r3.u32;
    REXLOG_INFO("--online: GameState dispatch: 395={} 396={} 398={} 39d={} 39e={} from {:08X}", *xerenge::GuestPointer(base, s + 0x395), *xerenge::GuestPointer(base, s + 0x396), *xerenge::GuestPointer(base, s + 0x398), *xerenge::GuestPointer(base, s + 0x39D), *xerenge::GuestPointer(base, s + 0x39E), uint32_t(ctx.lr));
  }
  __imp__sub_822162D0(ctx, base);
}

REX_HOOK_RAW(sub_82214E88) {
  if (Online()) REXLOG_INFO("--online: ShowInGameScreen from {:08X}", uint32_t(ctx.lr));
  __imp__sub_82214E88(ctx, base);
}

REX_HOOK_RAW(sub_8221C328) {
  static bool once = false;
  if (!once && Online()) {
    once = true;
    const uint32_t vt = xerenge::LoadGuestU32(base, ctx.r3.u32 + 0xB50);
    REXLOG_INFO("--online: DirtySock vtable {:08X} slot fc {:08X} slot 10c {:08X}", vt, xerenge::LoadGuestU32(base, vt + 0xFC), xerenge::LoadGuestU32(base, vt + 0x10C));
  }
  const uint32_t self = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8221C328(ctx, base);
  if (Online()) {
    static uint32_t last = 999;
    const uint32_t now = ctx.r3.u32 & 0xFF;
    if (now != last) {
      REXLOG_INFO("--online: 8221C328 -> {} (cb01 {} cb02 {} pm70 {} pm68 {}) from {:08X}", now, *xerenge::GuestPointer(base, self + 0x3CB01), *xerenge::GuestPointer(base, self + 0x3CB02), xerenge::LoadGuestU32(base, self + 0x18 + 0x70), xerenge::LoadGuestU32(base, self + 0x18 + 0x68), lr);
      last = now;
    }
  }
}

REX_HOOK_RAW(sub_82203D10) {
  if (Online()) REXLOG_WARN("--online: game leaves the session (UI leave), from {:08X}", uint32_t(ctx.lr));
  __imp__sub_82203D10(ctx, base);
}

REX_HOOK_RAW(sub_8236DE80) {
  if (Online()) REXLOG_WARN("--online: LeaveGame, from {:08X}", uint32_t(ctx.lr));
  __imp__sub_8236DE80(ctx, base);
}

REX_HOOK_RAW(sub_825BEB58) {
  static int calls = 0;
  if (Online() && (++calls <= 5 || calls % 20 == 0)) {
    REXLOG_INFO("--online: CommUDP idle callback, call {} skipped {} lock owner {:08X} count {} flag {}", calls, xerenge::LoadGuestU32(base, 0x82D3E268), xerenge::LoadGuestU32(base, 0x82D3E6C0), xerenge::LoadGuestU32(base, 0x82D3E6C4), xerenge::LoadGuestU32(base, 0x82D3E6C8));
  }
  __imp__sub_825BEB58(ctx, base);
}

static std::string GuestText(uint8_t* base, uint32_t address) {
  std::string s;
  for (uint32_t i = 0; i < 64 && address; ++i) {
    const uint8_t c = *xerenge::GuestPointer(base, address + i);
    if (c == 0) {
      break;
    }
    s += (c >= 32 && c < 127) ? char(c) : '?';
  }
  return s;
}

REX_HOOK_RAW(sub_825BE2F0) {
  static int calls = 0;
  if (Online() && (++calls <= 3 || calls % 20 == 0)) {
    const uint32_t self = xerenge::LoadGuestU32(base, 0x82D3E6E8);
    if (self < 0x10000000) { __imp__sub_825BE2F0(ctx, base); return; }
    REXLOG_INFO("--online: CommUDP step {} tick {} obj {:08X} state {} socket {:08X} lastsend {} lasterr {}", calls, ctx.r3.u32, self,
                xerenge::LoadGuestU32(base, self + 0x94), xerenge::LoadGuestU32(base, self + 0x80),
                xerenge::LoadGuestU32(base, self + 0xE4), int32_t(xerenge::LoadGuestU32(base, self + 0xE0)));
  }
  __imp__sub_825BE2F0(ctx, base);
}

REX_HOOK_RAW(sub_825BF2B8) {
  const std::string text = GuestText(base, ctx.r4.u32);
  __imp__sub_825BF2B8(ctx, base);
  if (Online()) {
    REXLOG_INFO("--online: CommUDP connect \"{}\" -> {}", text, int32_t(ctx.r3.u32));
  }
}

// The socket layer redirects every destination to the lobby host (a 0/0 entry in the game's
// address table). Traffic to the peers' own ports has to reach the peer itself.
REX_HOOK_RAW(sub_82415E48) {
  if (Online()) {
    const uint8_t* to = xerenge::GuestPointer(static_cast<const uint8_t*>(base), ctx.r4.u32);
    const uint32_t port = (uint32_t(to[2]) << 8) | to[3];
    if (port == 8192 || port == 6000) {
      ctx.r3.u32 = ctx.r4.u32;
      return;
    }
  }
  __imp__sub_82415E48(ctx, base);
}

REX_HOOK_RAW(sub_825BF3E8) {
  const std::string text = GuestText(base, ctx.r4.u32);
  const uint32_t self = ctx.r3.u32;
  __imp__sub_825BF3E8(ctx, base);
  if (Online()) {
    uint32_t table = xerenge::LoadGuestU32(base, 0x82D39330);
    for (int i = 0; i < 6 && table >= 0x10000000; ++i, table += 12) {
      REXLOG_INFO("--online: address remap [{}] net {:08X} mask {:08X} to {:08X}", i, xerenge::LoadGuestU32(base, table), xerenge::LoadGuestU32(base, table + 4), xerenge::LoadGuestU32(base, table + 8));
      if (!xerenge::LoadGuestU32(base, table + 8)) break;
    }
  }
  if (Online()) {
    REXLOG_INFO("--online: CommUDP listen \"{}\" remote {:08X} {:08X} {:08X} {:08X}", text, xerenge::LoadGuestU32(base, self + 0x84), xerenge::LoadGuestU32(base, self + 0x88), xerenge::LoadGuestU32(base, self + 0x8C), xerenge::LoadGuestU32(base, self + 0x90));
  }
}

// The session creation behind 'sess' (0x8240C908): the host when the host's
// XUID (ref+0x70) is this console's (ref+0x80), otherwise a joiner that copies
// the host's session from the pointer. A joiner handed no pointer read address
// 0 and brought the game down; it now says why and refuses instead.
REX_HOOK_RAW(sub_8240C908) {
  if (Online()) {
    const uint32_t ref = ctx.r3.u32;
    const uint64_t host = xerenge::LoadGuestU64(base, ref + 0x70);
    const uint64_t self = xerenge::LoadGuestU64(base, ref + 0x80);
    REXLOG_INFO("--online: session create - host XUID {:016X}, ours {:016X}, value {}, "
                "session pointer {:08X}",
                host, self, int32_t(ctx.r4.u32), ctx.r5.u32);
    if (host != self && ctx.r5.u32 == 0) {
      REXLOG_WARN("--online: joining a session with no session given - refused");
      ctx.r3.u64 = uint64_t(-1);
      return;
    }
  }
  __imp__sub_8240C908(ctx, base);
}

// CGtLobbyDirtySock::CheckForPlayerKicked (0x82366CF8): once in a game, the
// title looks for its own name (lobby+0x1098) among the game's players (OPPO%d,
// lobby+0x238 on, 0x8C apart, COUNT at lobby+0x22C) and leaves the game - "the
// game you were in no longer exists" - when it is not there. Logged when it
// leaves, with both sides of the comparison.
REX_HOOK_RAW(sub_82366CF8) {
  const uint32_t lobby = ctx.r3.u32;
  __imp__sub_82366CF8(ctx, base);
  if (Online() && ctx.r3.u32 == 1) {
    const auto text = [&](uint32_t address, uint32_t limit) {
      std::string s;
      for (uint32_t i = 0; i < limit; ++i) {
        const uint8_t c = *xerenge::GuestPointer(base, address + i);
        if (c == 0) {
          break;
        }
        s += (c >= 32 && c < 127) ? char(c) : '?';
      }
      return s;
    };
    const uint32_t count = xerenge::LoadGuestU32(base, lobby + 0x22C);
    std::string players;
    for (uint32_t i = 0; i < count && i < 8; ++i) {
      players += " \"" + text(lobby + 0x238 + i * 0x8C, 16) + "\"";
    }
    REXLOG_WARN("--online: left the game as kicked - own name \"{}\", players ({}):{}",
                text(lobby + 0x1098, 32), count, players);
  }
}

namespace {
// The lobby object's game flags (lobby+0xD08..0xD11) and its game record's
// IDENT, NAME and HOST (lobby+0x20, +0x24, +0x58), for the two hooks below.
std::string LobbyGameState(const uint8_t* base, uint32_t lobby) {
  const auto flag = [&](uint32_t offset) { return int(*xerenge::GuestPointer(base, lobby + offset)); };
  const auto text = [&](uint32_t address, uint32_t limit) {
    std::string s;
    for (uint32_t i = 0; i < limit; ++i) {
      const uint8_t c = *xerenge::GuestPointer(base, address + i);
      if (c == 0) {
        break;
      }
      s += (c >= 32 && c < 127) ? char(c) : '?';
    }
    return s;
  };
  return fmt::format("flags d08={} d09={} d0a={} d0b={} d0c={} d0f={} d11={}; record IDENT {} "
                     "NAME \"{}\" HOST \"{}\"; no-host count {}",
                     flag(0xD08), flag(0xD09), flag(0xD0A), flag(0xD0B), flag(0xD0C), flag(0xD0F),
                     flag(0xD11), int32_t(xerenge::LoadGuestU32(base, lobby + 0x20)),
                     text(lobby + 0x24, 36), text(lobby + 0x58, 16),
                     xerenge::LoadGuestU32(base, lobby + 0xD20));
}
}  // namespace

// CGtLobbyDirtySock::CheckRoomHasAHost (0x82366F78): while its game's record
// names no HOST it counts, and at 1000 leaves - "the game you were in no
// longer exists". Logged about once a second while it counts.
REX_HOOK_RAW(sub_82366F78) {
  const uint32_t lobby = ctx.r3.u32;
  if (Online() && *xerenge::GuestPointer(base, lobby + 0x58) == 0) {
    const uint32_t count = xerenge::LoadGuestU32(base, lobby + 0xD20);
    if (count % 60 == 0 || count >= 999) {
      REXLOG_WARN("--online: game has no host yet - {}", LobbyGameState(base, lobby));
    }
  }
  __imp__sub_82366F78(ctx, base);
}

// CGtLobbyDirtySock's LobbyApi event callback (0x8236C700): 'game' (+mgm, its
// own game's record), 'play' (+ses), 'uset'/'user'. Logged with the state it
// finds, before and after.
REX_HOOK_RAW(sub_8236C700) {
  const uint32_t lobby = ctx.r5.u32;
  const uint32_t event = xerenge::LoadGuestU32(base, ctx.r4.u32 + 8);
  std::string name;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const char c = char((event >> shift) & 0xFF);
    name += (c >= 32 && c < 127) ? c : '.';
  }
  const bool log = Online() && (event == 0x67616D65u || event == 0x706C6179u);
  if (log) {
    REXLOG_INFO("--online: lobby event '{}' - before: {}", name, LobbyGameState(base, lobby));
  }
  __imp__sub_8236C700(ctx, base);
  if (log) {
    REXLOG_INFO("--online: lobby event '{}' - after: {}", name, LobbyGameState(base, lobby));
    const uint32_t n = xerenge::LoadGuestU32(base, lobby + 0x22C);
    for (uint32_t i = 0; i < n && i < 8; ++i) {
      const uint32_t e = lobby + 0x234 + i * 0x8C;
      auto str = [&](uint32_t o) { std::string s; for (int k = 0; k < 32; ++k) { char c = char(*xerenge::GuestPointer(base, e + o + k)); if (!c) break; s += c; } return s; };
      REXLOG_INFO("--online: lobby cached player {} name='{}' params='{}'", i, str(4), str(0x68));
    }
  }
}

// The front end's popup (0x821E6928; r5 the text's key, "$Something"): every
// message box shown under --online is logged with its key and its caller, so a
// message on screen can be traced to the code that raised it.
REX_HOOK_RAW(sub_821E6928) {
  if (Online() && ctx.r5.u32 >= 0x80000000u) {
    std::string key;
    for (uint32_t i = 0; i < 64; ++i) {
      const uint8_t c = *xerenge::GuestPointer(base, ctx.r5.u32 + i);
      if (c == 0) {
        break;
      }
      key += (c >= 32 && c < 127) ? char(c) : '?';
    }
    REXLOG_WARN("--online: popup \"{}\" (from {:08X})", key, uint32_t(ctx.lr));
    if (key.rfind("$OnlineMessageNewsUpdated", 0) == 0) {
      return;
    }
  }
  __imp__sub_821E6928(ctx, base);
}

REX_HOOK_RAW(sub_82372638) {
  if (Online()) {
    const uint32_t self = ctx.r3.u32;
    const uint32_t f1b5 = *xerenge::GuestPointer(base, self + 0x1b5);
    const uint32_t connApi = xerenge::LoadGuestU32(base, self + 0x1c0);
    uint32_t list = 0, n = 0;
    if (connApi) {
      PPCContext c = ctx;
      c.r3.u32 = connApi;
      __imp__sub_82587570(c, base);
      list = c.r3.u32;
      if (list) n = xerenge::LoadGuestU32(base, list);
    }
    { const uint32_t vt = xerenge::LoadGuestU32(base, self); static bool once = false; if (!once) { once = true; REXLOG_INFO("--online: NATMgr vtable {:08X} slot0 {:08X}", vt, xerenge::LoadGuestU32(base, vt)); } }
    std::string recs; for (int k = 0; k < 6; ++k) { const uint32_t r = self + 0x34 + k * 0x38; const int32_t rid = int32_t(xerenge::LoadGuestU32(base, r + 4)); if (rid != -1) recs += " rec" + std::to_string(k) + "{id=" + std::to_string(rid) + " st=" + std::to_string(int32_t(xerenge::LoadGuestU32(base, r + 0x20))) + " f19=" + std::to_string(*xerenge::GuestPointer(base, r + 0x19)) + " f1a=" + std::to_string(*xerenge::GuestPointer(base, r + 0x1a)) + "}"; }
    std::string s = "cb c=" + std::to_string(xerenge::LoadGuestU32(base, self + 0xc)) + " 10=" + std::to_string(xerenge::LoadGuestU32(base, self + 0x10)) + " 14=" + std::to_string(xerenge::LoadGuestU32(base, self + 0x14)) + " b5=" + std::to_string(f1b5) + " connapi=" + std::to_string(connApi) + " list=" + std::to_string(list) + " n=" + std::to_string(n);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
      s += " st" + std::to_string(i) + "=" + std::to_string(xerenge::LoadGuestU32(base, list + 0x90 + i * 0xb0)); { PPCContext c = ctx; c.r3.u32 = xerenge::LoadGuestU32(base, self + 4); c.r4.u32 = list + 0x50 + i * 0xb0; __imp__sub_82365C40(c, base); s += " lk=" + std::to_string(c.r3.u32) + " pm=" + std::to_string(xerenge::LoadGuestU32(base, self + 4)); } }
    s += recs;
    static std::string last;
    if (s != last) {
      REXLOG_INFO("--online: NATDataManager update: {} (self {:08X})", s, self);
      last = s;
    }
  }
  __imp__sub_82372638(ctx, base);
}

REX_HOOK_RAW(sub_8222A2C8) {
  if (Online()) {
    ctx.r3.u32 = ealobby::PlayerId(GuestText(base, ctx.r4.u32));
    REXLOG_INFO("--online: ForEachNetworkOpponent id={} name='{}' local={} r7={:08X} r8={} lr={:08X}", int32_t(ctx.r3.u32),
                GuestText(base, ctx.r4.u32), ctx.r6.u32 & 0xFF, ctx.r7.u32, ctx.r8.u32, uint32_t(ctx.lr));
  }
  __imp__sub_8222A2C8(ctx, base);
}

REX_HOOK_RAW(sub_8221C478) {
  if (Online()) REXLOG_INFO("--online: PlayerNatCompleteCallback ok={} r4={} r5={} r6={} lr={:08X}", ctx.r3.u32 & 0xFF, int32_t(ctx.r4.u32), int32_t(ctx.r5.u32), int32_t(ctx.r6.u32), uint32_t(ctx.lr));
  __imp__sub_8221C478(ctx, base);
}

REX_HOOK_RAW(sub_823648A8) {
  if (Online()) REXLOG_INFO("--online: sub_823648A8 r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
  __imp__sub_823648A8(ctx, base);
}

REX_HOOK_RAW(sub_8236C060) {
  if (Online()) REXLOG_INFO("--online: sub_8236C060 r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
  __imp__sub_8236C060(ctx, base);
}

REX_HOOK_RAW(sub_8221C670) {
  if (Online()) REXLOG_INFO("--online: sub_8221C670 r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
  __imp__sub_8221C670(ctx, base);
}

REX_HOOK_RAW(sub_8221C6B8) {
  if (Online()) REXLOG_INFO("--online: sub_8221C6B8 r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
  __imp__sub_8221C6B8(ctx, base);
}

REX_HOOK_RAW(sub_82370EA0) {
  if (Online()) REXLOG_INFO("--online: sub_82370EA0 r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
  __imp__sub_82370EA0(ctx, base);
}

REX_HOOK_RAW(sub_8222A508) {
  if (Online()) REXLOG_INFO("--online: AddPlayerCallback ok={} r4={} r5={} r6={} lr={:08X}", ctx.r3.u32 & 0xFF, int32_t(ctx.r4.u32), int32_t(ctx.r5.u32), int32_t(ctx.r6.u32), uint32_t(ctx.lr));
  __imp__sub_8222A508(ctx, base);
}

REX_HOOK_RAW(sub_82370F60) {
  if (Online()) {
    const uint32_t p = ctx.r3.u32;
    const uint32_t mgr = xerenge::LoadGuestU32(base, p + 0xC);
    std::string s = "b1=" + std::to_string(*xerenge::GuestPointer(base, p + 0x4b1)) + " cnt=" + std::to_string(int32_t(xerenge::LoadGuestU32(base, p + 0x4ac))) +
                    " id=" + std::to_string(xerenge::LoadGuestU32(base, p + 8)) + " mgr0=" + std::to_string(xerenge::LoadGuestU32(base, mgr)) +
                    " m4=" + std::to_string(*xerenge::GuestPointer(base, mgr + 4)) + " m5=" + std::to_string(*xerenge::GuestPointer(base, mgr + 5));
    static std::string last;
    if (s != last) { REXLOG_INFO("--online: Player SendMessages {:08X}: {}", p, s); last = s; }
  }
  __imp__sub_82370F60(ctx, base);
}

REX_HOOK_RAW(sub_82371300) {
  if (Online()) REXLOG_INFO("--online: Player ReceiveMessage player={:08X} msg={:08X} r5={} r6={}", ctx.r3.u32, ctx.r4.u32, int32_t(ctx.r5.u32), int32_t(ctx.r6.u32));
  __imp__sub_82371300(ctx, base);
}

REX_HOOK_RAW(sub_82371478) {
  if (Online()) {
    const uint32_t m = ctx.r3.u32;
    std::string s = "m0=" + std::to_string(xerenge::LoadGuestU32(base, m)) + " m4=" + std::to_string(*xerenge::GuestPointer(base, m + 4)) + " m5=" + std::to_string(*xerenge::GuestPointer(base, m + 5)) + " n=" + std::to_string(int32_t(xerenge::LoadGuestU32(base, m + 0x20)));
    static std::string last;
    if (s != last) { REXLOG_INFO("--online: Manager SendMessages {:08X}: {}", m, s); last = s; }
  }
  __imp__sub_82371478(ctx, base);
}

REX_HOOK_RAW(sub_823729E8) {
  if (Online()) {
    const uint32_t m = ctx.r3.u32;
    std::string s = "m6c=" + std::to_string(xerenge::LoadGuestU32(base, m + 0x6c)) + " m4=" + std::to_string(*xerenge::GuestPointer(base, m + 4)) + " m0=" + std::to_string(xerenge::LoadGuestU32(base, m));
    static std::string last;
    if (s != last) { REXLOG_INFO("--online: ReceiveMessages {:08X}: {}", m, s); last = s; }
  }
  __imp__sub_823729E8(ctx, base);
}

REX_HOOK_RAW(sub_823610F8) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_823610F8(ctx, base);
  if (Online()) {
    static uint32_t calls = 0, lastRet = 0xFFFF;
    ++calls;
    if (ctx.r3.u32 != lastRet || calls == 1) {
      REXLOG_INFO("--online: Adapter Recv {:08X} sock={:08X} -> {} (call {})", self, xerenge::LoadGuestU32(base, self + 0xd8), int32_t(ctx.r3.u32), calls);
      lastRet = ctx.r3.u32;
    }
  }
}

static std::string NatMatrix(uint8_t* base, uint32_t p) {
  std::string s = fmt::format("p={:08X} 4b4={} ids/st:", p, xerenge::LoadGuestU32(base, p + 0x4B4));
  for (int i = 0; i < 6; i++)
    s += fmt::format(" {:08X}/{}", xerenge::LoadGuestU32(base, p + 0x518 + i * 4), xerenge::LoadGuestU32(base, p + 0x530 + i * 4));
  return s;
}

REX_HOOK_RAW(sub_8221B010) {
  static int n = 0;
  uint32_t p = ctx.r3.u32;
  __imp__sub_8221B010(ctx, base);
  if (Online() && (n++ < 6 || n % 500 == 0))
    REXLOG_INFO("--online: SendDetailsOfNATFinalisedStatus #{} {} own4d0..:{:08X}/{}", n, NatMatrix(base, p),
                xerenge::LoadGuestU32(base, p + 0x4CC), xerenge::LoadGuestU32(base, p + 0x4E4));
}

REX_HOOK_RAW(sub_8221A220) {
  if (Online()) REXLOG_INFO("--online: NAT matrix message prepared r3={:08X} lr={:08X}", ctx.r3.u32, uint32_t(ctx.lr));
  __imp__sub_8221A220(ctx, base);
}

REX_HOOK_RAW(sub_8221B1E8) {
  static std::map<uint32_t, std::string> last;
  uint32_t p = ctx.r3.u32;
  __imp__sub_8221B1E8(ctx, base);
  if (Online()) {
    std::string s = fmt::format("res={} {} localid={:08X}", ctx.r3.u32 & 0xFF, NatMatrix(base, p),
                                xerenge::LoadGuestU32(base, xerenge::LoadGuestU32(base, p + 0xC) + 0x68));
    if (last[p] != s) { last[p] = s; REXLOG_INFO("--online: PlayerNATFinalised {}", s); }
  }
}

REX_HOOK_RAW(sub_82219488) {
  static std::string last;
  uint32_t t = ctx.r3.u32, ev = ctx.r4.u32;
  if (Online()) {
    auto b = [&](uint32_t o) { return uint32_t(*xerenge::GuestPointer(base, t + o)); };
    std::string s = fmt::format("3a5={} 3a6={} 395={} 396={} 397={} 398={} 39a={} 39d={} 39e={} scr={:X} f218={} cnt={}", b(0x3A5), b(0x3A6), b(0x395), b(0x396), b(0x397), b(0x398), b(0x39A), b(0x39D), b(0x39E), xerenge::LoadGuestU32(base, 0x82A528B0 + 0x2FC), uint32_t(*xerenge::GuestPointer(base, 0x82A528B0 + 0x218)), xerenge::LoadGuestU32(base, 0x82A66880 + 0x38));
    if (s != last) { last = s; REXLOG_INFO("--online: CustomGameState dispatch ev={} this={:08X} {}", ev, t, s); }
  }
  // A stale popup flag would make the title drop 3a6 before the screen reaches 0x5B, so ConnectingToPlayers never opens.
  uint8_t* popupFlag = xerenge::GuestPointer(base, 0x82A528B0 + 0x218);
  uint8_t savedFlag = *popupFlag;
  const bool mask = Online() && ev == 4 && *xerenge::GuestPointer(base, ctx.r3.u32 + 0x3A6) && xerenge::LoadGuestU32(base, 0x82A528B0 + 0x2FC) != 0x5B;
  if (mask) *popupFlag = 0;
  __imp__sub_82219488(ctx, base);
  if (mask && savedFlag && !*popupFlag) *popupFlag = savedFlag;
}

REX_HOOK_RAW(sub_82224E78) {
  static std::map<uint32_t, std::string> last;
  uint32_t idx = ctx.r4.u32, md = ctx.r5.u32, lr = ctx.lr;
  __imp__sub_82224E78(ctx, base);
  if (Online()) {
    auto b = [&](uint32_t o) { return uint32_t(*xerenge::GuestPointer(base, md + o)); };
    std::string s = fmt::format("status={} 39={} 3b={} 3c={} lr={:08X}", b(0x38), b(0x39), b(0x3B), b(0x3C), lr);
    if (true) { last[idx] = s; REXLOG_INFO("--online: SetMenuData idx={:08X} {}", idx, s); }
  }
}

// TagFieldGetStructure(string, dest, size, format): logs how a lobby player-params string lands in the game's struct.
REX_HOOK_RAW(sub_82404EF8) {
  const uint32_t str = ctx.r3.u32, dest = ctx.r4.u32, size = ctx.r5.u32, fmt = ctx.r6.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82404EF8(ctx, base);
  if (Online() && lr == 0x823679DC && size <= 0x40 && dest && str && fmt && str < 0xA0000000 && fmt < 0xA0000000 && dest < 0xA0000000) {
    std::string s(reinterpret_cast<const char*>(xerenge::GuestPointer(base, str)), 40);
    s.resize(strnlen(s.c_str(), 40));
    std::string f(reinterpret_cast<const char*>(xerenge::GuestPointer(base, fmt)), 16);
    f.resize(strnlen(f.c_str(), 16));
    std::string bytes;
    for (uint32_t i = 0; i < size; ++i) bytes += fmt::format("{:02X} ", *xerenge::GuestPointer(base, dest + i));
    REXLOG_INFO("--online: TagFieldGetStructure '{}' fmt '{}' size {} -> {} ret {}", s, f, size, bytes, ctx.r3.u32);
  }
}

// A NULL source (the ranking table has no name for the lobby) crashed strncpy in CB4NetworkManager::Update; treat it as "".
REX_HOOK_RAW(sub_8259D2A0) {
  if (ctx.r4.u32 == 0) {
    if (Online()) {
      const uint32_t rk = xerenge::LoadGuestU32(base, 0x82A66880 + 0x1d30);
      REXLOG_INFO("--online: strncpy from NULL, lr={:08X}; rankings {:08X} +1c={:08X} +24={:08X} +30={:08X} +34={:08X} +38={:08X}",
                  uint32_t(ctx.lr), rk, xerenge::LoadGuestU32(base, rk + 0x1c), xerenge::LoadGuestU32(base, rk + 0x24),
                  xerenge::LoadGuestU32(base, rk + 0x30), xerenge::LoadGuestU32(base, rk + 0x34), xerenge::LoadGuestU32(base, rk + 0x38));
    }
    if (ctx.r5.u32) std::memset(xerenge::GuestPointer(base, ctx.r3.u32), 0, ctx.r5.u32);
    return;
  }
  __imp__sub_8259D2A0(ctx, base);
}

REX_HOOK_RAW(sub_823667E0) {
  if (Online()) {
    uint32_t msg = ctx.r4.u32, lobby = ctx.r5.u32;
    uint32_t tag = xerenge::LoadGuestU32(base, msg + 0x10);
    std::string s;
    for (uint32_t i = 0; i < 200 && tag; ++i) { char c = char(*xerenge::GuestPointer(base, tag + i)); if (!c) break; s += (c >= 32 && c < 127) ? c : '.'; }
    std::string dump; for (uint32_t o = 0; o < 0x40; o += 4) dump += fmt::format("{:08X} ", xerenge::LoadGuestU32(base, msg + o));
    REXLOG_INFO("--online: ChatCallback lr={:08X} msg={:08X} [{}] flags={:08X} cb={:08X} tag=\"{}\" queued={}", uint32_t(ctx.lr), msg, dump, xerenge::LoadGuestU32(base, msg + 0xC),
                xerenge::LoadGuestU32(base, lobby + 0x11C8), s, xerenge::LoadGuestU32(base, 0x82D2AE28));
  }
  __imp__sub_823667E0(ctx, base);
}

REX_HOOK_RAW(sub_82220EF8) {
  static uint32_t lastQueued = 0;
  uint32_t q = xerenge::LoadGuestU32(base, 0x82D2AE28), t = ctx.r3.u32;
  if (Online() && (q || lastQueued)) {
    std::string h;
    for (uint32_t o = 0; o < 0x20; o += 4) h += fmt::format("{:08X} ", xerenge::LoadGuestU32(base, t + o));
    REXLOG_INFO("--online: ArbitraryMessages pump this={:08X} queued={} handlers [{}]", t, q, h);
  }
  lastQueued = q;
  __imp__sub_82220EF8(ctx, base);
}

REX_HOOK_RAW(sub_822185D8) {
  const uint32_t us = ctx.r5.u32;
  if (Online()) {
    uint32_t d = ctx.r3.u32;
    REXLOG_INFO("--online: ArbitraryMessage type-0 handler data {:08X} {:08X} user={:08X} 395={} 396={}", xerenge::LoadGuestU32(base, d),
                xerenge::LoadGuestU32(base, d + 4), us, uint32_t(*xerenge::GuestPointer(base, us + 0x395)), uint32_t(*xerenge::GuestPointer(base, us + 0x396)));
  }
  __imp__sub_822185D8(ctx, base);
  if (Online()) {
    REXLOG_INFO("--online: ArbitraryMessage type-0 handler done: 395={} 396={}", uint32_t(*xerenge::GuestPointer(base, us + 0x395)), uint32_t(*xerenge::GuestPointer(base, us + 0x396)));
  }
}

REX_HOOK_RAW(sub_82367CF0) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82367CF0(ctx, base);
  if (Online() && (ctx.r3.u32 & 0xFF)) {
    const uint32_t data = xerenge::LoadGuestU32(base, out), len = xerenge::LoadGuestU32(base, out + 4), type = xerenge::LoadGuestU32(base, out + 8);
    std::string bytes;
    for (uint32_t i = 0; i < len && i < 16 && data; ++i) bytes += fmt::format("{:02X} ", uint32_t(*xerenge::GuestPointer(base, data + i)));
    REXLOG_INFO("--online: arbitrary message popped: type={} len={} data@{:08X} bytes [{}]", type, len, data, bytes);
  }
}

static std::string GuestCString(uint8_t* base, uint32_t p) {
  std::string s;
  for (uint32_t i = 0; i < 48 && p; ++i) { char c = char(*xerenge::GuestPointer(base, p + i)); if (!c) break; s += (c >= 32 && c < 127) ? c : '.'; }
  return s;
}

REX_HOOK_RAW(sub_8236FED0) {
  if (Online()) REXLOG_INFO("--online: KickUserFromUserSet name=\"{}\" reason={} lr={:08X}", GuestCString(base, ctx.r4.u32), ctx.r5.u32, uint32_t(ctx.lr));
  __imp__sub_8236FED0(ctx, base);
}

REX_HOOK_RAW(sub_8222E8F8) {
  if (Online()) REXLOG_INFO("--online: RebuildPlayerList arg={} lr={:08X}", ctx.r4.u32 & 0xFF, uint32_t(ctx.lr));
  __imp__sub_8222E8F8(ctx, base);
}

REX_HOOK_RAW(sub_821EEA68) {
  const std::string name = GuestCString(base, ctx.r4.u32);
  const uint32_t action = ctx.r5.u32;
  __imp__sub_821EEA68(ctx, base);
  if (Online()) REXLOG_INFO("--online: PlayerAction available? name=\"{}\" action={} -> {}", name, action, ctx.r3.u32 & 0xFF);
}

REX_HOOK_RAW(sub_821F3390) {
  const std::string name = GuestCString(base, ctx.r4.u32);
  const uint32_t action = ctx.r5.u32;
  if (Online()) REXLOG_INFO("--online: PlayerAction run name=\"{}\" action={} lr={:08X}", name, action, uint32_t(ctx.lr));
  __imp__sub_821F3390(ctx, base);
  if (Online()) REXLOG_INFO("--online: PlayerAction done action={} -> {}", action, int32_t(ctx.r3.u32));
}

REX_HOOK_RAW(sub_82368590) {
  if (Online()) REXLOG_INFO("--online: lobby kick from game name=\"{}\" lr={:08X}", GuestCString(base, ctx.r4.u32), uint32_t(ctx.lr));
  __imp__sub_82368590(ctx, base);
}

REX_HOOK_RAW(sub_82588030) {
  if (Online()) {
    const uint32_t ev = ctx.r4.u32, user = ctx.r5.u32;
    REXLOG_INFO("--online: ConnApi event {:08X} {:08X} {:08X} {:08X} {:08X} -> game callback {:08X} arg {:08X}",
                xerenge::LoadGuestU32(base, ev), xerenge::LoadGuestU32(base, ev + 4), xerenge::LoadGuestU32(base, ev + 8),
                xerenge::LoadGuestU32(base, ev + 0xC), xerenge::LoadGuestU32(base, ev + 0x10),
                xerenge::LoadGuestU32(base, user + 0x680), xerenge::LoadGuestU32(base, user + 0x684));
  }
  __imp__sub_82588030(ctx, base);
}

// CB4PostNetworkRaceManager::Update: state 5 of the custom game (returning from a race) waits for it to return 2.
REX_HOOK_RAW(sub_822302E8) {
  static uint32_t last_state = 0xFFFF, last_ret = 0xFFFF;
  const uint32_t self = ctx.r3.u32;
  const uint32_t before = xerenge::LoadGuestU32(base, self);
  __imp__sub_822302E8(ctx, base);
  if (Online()) {
    const uint32_t after = xerenge::LoadGuestU32(base, self), ret = ctx.r3.u32;
    if (after != last_state || ret != last_ret || before != after) {
      last_state = after;
      last_ret = ret;
      REXLOG_INFO("--online: PostNetworkRace update: state {} -> {}, returned {}", before, after, ret);
    }
  }
}
