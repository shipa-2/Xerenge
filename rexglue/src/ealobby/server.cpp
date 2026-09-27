#include "ealobby/server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

namespace ealobby {
namespace {

// The Windows Sockets API differs from BSD sockets in its types (SOCKET vs.
// int), close/error/blocking calls, and needs WSAStartup once per process;
// WSAPoll stands in for poll() and a loopback socket pair for the wake pipe,
// since Windows has no fd that is both a pipe and pollable as a socket.
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNativeSocket = INVALID_SOCKET;
using PollFd = WSAPOLLFD;
using SockLen = int;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNativeSocket = -1;
using PollFd = pollfd;
using SockLen = socklen_t;
#endif

NativeSocket ToNative(SocketHandle handle) {
  return static_cast<NativeSocket>(handle);
}

SocketHandle FromNative(NativeSocket socket) {
  return static_cast<SocketHandle>(socket);
}

int LastSocketError() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool WouldBlock(int code) {
#ifdef _WIN32
  return code == WSAEWOULDBLOCK;
#else
  return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

bool Interrupted(int code) {
#ifdef _WIN32
  return code == WSAEINTR;
#else
  return code == EINTR;
#endif
}

std::string SocketErrorString(int code) {
#ifdef _WIN32
  char text[256] = {};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), text,
                 sizeof(text), nullptr);
  std::string message(text);
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  return message;
#else
  return std::strerror(code);
#endif
}

void CloseNative(NativeSocket socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

bool SetNonBlocking(NativeSocket socket) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
  return fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK) == 0;
#endif
}

int PollWait(std::vector<PollFd>& fds, int timeout_ms) {
#ifdef _WIN32
  return WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
#else
  return poll(fds.data(), fds.size(), timeout_ms);
#endif
}

PollFd MakePollFd(SocketHandle handle, short events) {
  PollFd entry{};
  entry.fd = static_cast<decltype(PollFd::fd)>(handle);
  entry.events = events;
  entry.revents = 0;
  return entry;
}

#ifdef _WIN32
struct WinsockInit {
  WinsockInit() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockInit() { WSACleanup(); }
};

// Windows has no fd that is both a pipe and poll()-able as a socket, so the
// wake signal rides a loopback TCP connection instead: a listener on an
// ephemeral port, a writer that connects to it, and the accepted end as the
// reader.
bool MakeWakePair(SocketHandle wake[2]) {
  static WinsockInit winsock_init;
  const NativeSocket listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener == kInvalidNativeSocket) {
    return false;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  SockLen length = sizeof(address);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
      listen(listener, 1) != 0) {
    CloseNative(listener);
    return false;
  }
  const NativeSocket writer = socket(AF_INET, SOCK_STREAM, 0);
  if (writer == kInvalidNativeSocket ||
      connect(writer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    CloseNative(listener);
    if (writer != kInvalidNativeSocket) {
      CloseNative(writer);
    }
    return false;
  }
  const NativeSocket reader = accept(listener, nullptr, nullptr);
  CloseNative(listener);
  if (reader == kInvalidNativeSocket) {
    CloseNative(writer);
    return false;
  }
  wake[0] = FromNative(reader);
  wake[1] = FromNative(writer);
  return true;
}
#endif

std::string AddressString(const sockaddr_in& address) {
  char text[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
  return text;
}

std::string RandomHex(size_t bytes) {
  static std::mt19937_64 random{std::random_device{}()};
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < bytes; ++i) {
    const unsigned value = unsigned(random() & 0xFF);
    out += digits[value >> 4];
    out += digits[value & 0xF];
  }
  return out;
}

std::string CurrentTimeString() {
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof(stamp), "%Y.%m.%d %H:%M:%S", std::localtime(&now));
  return stamp;
}

// std::stoul throws on anything that isn't a valid number, which a client
// field (IDENT and the like) is never guaranteed to be; letting that escape
// the server thread's poll loop would take both players down with it.
uint32_t ParseUint(const std::string& text, uint32_t fallback = 0) {
  try {
    return uint32_t(std::stoul(text));
  } catch (const std::exception&) {
    return fallback;
  }
}

}  // namespace

Server::Server(Options options, LogFunction log) : options_(std::move(options)), log_(std::move(log)) {}

Server::~Server() {
  Stop();
}

