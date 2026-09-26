// --online: which lobby server the title logs in to.
//
// Burnout finds EA's lobby as an Xbox Live title service (XOnlineGetServiceInfo
// for 0x45410004), so the choice is the address the SDK answers that lookup
// with. It is made once, the first time the title asks (during its "Signing
// in to Xbox Live"):
//
//   --lobby_server=<host>   a server of your own (tools/ealobby) - at home,
//                           on a VPS. The title always uses its ports 31860/1.
//   otherwise               the LAN: ask on UDP 31859 whether another copy of
//                           the game already runs a lobby; if one answers, use
//                           it; if none does, run one in this process (and
//                           answer that question for the others).
//
// So two machines on a LAN find each other with no server set up anywhere;
// the first one online hosts. Two copies on one machine can do the same with
// --online_address=127.0.0.2 and 127.0.0.3.

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

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/kernel/xam/online.h>
#include <rex/logging.h>

#include "ealobby/server.h"

REXCVAR_DECLARE(bool, online);
REXCVAR_DEFINE_STRING(lobby_server, "", "Network",
                      "With --online: the host running the lobby server (tools/ealobby). Empty: "
                      "find one on the LAN, or run one in this game when there is none");

namespace {

constexpr uint16_t kDiscoveryPort = 31859;
constexpr char kQuery[] = "XERENGE-LOBBY?";
constexpr char kAnswer[] = "XERENGE-LOBBY!";

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

// Asks the LAN (and this machine) whether a lobby is running; the address of
// the first copy that answers, or 0.
uint32_t Discover() {
  const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (!Valid(s)) {
    return 0;
  }
  const int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
  // From this console's own address, so that a copy on 127.0.0.3 is answered
  // at 127.0.0.3.
  sockaddr_in self{};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = rex::kernel::xam::OnlineLocalAddress();
  ::bind(s, reinterpret_cast<sockaddr*>(&self), sizeof(self));

  // A random pause first, so that two machines started together do not both
  // miss each other and both host.
  static std::mt19937 random{std::random_device{}()};
  std::this_thread::sleep_for(std::chrono::milliseconds(random() % 400));

  const auto send_to = [&](uint32_t address) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kDiscoveryPort);
    to.sin_addr.s_addr = address;
    ::sendto(s, kQuery, int(sizeof(kQuery) - 1), 0, reinterpret_cast<sockaddr*>(&to),
             sizeof(to));
  };
  uint32_t answer = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
  auto next_ask = std::chrono::steady_clock::now();
  while (!answer) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    if (now >= next_ask) {
      send_to(htonl(INADDR_BROADCAST));
      send_to(htonl(INADDR_LOOPBACK));  // another copy on this machine
      next_ask = now + std::chrono::milliseconds(300);
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval wait{0, 100000};
    if (::select(int(s) + 1, &readable, nullptr, nullptr, &wait) <= 0) {
      continue;
    }
    char in[128];
    sockaddr_in from{};
    AddressLength from_length = sizeof(from);
    const int got =
        int(::recvfrom(s, in, sizeof(in) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_length));
    if (got < int(sizeof(kAnswer) - 1) || std::memcmp(in, kAnswer, sizeof(kAnswer) - 1) != 0) {
      continue;
    }
    // "XERENGE-LOBBY! <address>": the address the host wants to be reached
    // at; the sender's address when it names none.
    in[got] = '\0';
    in_addr named{};
    const char* text = in + sizeof(kAnswer) - 1;
    while (*text == ' ') {
      ++text;
    }
    answer = (*text && inet_pton(AF_INET, text, &named) == 1 && named.s_addr != 0)
                 ? uint32_t(named.s_addr)
                 : uint32_t(from.sin_addr.s_addr);
  }
  CloseSocket(s);
  return answer;
}

// Answers other copies' questions for as long as this one hosts.
void AnswerDiscovery(uint32_t hosted_at) {
  std::thread([hosted_at] {
    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!Valid(s)) {
      return;
    }
    const int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_port = htons(kDiscoveryPort);
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
      REXLOG_WARN("--online: cannot answer lobby queries on UDP {}; other players will not find "
                  "this lobby",
                  kDiscoveryPort);
      CloseSocket(s);
      return;
    }
    const std::string answer = std::string(kAnswer) + " " + ToString(hosted_at);
    for (;;) {
      char in[64];
      sockaddr_in from{};
      AddressLength from_length = sizeof(from);
      const int got = int(
          ::recvfrom(s, in, sizeof(in), 0, reinterpret_cast<sockaddr*>(&from), &from_length));
      if (got < int(sizeof(kQuery) - 1) || std::memcmp(in, kQuery, sizeof(kQuery) - 1) != 0) {
        continue;
      }
      REXLOG_INFO("--online: {} asked for the lobby", ToString(from.sin_addr.s_addr));
      ::sendto(s, answer.data(), int(answer.size()), 0, reinterpret_cast<sockaddr*>(&from),
               from_length);
    }
  }).detach();
}

std::unique_ptr<ealobby::Server> g_hosted;

uint32_t ChooseLobby() {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const std::string configured = REXCVAR_GET(lobby_server);
  if (!configured.empty()) {
    const uint32_t address = Resolve(configured);
    if (address) {
      REXLOG_INFO("--online: using the lobby server at {} ({})", configured, ToString(address));
      return address;
    }
    REXLOG_ERROR("--online: cannot resolve --lobby_server={}; looking on the LAN instead",
                 configured);
  }

  if (const uint32_t found = Discover()) {
    REXLOG_INFO("--online: joining the lobby at {} on the LAN", ToString(found));
    return found;
  }

  const uint32_t self = rex::kernel::xam::OnlineLocalAddress();
  ealobby::Options options;
  options.host = "0.0.0.0";
  g_hosted = std::make_unique<ealobby::Server>(
      options, [](const std::string& line) { REXLOG_INFO("lobby: {}", line); });
  if (!g_hosted->Start()) {
    // Most often a standalone ealobby already on this machine.
    g_hosted.reset();
    REXLOG_WARN("--online: no lobby found and none could be started here; trying 127.0.0.1");
    return htonl(INADDR_LOOPBACK);
  }
  AnswerDiscovery(self);
  REXLOG_INFO("--online: no lobby on the LAN; hosting one at {}", ToString(self));
  return self;
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
