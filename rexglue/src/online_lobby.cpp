// --online: which lobby server the title logs in to.
//
// Burnout finds EA's lobby as an Xbox Live title service (XOnlineGetServiceInfo
// for 0x45410004), so the choice is the address the SDK answers that lookup
// with, made the first time the title asks (during its "Signing in to Xbox
// Live"):
//
//   --lobby_server=<host>   a server of your own (tools/ealobby) - at home,
//                           on a VPS. The title always uses its ports 31860/1.
//   otherwise               every copy starts a lobby of its own when the game
//                           starts and announces it on the LAN once a second
//                           (UDP 31859: broadcast, and a multicast group that
//                           also reaches other copies on this machine). The
//                           lobby used is the one started first among those
//                           heard - the same one on every machine, so players
//                           on a LAN meet with no server set up anywhere.
//
// Two copies on one machine can do the same with --online_address=127.0.0.2
// and 127.0.0.3; each then serves on its own address.
//
// Not handled: the chosen lobby's copy quitting takes the lobby with it.

#include "online_lobby.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/kernel/xam/online.h>
#include <rex/logging.h>

#include "ealobby/server.h"

REXCVAR_DECLARE(bool, online);
REXCVAR_DECLARE(std::string, online_address);
REXCVAR_DEFINE_STRING(lobby_server, "", "Network",
                      "With --online: the host running the lobby server (tools/ealobby). Empty: "
                      "run one in this game and use the first one started on the LAN");

namespace {

constexpr uint16_t kBeaconPort = 31859;
constexpr char kBeacon[] = "XERENGE-LOBBY 1";
// 239.255.77.77: administratively scoped, looped back to this machine too.
constexpr uint32_t kBeaconGroup = 0xEFFF4D4Du;
// A lobby not heard from for this long is gone.
constexpr auto kBeaconLifetime = std::chrono::seconds(4);
// How long after starting to wait for others before choosing, should the
// title ask that early.
constexpr auto kSettleTime = std::chrono::milliseconds(2500);

#ifdef _WIN32
using NativeSocket = SOCKET;
bool Valid(NativeSocket s) {
  return s != INVALID_SOCKET;
}
void CloseSocket(NativeSocket s) {
  closesocket(s);
}
using AddressLength = int;
#else
using NativeSocket = int;
bool Valid(NativeSocket s) {
  return s >= 0;
}
void CloseSocket(NativeSocket s) {
  ::close(s);
}
using AddressLength = socklen_t;
#endif

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

uint64_t WallMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count());
}

// A lobby is known by when its copy started and where it serves: the earliest
// started wins, the lower address breaking a tie.
struct LobbyId {
  uint64_t started_ms = 0;
  uint32_t address = 0;  // network order
  bool operator<(const LobbyId& other) const {
    if (started_ms != other.started_ms) {
      return started_ms < other.started_ms;
    }
    return ntohl(address) < ntohl(other.address);
  }
};

struct State {
  std::mutex mutex;
  std::map<LobbyId, std::chrono::steady_clock::time_point> heard;  // other copies' lobbies
  LobbyId self;
  bool hosting = false;
  std::chrono::steady_clock::time_point started_at;
  std::unique_ptr<ealobby::Server> server;
};

State& Lobby() {
  static State state;
  return state;
}

std::atomic<bool> g_started{false};