void Server::Log(const std::string& line) const {
  if (log_) {
    log_(line);
  }
}

SocketHandle Server::Listen(uint16_t port) {
#ifdef _WIN32
  static WinsockInit winsock_init;
#endif
  const NativeSocket fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == kInvalidNativeSocket) {
    Log("ealobby: socket: " + SocketErrorString(LastSocketError()));
    return -1;
  }
  const int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
    Log("ealobby: bad listen address " + options_.host);
    CloseNative(fd);
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(fd, 16) < 0) {
    Log("ealobby: cannot listen on " + options_.host + ":" + std::to_string(port) + ": " +
        SocketErrorString(LastSocketError()));
    CloseNative(fd);
    return -1;
  }
  SetNonBlocking(fd);
  return FromNative(fd);
}

bool Server::Start() {
  if (running_) {
    return true;
  }
  directory_listener_ = Listen(options_.directory_port);
  lobby_listener_ = Listen(options_.lobby_port);
#ifdef _WIN32
  const bool wake_ok = MakeWakePair(wake_);
#else
  int wake_fds[2] = {-1, -1};
  const bool wake_ok = pipe(wake_fds) == 0;
  wake_[0] = wake_fds[0];
  wake_[1] = wake_fds[1];
#endif
  if (directory_listener_ < 0 || lobby_listener_ < 0 || !wake_ok) {
    Stop();
    return false;
  }
  SetNonBlocking(ToNative(wake_[0]));
  running_ = true;
  thread_ = std::thread([this] { Run(); });
  Log("ealobby: directory on " + options_.host + ":" + std::to_string(options_.directory_port) +
      ", lobby on port " + std::to_string(options_.lobby_port));
  return true;
}

void Server::Stop() {
  if (running_.exchange(false) && wake_[1] >= 0) {
    const char byte = 0;
#ifdef _WIN32
    send(ToNative(wake_[1]), &byte, 1, 0);
#else
    [[maybe_unused]] ssize_t written = write(wake_[1], &byte, 1);
#endif
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  for (auto& [fd, connection] : connections_) {
    CloseNative(ToNative(fd));
  }
  connections_.clear();
  for (SocketHandle* fd : {&directory_listener_, &lobby_listener_, &wake_[0], &wake_[1]}) {
    if (*fd >= 0) {
      CloseNative(ToNative(*fd));
      *fd = -1;
    }
  }
}

void Server::Run() {
  while (running_) {
    std::vector<PollFd> fds;
    fds.push_back(MakePollFd(wake_[0], POLLIN));
    fds.push_back(MakePollFd(directory_listener_, POLLIN));
    fds.push_back(MakePollFd(lobby_listener_, POLLIN));
    for (auto& [fd, connection] : connections_) {
      fds.push_back(MakePollFd(fd, short(POLLIN | (connection->out.empty() ? 0 : POLLOUT))));
    }
    if (PollWait(fds, 1000) < 0) {
      const int code = LastSocketError();
      if (Interrupted(code)) {
        continue;
      }
      Log("ealobby: poll: " + SocketErrorString(code));
      break;
    }
    if (fds[1].revents & POLLIN) {
      Accept(directory_listener_, Role::kDirectory);
    }
    if (fds[2].revents & POLLIN) {
      Accept(lobby_listener_, Role::kLobby);
    }
    std::vector<SocketHandle> closed;
    for (size_t i = 3; i < fds.size(); ++i) {
      auto found = connections_.find(static_cast<SocketHandle>(fds[i].fd));
      if (found == connections_.end()) {
        continue;
      }
      Connection& connection = *found->second;
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        Receive(connection);
      }
      if (connection.fd >= 0 && (fds[i].revents & POLLOUT)) {
        Flush(connection);
      }
      if (connection.fd < 0) {
        closed.push_back(static_cast<SocketHandle>(fds[i].fd));
      }
    }
    for (SocketHandle fd : closed) {
      OnDisconnect(fd);
      connections_.erase(fd);
    }
  }
}

