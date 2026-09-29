// --online: which lobby server the title logs in to.
//
// Burnout finds EA's lobby as an Xbox Live title service (XOnlineGetServiceInfo
// for 0x45410004), so the choice is the address the SDK answers that lookup
// with, made the first time the title asks (during its "Signing in to Xbox
// Live"):
//
//   --lobby_server=<host>   a real server of your own (tools/ealobby) - at
//                           home, on a VPS. It is the only lobby then: no
//                           lobby of this copy is started. The title always
//                           uses its ports 31860/1.
//   otherwise               every copy runs a lobby of its own and the title
//                           always logs in to that one. The lobbies find each
//                           other on the LAN (UDP 31859: a broadcast and a
//                           multicast group that also reaches other copies on
//                           this machine): a copy announces the games it
//                           hosts, the others list them in their searches,
//                           and a player who joins one stays connected to its
//                           own lobby, which carries the game's traffic to
//                           the host's (ealobby::Server, "seamless
//                           multiplayer"). No copy is special and none has to
//                           be started first.
//
// Two copies on one machine can do the same with --online_address=127.0.0.2
// and 127.0.0.3; each then serves on its own address.

#include "online_lobby.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/kernel/xam/online.h>
#include <rex/logging.h>

#include "ealobby/server.h"

REXCVAR_DECLARE(bool, online);
REXCVAR_DECLARE(std::string, online_address);
REXCVAR_DEFINE_STRING(lobby_server, "", "Network",
                      "With --online: the host running a real lobby server (tools/ealobby). Empty: "
                      "run one in this game; the copies on the LAN find each other's games");

namespace {

std::string ToString(uint32_t address) {
  char text[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &address, text, sizeof(text));
  return text;
}

// --lobby_server: the name or address, as an IPv4 in network order; 0 if it
// cannot be resolved.
uint32_t Resolve(std::string host) {
  if (const size_t colon = host.find(':'); colon != std::string::npos) {
    REXLOG_WARN("--online: the title always uses ports 31860/31861; ignoring '{}'",
                host.substr(colon));
    host.resize(colon);
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) {
    return 0;
  }
  const uint32_t address = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(found);
  return address;
}

struct State {
  std::mutex mutex;
  uint32_t self = 0;  // network order: where this copy's lobby serves
  std::unique_ptr<ealobby::Server> server;
};

State& Lobby() {
  static State state;
  return state;
}

std::atomic<bool> g_started{false};

uint32_t ChooseLobby() {
  const std::string configured = REXCVAR_GET(lobby_server);
  if (!configured.empty()) {
    if (const uint32_t address = Resolve(configured)) {
      REXLOG_INFO("--online: using the lobby server at {} ({})", configured, ToString(address));
      return address;
    }
    REXLOG_ERROR("--online: cannot resolve --lobby_server={}; using this copy's own lobby instead",
                 configured);
  }
  xerenge::online::StartLobbyNetwork();
  std::lock_guard lock(Lobby().mutex);
  if (!Lobby().server) {
    REXLOG_WARN("--online: no lobby of this copy is running; trying 127.0.0.1");
    return htonl(INADDR_LOOPBACK);
  }
  REXLOG_INFO("--online: logging in to this copy's own lobby at {}", ToString(Lobby().self));
  return Lobby().self;
}

uint32_t LobbyAddress() {
  static std::once_flag once;
  static uint32_t address = 0;
  std::call_once(once, [] { address = ChooseLobby(); });
  return address;
}

[[maybe_unused]] const bool g_registered = [] {
  rex::kernel::xam::SetOnlineServiceAddressProvider([] {
    return REXCVAR_GET(online) ? LobbyAddress() : 0u;
  });
  return true;
}();

}  // namespace

namespace xerenge::online {

void StartLobbyNetwork() {
  if (!REXCVAR_GET(online) || !REXCVAR_GET(lobby_server).empty()) {
    return;
  }
  if (g_started.exchange(true)) {
    return;
  }
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const uint32_t self = rex::kernel::xam::OnlineLocalAddress();
  // With --online_address the lobby serves on that address alone, so that a
  // second copy on this machine can serve on another.
  ealobby::Options options;
  options.host = REXCVAR_GET(online_address).empty() ? "0.0.0.0" : ToString(self);
  options.self_address = ToString(self);
  auto server = std::make_unique<ealobby::Server>(
      options, [](const std::string& line) { REXLOG_INFO("lobby: {}", line); });
  const bool serving = server->Start();
  if (serving) {
    std::lock_guard lock(Lobby().mutex);
    Lobby().self = self;
    Lobby().server = std::move(server);
    REXLOG_INFO("--online: this copy's lobby is up at {} (ports 31860/31861)", ToString(self));
  } else {
    // Most often a standalone ealobby already on this machine.
    REXLOG_WARN("--online: could not start a lobby here (ports 31860/31861 in use?)");
  }
}

}  // namespace xerenge::online