// Announces this copy's lobby (while it has one) and collects the others'.
void BeaconLoop() {
  const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (!Valid(s)) {
    REXLOG_WARN("--online: no UDP socket for the LAN lobby beacon");
    return;
  }
  const int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_REUSEPORT
  // Every copy on this machine listens on the same port.
  setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in at{};
  at.sin_family = AF_INET;
  at.sin_port = htons(kBeaconPort);
  at.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
    REXLOG_WARN("--online: cannot listen on UDP {}; lobbies on the LAN will not be found",
                kBeaconPort);
    CloseSocket(s);
    return;
  }
  ip_mreq group{};
  group.imr_multiaddr.s_addr = htonl(kBeaconGroup);
  group.imr_interface.s_addr = htonl(INADDR_ANY);
  setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&group),
             sizeof(group));
  const int loop = 1;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop),
             sizeof(loop));

  auto next_beacon = std::chrono::steady_clock::now();
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_beacon) {
      next_beacon = now + std::chrono::seconds(1);
      std::string beacon;
      {
        std::lock_guard lock(Lobby().mutex);
        if (Lobby().hosting) {
          beacon = std::string(kBeacon) + " " + ToString(Lobby().self.address) + " " +
                   std::to_string(Lobby().self.started_ms);
        }
      }
      if (!beacon.empty()) {
        for (const uint32_t to_address : {htonl(INADDR_BROADCAST), htonl(kBeaconGroup)}) {
          sockaddr_in to{};
          to.sin_family = AF_INET;
          to.sin_port = htons(kBeaconPort);
          to.sin_addr.s_addr = to_address;
          ::sendto(s, beacon.data(), int(beacon.size()), 0, reinterpret_cast<sockaddr*>(&to),
                   sizeof(to));
        }
      }
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval wait{0, 200000};
    if (::select(int(s) + 1, &readable, nullptr, nullptr, &wait) <= 0) {
      continue;
    }
    char in[128];
    sockaddr_in from{};
    AddressLength from_length = sizeof(from);
    const int got =
        int(::recvfrom(s, in, sizeof(in) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_length));
    if (got < int(sizeof(kBeacon) - 1) || std::memcmp(in, kBeacon, sizeof(kBeacon) - 1) != 0) {
      continue;
    }
    // "XERENGE-LOBBY 1 <address> <started ms>"
    in[got] = '\0';
    char address_text[INET_ADDRSTRLEN + 1] = {};
    unsigned long long started = 0;
    if (std::sscanf(in + sizeof(kBeacon) - 1, " %16s %llu", address_text, &started) != 2) {
      continue;
    }
    in_addr named{};
    if (inet_pton(AF_INET, address_text, &named) != 1) {
      continue;
    }
    // A lobby on 0.0.0.0 or this machine's loopback is reached at the sender.
    uint32_t address = named.s_addr;
    if (address == 0) {
      address = from.sin_addr.s_addr;
    }
    const LobbyId id{uint64_t(started), address};
    std::lock_guard lock(Lobby().mutex);
    if (id.started_ms == Lobby().self.started_ms && id.address == Lobby().self.address) {
      continue;
    }
    const bool fresh = Lobby().heard.find(id) == Lobby().heard.end();
    Lobby().heard[id] = std::chrono::steady_clock::now();
    if (fresh) {
      REXLOG_INFO("--online: a lobby on the LAN at {} (started {} ms {} this one)",
                  ToString(address),
                  started > Lobby().self.started_ms ? started - Lobby().self.started_ms
                                                    : Lobby().self.started_ms - started,
                  started > Lobby().self.started_ms ? "after" : "before");
    }
  }
}

uint32_t ChooseLobby() {
  const std::string configured = REXCVAR_GET(lobby_server);
  if (!configured.empty()) {
    if (const uint32_t address = Resolve(configured)) {
      REXLOG_INFO("--online: using the lobby server at {} ({})", configured, ToString(address));
      return address;
    }
    REXLOG_ERROR("--online: cannot resolve --lobby_server={}; using the LAN instead",
                 configured);
  }
  xerenge::online::StartLobbyNetwork();

  // Asked right after starting, the others' beacons may not have arrived yet.
  State& lobby = Lobby();
  std::chrono::steady_clock::time_point started_at;
  {
    std::lock_guard lock(lobby.mutex);
    started_at = lobby.started_at;
  }
  const auto settled = started_at + kSettleTime;
  if (std::chrono::steady_clock::now() < settled) {
    std::this_thread::sleep_until(settled);
  }

  std::lock_guard lock(lobby.mutex);
  const auto now = std::chrono::steady_clock::now();
  bool found = false;
  LobbyId best{};
  if (lobby.hosting) {
    best = lobby.self;
    found = true;
  }
  size_t alive = 0;
  for (const auto& [id, seen] : lobby.heard) {
    if (now - seen > kBeaconLifetime) {
      continue;
    }
    ++alive;
    if (!found || id < best) {
      best = id;
      found = true;
    }
  }
  if (!found) {
    REXLOG_WARN("--online: no lobby here or on the LAN; trying 127.0.0.1");
    return htonl(INADDR_LOOPBACK);
  }
  const bool own = lobby.hosting && best.started_ms == lobby.self.started_ms &&
                   best.address == lobby.self.address;
  REXLOG_INFO("--online: {} other lobby(ies) on the LAN; using {} at {}", alive,
              own ? "this copy's own" : "the one started first", ToString(best.address));
  return best.address;
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
  State& lobby = Lobby();
  const uint32_t self = rex::kernel::xam::OnlineLocalAddress();
  // With --online_address the lobby serves on that address alone, so that a
  // second copy on this machine can serve on another.
  ealobby::Options options;
  options.host = REXCVAR_GET(online_address).empty() ? "0.0.0.0" : ToString(self);
  auto server = std::make_unique<ealobby::Server>(
      options, [](const std::string& line) { REXLOG_INFO("lobby: {}", line); });
  const bool serving = server->Start();
  {
    std::lock_guard lock(lobby.mutex);
    lobby.started_at = std::chrono::steady_clock::now();
    lobby.self = {WallMs(), self};
    lobby.hosting = serving;
    if (serving) {
      lobby.server = std::move(server);
    }
  }
  if (serving) {
    REXLOG_INFO("--online: this copy's lobby is up at {} (ports 31860/31861)", ToString(self));
  } else {
    // Most often a standalone ealobby already on this machine; it is found
    // through its own beacon if it sends one, or at 127.0.0.1.
    REXLOG_WARN("--online: could not start a lobby here (ports 31860/31861 in use?)");
  }
  std::thread(BeaconLoop).detach();
}

}  // namespace xerenge::online