void Server::Accept(SocketHandle listener, Role role) {
  for (;;) {
    sockaddr_in peer = {};
    SockLen peer_length = sizeof(peer);
    const NativeSocket native =
        accept(ToNative(listener), reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (native == kInvalidNativeSocket) {
      return;
    }
    const SocketHandle fd = FromNative(native);
    SetNonBlocking(native);
    const int yes = 1;
    setsockopt(native, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    auto connection = std::make_unique<Connection>();
    connection->fd = fd;
    connection->role = role;
    connection->peer = AddressString(peer) + ":" + std::to_string(ntohs(peer.sin_port));
    connection->peer_ip = AddressString(peer);
    sockaddr_in local = {};
    SockLen local_length = sizeof(local);
    getsockname(native, reinterpret_cast<sockaddr*>(&local), &local_length);
    connection->local_address = AddressString(local);
    Log(std::string(role == Role::kDirectory ? "directory" : "lobby") + ": connection from " +
        connection->peer);
    connections_[fd] = std::move(connection);
  }
}

void Server::Receive(Connection& connection) {
  uint8_t chunk[4096];
  const NativeSocket native = ToNative(connection.fd);
  for (;;) {
    const auto received = recv(native, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
    if (received > 0) {
      connection.in.insert(connection.in.end(), chunk, chunk + received);
      continue;
    }
    if (received < 0 && WouldBlock(LastSocketError())) {
      break;
    }
    Log(std::string(connection.role == Role::kDirectory ? "directory" : "lobby") + ": " +
        connection.peer + " closed");
    CloseNative(native);
    connection.fd = -1;
    return;
  }
  Message message;
  while (connection.fd >= 0 && TakeMessage(&connection.in, &message)) {
    Handle(connection, message);
  }
}

void Server::Flush(Connection& connection) {
  const NativeSocket native = ToNative(connection.fd);
  while (!connection.out.empty()) {
#ifdef _WIN32
    const auto sent = send(native, reinterpret_cast<const char*>(connection.out.data()),
                            static_cast<int>(connection.out.size()), 0);
#else
    const auto sent =
        send(native, connection.out.data(), connection.out.size(), MSG_NOSIGNAL);
#endif
    if (sent <= 0) {
      if (sent < 0 && WouldBlock(LastSocketError())) {
        return;
      }
      CloseNative(native);
      connection.fd = -1;
      return;
    }
    connection.out.erase(connection.out.begin(), connection.out.begin() + sent);
  }
}

void Server::Send(Connection& connection, const std::vector<uint8_t>& bytes) {
  connection.out.insert(connection.out.end(), bytes.begin(), bytes.end());
  Flush(connection);
}

void Server::Handle(Connection& connection, const Message& message) {
  const char* who = connection.role == Role::kDirectory ? "directory" : "lobby";
  Log(std::string(who) + " <- " + Printable(message.command) + " [" + Printable(message.code) +
      "] " + Printable(message.body));
  if (connection.role == Role::kDirectory) {
    HandleDirectory(connection, message);
  } else {
    HandleLobby(connection, message);
  }
}

void Server::HandleDirectory(Connection& connection, const Message& message) {
  if (message.command == "@tic") {
    // The client offers to encrypt the session (RC4+MD5-V2). Not answering
    // keeps it in the clear.
    return;
  }
  if (message.command == "@dir") {
    Fields reply;
    reply["ADDR"] = options_.advertise.empty() ? connection.local_address : options_.advertise;
    reply["PORT"] = std::to_string(options_.lobby_port);
    reply["SESS"] = std::to_string(next_session_++);
    reply["MASK"] = RandomHex(16);
    Log("directory -> @dir " + Printable(FormatFields(reply)));
    Send(connection, Encode("@dir", reply));
    return;
  }
  Log("directory: " + Printable(message.command) + " not handled yet; answered empty");
  Send(connection, Encode(message.command, std::string(1, '\0')));
}

void Server::HandleLobby(Connection& connection, const Message& message) {
  if (message.command == "addr") {
    // The client's own address and port, as it sees them.
    Fields fields = ParseFields(message.body);
    Fields reply;
    reply["ADDR"] = options_.advertise.empty()
                        ? (connection.peer_ip.empty() ? "127.0.0.1" : connection.peer_ip)
                        : options_.advertise;
    reply["PORT"] = fields.count("PORT") ? fields["PORT"] : "3074";
    Log("lobby -> addr " + Printable(FormatFields(reply)));
    Send(connection, Encode("addr", reply));
    return;
  }
  if (message.command == "skey") {
    // The session key exchange. The client reads SKEY (binary, $hex) and DP
    // from the reply, then counts itself connected.
    Fields reply;
    reply["SKEY"] = "$" + RandomHex(16);
    reply["DP"] = "XBL2/Burnout-Feb2006";
    Log("lobby -> skey " + Printable(FormatFields(reply)));
    Send(connection, Encode("skey", reply));
    return;
  }
  if (message.command == "news") {
    // NAME=7: the configuration the lobby code reads back later - where the
    // news and terms of service are, and the buddy server. There are none.
    Fields reply;
    reply["BUDDY_SERVER"] = connection.local_address;
    reply["BUDDY_PORT"] = "0";
    reply["BUDDY_URL"] = "";
    reply["TOSAC_URL"] = "";
    reply["NEWS_URL"] = "";
    reply["NEWS_DATE"] = "2006.2.10-0:00:00";
    Log("lobby -> news " + Printable(FormatFields(reply)));
    Send(connection, Encode("news", reply));
    return;
  }
  if (message.command == "auth") {
    HandleAuth(connection, message);
    return;
  }
  if (message.command == "user") {
    HandleUser(connection, message);
    return;
  }
  if (message.command == "cper") {
    HandleCper(connection, message);
    return;
  }
  if (message.command == "pers") {
    HandlePers(connection, message);
    return;
  }
  if (message.command == "sele") {
    HandleSele(connection, message);
    return;
  }
  if (message.command == "qdef") {
    HandleQdef(connection, message);
    return;
  }
  if (message.command == "slst") {
    HandleSlst(connection, message);
    return;
  }
  if (message.command == "priv") {
    HandlePriv(connection, message);
    return;
  }
  if (message.command == "llvl") {
    HandleLlvl(connection, message);
    return;
  }
  if (message.command == "uatr") {
    HandleUatr(connection, message);
    return;
  }
  if (message.command == "rcat") {
    HandleRcat(connection, message);
    return;
  }
  if (message.command == "room") {
    HandleRoom(connection, message);
    return;
  }
  if (message.command == "move") {
    HandleMove(connection, message);
    return;
  }
  if (message.command == "gsea") {
    HandleGsea(connection, message);
    return;
  }
  if (message.command == "gcre") {
    HandleGcre(connection, message);
    return;
  }
  if (message.command == "gjoi") {
    HandleGjoi(connection, message);
    return;
  }
  if (message.command == "gget") {
    HandleGget(connection, message);
    return;
  }
  if (message.command == "gset") {
    HandleGset(connection, message);
    return;
  }
  if (message.command == "gsta") {
    HandleGsta(connection, message);
    return;
  }
  if (message.command == "glea") {
    HandleGlea(connection, message);
    return;
  }
  if (message.command == "mesg") {
    HandleMesg(connection, message);
    return;
  }
  if (message.command == "conn" || message.command == "ping" || message.command == "~png") {
    Send(connection, Encode(message.command, std::string(1, '\0')));
    return;
  }

  // Not handled yet: an empty success, to see what comes next.
  Log("lobby: " + Printable(message.command) + " not handled yet; answered empty");
  Send(connection, Encode(message.command, std::string(1, '\0')));
}

void Server::HandleAuth(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  std::string name = fields["NAME"];
  if (name.empty()) {
    name = "BurnoutPlayer";
  }
  connection.player.account_name = name;
  connection.player.persona = name;
  connection.player.ip = connection.peer_ip.empty() ? "127.0.0.1" : connection.peer_ip;
  if (connection.player.id == 0) {
    connection.player.id = next_player_id_++;
  }

  Fields reply;
  reply["NAME"] = name;
  reply["ADDR"] = connection.player.ip;
  reply["PERSONAS"] = name;
  reply["LOC"] = "enUS";
  reply["MAIL"] = name + "@revenge.local";
  reply["SPAM"] = "NN";
  reply["TOS"] = "1";
  Log("lobby -> auth " + Printable(FormatFields(reply)));
  Send(connection, Encode("auth", reply));
}

void Server::HandleUser(Connection& connection, const Message& /*message*/) {
  Fields reply;
  reply["NAME"] = connection.player.account_name;
  reply["SPAM"] = "NN";
  reply["MAIL"] = connection.player.account_name + "@revenge.local";
  Log("lobby -> user " + Printable(FormatFields(reply)));
  Send(connection, Encode("user", reply));
}

void Server::HandleCper(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  std::string pers = fields["PERS"];
  if (!pers.empty()) {
    connection.player.persona = pers;
  }
  Fields reply;
  reply["PERS"] = connection.player.persona;
  Log("lobby -> cper " + Printable(FormatFields(reply)));
  Send(connection, Encode("cper", reply));
}

void Server::HandlePers(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  std::string pers = fields["PERS"];
  if (!pers.empty()) {
    connection.player.persona = pers;
  }
  if (connection.player.lkey.empty()) {
    connection.player.lkey = RandomHex(16);
  }
  std::string ip = connection.player.ip.empty() ? "127.0.0.1" : connection.player.ip;

  Fields reply;
  reply["PERS"] = connection.player.persona;
  reply["LKEY"] = connection.player.lkey;
  reply["EX-ticker"] = "";
  reply["LOC"] = "enUS";
  reply["A"] = ip;
  reply["LA"] = ip;
  reply["IDLE"] = "100000";
  Log("lobby -> pers " + Printable(FormatFields(reply)));
  Send(connection, Encode("pers", reply));

  SendWho(connection);
}

void Server::HandleSele(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  Fields reply;
  reply["INGAME"] = fields.count("INGAME") ? fields["INGAME"] : "0";
  reply["GAMES"] = fields.count("GAMES") ? fields["GAMES"] : "0";
  reply["MYGAME"] = fields.count("MYGAME") ? fields["MYGAME"] : "0";
  reply["ROOMS"] = fields.count("ROOMS") ? fields["ROOMS"] : "0";
  reply["USERS"] = fields.count("USERS") ? fields["USERS"] : "0";
  reply["USERSETS"] = fields.count("USERSETS") ? fields["USERSETS"] : "0";
  reply["MESGS"] = fields.count("MESGS") ? fields["MESGS"] : "1";
  reply["MESGTYPES"] = fields.count("MESGTYPES") ? fields["MESGTYPES"] : "GPY";
  reply["ASYNC"] = fields.count("ASYNC") ? fields["ASYNC"] : "0";
  reply["STATS"] = fields.count("STATS") ? fields["STATS"] : "0";
  reply["SLOTS"] = "3";
  Log("lobby -> sele " + Printable(FormatFields(reply)));
  Send(connection, Encode("sele", reply));

  if (fields.count("STATS") || fields.count("INGAME") || fields.count("ROOMS")) {
    SendWho(connection);
  }
}

void Server::HandleQdef(Connection& connection, const Message& /*message*/) {
  Fields reply;
  reply["IMGATE"] = "0";
  reply["QMSG0"] = "\"Do you want to play a game?\"";
  reply["QMSG1"] = "Yes";
  reply["QMSG2"] = "No";
  reply["QMSG3"] = "\"OK, let's start\"";
  reply["QMSG4"] = "\"Have fun!\"";
  reply["QMSG5"] = "\"Good game!\"";
  reply["QMSG6"] = "Thanks!";
  reply["QMSG7"] = "\"Catch you later\"";
  reply["QMSG8"] = "\"See Ya!\"";
  reply["SPM_EA"] = "0";
  reply["SPM_PART"] = "0";
  Log("lobby -> qdef " + Printable(FormatFields(reply)));
  Send(connection, Encode("qdef", reply));
}

void Server::HandleSlst(Connection& connection, const Message& /*message*/) {
  Fields reply;
  reply["COUNT"] = "1";
  reply["VIEW0"] = "Career,\"My Career\"";
  Log("lobby -> slst " + Printable(FormatFields(reply)));
  Send(connection, Encode("slst", reply));
}

void Server::HandlePriv(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  std::string mode = fields["MODE"];
  Fields reply;
  reply["PRIV"] = (mode == "off") ? "0" : "1";
  Log("lobby -> priv " + Printable(FormatFields(reply)));
  Send(connection, Encode("priv", reply));
}

void Server::HandleLlvl(Connection& connection, const Message& /*message*/) {
  Fields reply;
  reply["SKILL_PTS"] = "0";
  reply["SKILL_LVL"] = "0";
  reply["SKILL"] = "";
  Log("lobby -> llvl " + Printable(FormatFields(reply)));
  Send(connection, Encode("llvl", reply));
}

void Server::HandleUatr(Connection& connection, const Message& /*message*/) {
  Log("lobby -> uatr success");
  Send(connection, Encode("uatr", std::string(1, '\0')));
  SendWho(connection);
}

void Server::HandleRcat(Connection& connection, const Message& /*message*/) {
  Fields reply;
  reply["COUNT"] = "1";
  reply["CAT0"] = "1,\"Main Categories\",0,50";
  Log("lobby -> rcat " + Printable(FormatFields(reply)));
  Send(connection, Encode("rcat", reply));

  SendRoomUpdate(connection);
}

void Server::HandleRoom(Connection& connection, const Message& /*message*/) {
  Fields reply;
  reply["COUNT"] = "1";
  reply["ROOM0"] = "1,LVL.1,\"Revenge Lobby\",,A,1,50";
  Log("lobby -> room " + Printable(FormatFields(reply)));
  Send(connection, Encode("room", reply));

  SendRoomUpdate(connection);
}

void Server::HandleMove(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  std::string ident = fields["IDENT"];
  if (ident.empty() || ident == "0") {
    ident = "1";
  }
  connection.player.room_id = ParseUint(ident, 1);

  Fields reply;
  reply["IDENT"] = ident;
  reply["NAME"] = "LVL.1";
  reply["COUNT"] = std::to_string(GetRoomPlayerCount(connection.player.room_id));
  reply["FLAGS"] = "A";
  reply["LIMIT"] = "50";
  reply["DESC"] = "Burnout Revenge Lobby";
  Log("lobby -> move " + Printable(FormatFields(reply)));
  Send(connection, Encode("move", reply));

  for (const auto& [gid, session] : games_) {
    if (!session.started && session.room_id == connection.player.room_id) {
      Send(connection, Encode("+gam", FormatGameInfo(session)));
    }
  }

  SendWho(connection);
}

void Server::HandleGsea(Connection& connection, const Message& /*message*/) {
  std::vector<GameSession> available;
  for (const auto& [gid, session] : games_) {
    if (!session.started && session.players.size() < session.maxsize) {
      available.push_back(session);
    }
  }

  Fields reply;
  reply["COUNT"] = std::to_string(available.size());
  Log("lobby -> gsea " + Printable(FormatFields(reply)));
  Send(connection, Encode("gsea", reply));

  for (const auto& session : available) {
    Fields gam_info;
    gam_info["IDENT"] = std::to_string(session.id);
    gam_info["NAME"] = session.name;
    gam_info["PARAMS"] = session.params;
    gam_info["SYSFLAGS"] = session.sysflags;
    gam_info["COUNT"] = std::to_string(session.players.size());
    gam_info["MAXSIZE"] = std::to_string(session.maxsize);
    Log("lobby -> +gam " + Printable(FormatFields(gam_info)));
    Send(connection, Encode("+gam", gam_info));
  }
}

void Server::HandleGcre(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  GameSession session;
  session.id = next_game_id_++;
  session.name = fields["NAME"].empty() ? connection.player.persona + "'s Game" : fields["NAME"];
  session.host_persona = connection.player.persona;
  session.params = fields["PARAMS"];
  session.sysflags = fields["SYSFLAGS"].empty() ? "0" : fields["SYSFLAGS"];
  session.maxsize = 6;
  session.minsize = 2;
  session.room_id = connection.player.room_id;
  session.start_time = CurrentTimeString();
  session.started = false;

  connection.player.game_id = session.id;
  connection.player.is_host = true;
  connection.player.userflags = "1";
  if (!fields["USERPARAMS"].empty()) {
    connection.player.userparams = fields["USERPARAMS"];
  }

  session.players.push_back(connection.fd);
  games_[session.id] = session;

  Fields reply = FormatGameInfo(session);
  Log("lobby -> gcre " + Printable(FormatFields(reply)));
  Send(connection, Encode("gcre", reply));

  BroadcastToRoom(session.room_id, Encode("+gam", reply), connection.fd);
}

void Server::HandleGjoi(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  std::string ident = fields["IDENT"];
  uint32_t gid = ParseUint(ident, 0);
  auto it = games_.find(gid);
  if (it == games_.end() || it->second.started || it->second.players.size() >= it->second.maxsize) {
    Log("lobby -> gjoiugam (unknown or full game)");
    Send(connection, Encode("gjoiugam", std::string(1, '\0')));
    return;
  }

  GameSession& session = it->second;
  connection.player.game_id = session.id;
  connection.player.is_host = false;
  connection.player.userflags = "0";
  if (!fields["USERPARAMS"].empty()) {
    connection.player.userparams = fields["USERPARAMS"];
  }

  session.players.push_back(connection.fd);

  Fields reply = FormatGameInfo(session);
  Log("lobby -> gjoi " + Printable(FormatFields(reply)));
  Send(connection, Encode("gjoi", reply));

  BroadcastToGame(session.id, Encode("+gam", reply), connection.fd);
}

void Server::HandleGget(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  std::string ident = fields["IDENT"];
  uint32_t gid = ParseUint(ident, 0);
  auto it = games_.find(gid);
  if (it != games_.end()) {
    Fields reply = FormatGameInfo(it->second);
    Log("lobby -> gget " + Printable(FormatFields(reply)));
    Send(connection, Encode("gget", reply));
  } else {
    Send(connection, Encode("gget", std::string(1, '\0')));
  }
}

void Server::HandleGset(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  if (fields.count("USERFLAGS")) {
    connection.player.userflags = fields["USERFLAGS"];
  }
  if (fields.count("USERPARAMS")) {
    connection.player.userparams = fields["USERPARAMS"];
  }

  uint32_t gid = connection.player.game_id;
  auto it = games_.find(gid);
  if (it != games_.end()) {
    GameSession& session = it->second;
    if (fields.count("PARAMS")) {
      session.params = fields["PARAMS"];
    }
    if (fields.count("SYSFLAGS")) {
      session.sysflags = fields["SYSFLAGS"];
    }
    Fields info = FormatGameInfo(session);
    Log("lobby -> gset " + Printable(FormatFields(info)));
    Send(connection, Encode("gset", info));

    BroadcastToGame(session.id, Encode("+gam", info), connection.fd);
    return;
  }

  Send(connection, Encode("gset", std::string(1, '\0')));
}

void Server::HandleGsta(Connection& connection, const Message& /*message*/) {
  uint32_t gid = connection.player.game_id;
  auto it = games_.find(gid);
  if (it == games_.end()) {
    Send(connection, Encode("gsta", std::string(1, '\0')));
    return;
  }

  GameSession& session = it->second;
  session.started = true;

  Log("lobby -> gsta (game started)");
  Send(connection, Encode("gsta", std::string(1, '\0')));

  Fields info = FormatGameInfo(session);
  Log("lobby -> +ses broadcast to all players");
  BroadcastToGame(session.id, Encode("+ses", info), -1);
}

void Server::HandleGlea(Connection& connection, const Message& /*message*/) {
  uint32_t gid = connection.player.game_id;
  auto it = games_.find(gid);
  if (it != games_.end()) {
    GameSession& session = it->second;
    session.players.erase(
        std::remove(session.players.begin(), session.players.end(), connection.fd),
        session.players.end());
    if (session.players.empty() || connection.player.is_host) {
      games_.erase(it);
    } else {
      Fields info = FormatGameInfo(session);
      BroadcastToGame(session.id, Encode("+gam", info), -1);
    }
  }

  connection.player.game_id = 0;
  connection.player.is_host = false;
  connection.player.userflags = "0";

  Log("lobby -> glea");
  Send(connection, Encode("glea", std::string(1, '\0')));
}

void Server::HandleMesg(Connection& connection, const Message& message) {
  Fields fields = ParseFields(message.body);
  Fields msg;
  msg["FROM"] = connection.player.persona;
  msg["TEXT"] = fields["TEXT"];
  msg["TYPE"] = fields["TYPE"].empty() ? "0" : fields["TYPE"];
  BroadcastToRoom(connection.player.room_id, Encode("+msg", msg), connection.fd);
  Send(connection, Encode("mesg", std::string(1, '\0')));
}

Fields Server::FormatGameInfo(const GameSession& session) {
  Fields info;
  info["IDENT"] = std::to_string(session.id);
  info["NAME"] = session.name;
  info["HOST"] = session.host_persona;
  info["PARAMS"] = session.params;
  info["PLATPARAMS"] = "0";
  info["ROOM"] = std::to_string(session.room_id);
  info["CUSTFLAGS"] = "413082880";
  info["SYSFLAGS"] = session.sysflags;
  info["COUNT"] = std::to_string(session.players.size());
  info["PRIV"] = "0";
  info["MINSIZE"] = std::to_string(session.minsize);
  info["MAXSIZE"] = std::to_string(session.maxsize);
  info["NUMPART"] = "1";
  info["SEED"] = "3";
  info["WHEN"] = session.start_time.empty() ? "2006.02.10 00:00:00" : session.start_time;
  info["AUTH"] = "";
  info["SESS"] = "0";
  info["EVID"] = "0";
  info["EVGID"] = "0";

  for (size_t i = 0; i < session.players.size(); ++i) {
    SocketHandle pfd = session.players[i];
    auto it = connections_.find(pfd);
    if (it == connections_.end()) continue;
    const Player& p = it->second->player;
    std::string prefix = std::to_string(i);
    info["OPID" + prefix] = std::to_string(p.id);
    info["OPPO" + prefix] = p.persona;
    std::string ip = p.ip.empty() ? "127.0.0.1" : p.ip;
    info["ADDR" + prefix] = ip;
    info["LADDR" + prefix] = ip;
    info["MADDR" + prefix] = "";
    info["OPPART" + prefix] = "0";
    info["OPPARAM" + prefix] = p.userparams;
    info["OPFLAG" + prefix] = p.userflags;
    info["PRES" + prefix] = "0";
    info["PARTSIZE" + prefix] = std::to_string(session.maxsize);
  }
  return info;
}

void Server::SendWho(Connection& connection) {
  Fields who;
  who["I"] = std::to_string(connection.player.id);
  who["M"] = connection.player.account_name;
  who["N"] = connection.player.persona;
  who["F"] = "U";
  who["P"] = "80";
  who["S"] = ",,,,,,,,,";
  who["X"] = "";
  who["G"] = std::to_string(connection.player.game_id);
  who["AT"] = "";
  who["CL"] = "511";
  who["LV"] = "1049601";
  who["MD"] = "0";
  who["R"] = "1";
  who["US"] = "";
  Log("lobby -> +who " + Printable(FormatFields(who)));
  Send(connection, Encode("+who", who));
}

void Server::SendRoomUpdate(Connection& connection) {
  Fields rom;
  rom["I"] = "1";
  rom["N"] = "LVL.1";
  rom["DN"] = "Revenge Lobby";
  rom["D"] = "Burnout Revenge Lobby";
  rom["F"] = "A";
  rom["T"] = std::to_string(GetRoomPlayerCount(1));
  rom["L"] = "50";
  Log("lobby -> +rom " + Printable(FormatFields(rom)));
  Send(connection, Encode("+rom", rom));
}

void Server::BroadcastToRoom(uint32_t room_id, const std::vector<uint8_t>& bytes, SocketHandle except_fd) {
  for (auto& [fd, conn] : connections_) {
    if (fd != except_fd && conn->role == Role::kLobby && conn->player.room_id == room_id) {
      Send(*conn, bytes);
    }
  }
}

void Server::BroadcastToGame(uint32_t game_id, const std::vector<uint8_t>& bytes, SocketHandle except_fd) {
  auto it = games_.find(game_id);
  if (it == games_.end()) return;
  for (SocketHandle pfd : it->second.players) {
    if (pfd != except_fd) {
      auto cit = connections_.find(pfd);
      if (cit != connections_.end()) {
        Send(*cit->second, bytes);
      }
    }
  }
}

uint32_t Server::GetRoomPlayerCount(uint32_t room_id) const {
  uint32_t count = 0;
  for (const auto& [fd, conn] : connections_) {
    if (conn->role == Role::kLobby && conn->player.room_id == room_id) {
      count++;
    }
  }
  return count > 0 ? count : 1;
}

void Server::OnDisconnect(SocketHandle fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) return;
  Connection& conn = *it->second;
  if (conn.player.game_id != 0) {
    auto git = games_.find(conn.player.game_id);
    if (git != games_.end()) {
      GameSession& session = git->second;
      session.players.erase(
          std::remove(session.players.begin(), session.players.end(), fd),
          session.players.end());
      if (session.players.empty() || conn.player.is_host) {
        games_.erase(git);
      } else {
        Fields info = FormatGameInfo(session);
        BroadcastToGame(session.id, Encode("+gam", info), -1);
      }
    }
  }
}

}  // namespace ealobby
