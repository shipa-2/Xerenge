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

#include <cerrno>
#include <cstdio>
#include <cstring>
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
      if (Connection* gone = ConnectionFor(fd)) {
        LeaveGame(*gone);
      }
      connections_.erase(fd);
    }
    // DirtySock drops a lobby connection it has heard nothing on for 60 s
    // (LobbyApi ref+0x34, renewed by every message). EA's server kept it
    // alive with '~png', which the client answers in kind.
    const auto now = std::chrono::steady_clock::now();
    for (auto& [fd, connection] : connections_) {
      if (connection->role == Role::kLobby && connection->id != 0 &&
          now - connection->last_sent >= std::chrono::seconds(20)) {
        Send(*connection, Encode("~png", Fields{{"REF", std::to_string(next_ping_++)}}));
      }
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
  connection.last_sent = std::chrono::steady_clock::now();
  connection.out.insert(connection.out.end(), bytes.begin(), bytes.end());
  Flush(connection);
}

void Server::Handle(Connection& connection, const Message& message) {
  if (message.command == "~png") {
    // The answer to our keepalive (or the client's own): nothing to say back,
    // and every 20 s is too often to log.
    return;
  }
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
  // The client as its peers see it: what it said in 'addr', else where its
  // connection comes from.
  const auto PeerAddress = [](const Connection& c) {
    return !c.address.empty() ? c.address : c.peer.substr(0, c.peer.find(':'));
  };
  if (message.command == "addr") {
    // The client's own address and port, as it sees them. Kept: it is the
    // address its peers reach it at, which the connection's own source (on
    // one machine, 127.0.0.1) is not.
    const Fields asked = ParseFields(message.body);
    const auto addr = asked.find("ADDR");
    if (addr != asked.end() && !addr->second.empty()) {
      connection.address = addr->second;
    }
    Send(connection, Encode("addr", std::string(1, '\0')));
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
    // The client files the reply by its header code, 'new' and the NAME asked
    // for (sub_8240EAD8 checks for 'new7'); without it the configuration is
    // dropped and the login waits at the connect step for good.
    const Fields asked = ParseFields(message.body);
    const auto name = asked.find("NAME");
    const std::string code =
        "new" + (name != asked.end() && !name->second.empty() ? name->second.substr(0, 1)
                                                              : std::string("7"));
    Log("lobby -> news/" + code + " " + Printable(FormatFields(reply)));
    Send(connection, Encode("news", reply, code));
    return;
  }

  // The login, from what the client code reads back (retail strings near
  // 0x82052880 and 0x820469A0). Until each reply's exact shape is known from
  // a run, a reply repeats what was asked - the client finds its own fields
  // where it expects them - and adds the ones it reads.
  const Fields asked = ParseFields(message.body);
  Log("lobby <- " + message.command + " " + Printable(message.body));
  const auto reply_with = [&](Fields reply) {
    Log("lobby -> " + message.command + " " + Printable(FormatFields(reply)));
    Send(connection, Encode(message.command, reply));
  };
  const auto field = [&](const char* key, const std::string& fallback) {
    const auto it = asked.find(key);
    return it != asked.end() && !it->second.empty() ? it->second : fallback;
  };

  if (message.command == "sele") {
    // What the client subscribes to (INGAME, MESGS, USERS, GAMES, ROOMS, ...).
    // The dispatcher reads MORE, SLOTS and STATS back.
    Fields reply = asked;
    reply["MORE"] = "0";
    reply["SLOTS"] = "4";
    reply.emplace("STATS", "0");
    reply_with(reply);
    return;
  }
  if (message.command == "auth" || message.command == "acct") {
    // Logging in as the Xbox Live gamertag. PERSONAS lists the names the
    // account plays as; LKEY is the key later requests (and EA's web
    // services) carry.
    const std::string name = field("GTAG", field("NAME", field("PERS", "Player")));
    connection.user = name;
    connection.maddr = field("MADDR", "");
    connection.xuid = field("XUID", "");
    Fields reply;
    for (const auto& [key, value] : asked) {
      if (key != "PASS") {
        reply[key] = value;
      }
    }
    reply["NAME"] = name;
    reply["PERSONAS"] = name;
    reply["LKEY"] = RandomHex(16);
    reply["TOS"] = "1";
    reply["SHARE"] = "1";
    reply["SPAM"] = "NN";
    reply["BORN"] = "19800101";
    reply["GEND"] = "M";
    reply["MAIL"] = "player@xerenge.local";
    reply["LAST"] = "2006.2.10-0:00:00";
    reply["ADDR"] = PeerAddress(connection);
    reply_with(reply);
    return;
  }
  if (message.command == "pers") {
    // Choosing the persona to play as.
    const std::string name = field("PERS", field("NAME", connection.user.empty() ? "Player"
                                                                                  : connection.user));
    connection.user = name;
    Fields reply = asked;
    reply["PERS"] = name;
    reply["NAME"] = name;
    reply["LKEY"] = RandomHex(16);
    reply["LAST"] = "2006.2.10-0:00:00";
    reply["PLAST"] = "2006.2.10-0:00:00";
    reply["ADDR"] = PeerAddress(connection);
    reply_with(reply);

    // '+who': the user's own record, which the client files as 'self'
    // (handler 0x8241C4C8, LobbyApiExtractUserRecord). Until it arrives the
    // login's last request stays in flight - CGtLobbyDirtySock's rejoin
    // step (0x823719B8) waits for self's I, then reads G, the game the user
    // was in (0: none) - and the lobby reports itself busy for good.
    if (connection.id == 0) {
      connection.id = next_user_++;
    }
    SendWho(connection);
    return;
  }
  if (message.command == "cate") {
    // The ranking categories (DirtySock's LobbyRank, reply parsed at
    // 0x824066D0). CC, IC and VC (each defaulting to 1) size its tables;
    // then R, comma separated: per category "name,indices", per index
    // "name,variations", per variation "name,n,columns" and that many column
    // values. The category's and the index's counts drive the parser's loop
    // (0 underflows it). Only once R has been read to the end is the fetch
    // reported done - short of that it stays in flight and the lobby busy
    // for good. One empty ranking table.
    Fields reply;
    reply["CC"] = "1";
    reply["IC"] = "1";
    reply["VC"] = "1";
    reply["R"] = "RANKED,1,ALL,1,DEFAULT,0,0";
    reply_with(reply);
    return;
  }
  if (message.command == "onln" || message.command == "user") {
    // Who is online / a user's details: this lobby knows only who is
    // connected to it.
    Fields reply = asked;
    reply["NAME"] = field("PERS", field("NAME", connection.user));
    reply["ADDR"] = PeerAddress(connection);
    reply_with(reply);
    return;
  }
  // Games. A game is created (gcre), found (gsea), joined (gjoi), changed
  // (gset), started (gsta) and left (glea); every player in it is sent the
  // whole record again as '+mgm' whenever it changes, which is how the title
  // (CGtLobbyDirtySock's event callback, 0x8236C700, event 'game') learns who
  // is in its game and who hosts it - it finds itself among OPPO%d by name.
  // '+ses' (event 'play') starts the session.
  if (message.command == "gcre") {
    LeaveGame(connection);
    Game game;
    game.id = next_game_++;
    game.host = connection.user.empty() ? "Player" : connection.user;
    game.name = field("NAME", game.host);
    game.params = field("PARAMS", "");
    game.room = field("ROOM", "0");
    game.custflags = field("CUSTFLAGS", "0");
    game.sysflags = field("SYSFLAGS", "0");
    game.minsize = field("MINSIZE", "2");
    game.maxsize = field("MAXSIZE", "2");
    game.seed = uint32_t(std::stoul(RandomHex(4), nullptr, 16));
    game.players.push_back(connection.fd);
    connection.game = game.id;
    connection.user_params = field("USERPARAMS", "");
    const Game& stored = games_[game.id] = std::move(game);
    reply_with(GameRecord(stored, connection));
    SendGameToPlayers(stored);
    SendWho(connection);
    return;
  }
  if (message.command == "gjoi") {
    // By IDENT, or by the host's name (NAME/USER).
    Game* game = nullptr;
    const std::string ident = field("IDENT", "");
    const std::string by_name = field("NAME", field("USER", ""));
    for (auto& [id, g] : games_) {
      if ((!ident.empty() && std::to_string(id) == ident) ||
          (ident.empty() && !by_name.empty() && (g.name == by_name || g.host == by_name))) {
        game = &g;
        break;
      }
    }
    if (!game) {
      Log("lobby: gjoi - no such game");
      Send(connection, Encode(message.command, Fields{}, "gjnf"));
      return;
    }
    if (connection.game != game->id) {
      LeaveGame(connection);
      game->players.push_back(connection.fd);
      connection.game = game->id;
    }
    connection.user_params = field("USERPARAMS", connection.user_params);
    reply_with(GameRecord(*game, connection));
    SendGameToPlayers(*game);
    SendWho(connection);
    return;
  }
  if (message.command == "gqwk") {
    // Quick match: the first open game with room, or none.
    Game* game = nullptr;
    for (auto& [id, g] : games_) {
      if (id != connection.game && std::stoi(g.maxsize.empty() ? "0" : g.maxsize) >
                                       static_cast<int>(g.players.size())) {
        game = &g;
        break;
      }
    }
    if (!game) {
      Log("lobby: gqwk - no open game");
      Send(connection, Encode(message.command, Fields{}, "gjnf"));
      return;
    }
    LeaveGame(connection);
    game->players.push_back(connection.fd);
    connection.game = game->id;
    connection.user_params = field("USERPARAMS", connection.user_params);
    reply_with(GameRecord(*game, connection));
    SendGameToPlayers(*game);
    SendWho(connection);
    return;
  }
  if (message.command == "glea" || message.command == "gdel") {
    reply_with(Fields{});
    LeaveGame(connection);
    Send(connection, Encode("+mgm", Fields{{"IDENT", "0"}, {"NAME", ""}, {"HOST", ""}, {"COUNT", "0"}}));
    SendWho(connection);
    return;
  }
  if (message.command == "gset") {
    auto it = games_.find(connection.game);
    if (it == games_.end()) {
      reply_with(Fields{});
      return;
    }
    Game& game = it->second;
    game.name = field("NAME", game.name);
    game.params = field("PARAMS", game.params);
    game.custflags = field("CUSTFLAGS", game.custflags);
    game.sysflags = field("SYSFLAGS", game.sysflags);
    game.minsize = field("MINSIZE", game.minsize);
    game.maxsize = field("MAXSIZE", game.maxsize);
    game.session = field("SESS", game.session);
    connection.user_params = field("USERPARAMS", connection.user_params);
    reply_with(GameRecord(game, connection));
    SendGameToPlayers(game);
    return;
  }
  if (message.command == "gsta") {
    auto it = games_.find(connection.game);
    reply_with(it != games_.end() ? GameRecord(it->second, connection) : Fields{});
    if (it != games_.end()) {
      for (SocketHandle fd : it->second.players) {
        if (Connection* player = ConnectionFor(fd)) {
          const Fields record = GameRecord(it->second, *player);
          Log("lobby -> +ses to " + player->user + " " + Printable(FormatFields(record)));
          Send(*player, Encode("+ses", record));
        }
      }
    }
    return;
  }
  if (message.command == "gsea") {
    // Every open game, one '+gam' each, after the reply says how many.
    Fields reply;
    reply["COUNT"] = std::to_string(games_.size());
    reply_with(reply);
    for (const auto& [id, game] : games_) {
      const Fields record = GameRecord(game, connection);
      Log("lobby -> +gam " + Printable(FormatFields(record)));
      Send(connection, Encode("+gam", record));
    }
    return;
  }
  // Not handled yet: an empty success, to see what comes next.
  Log("lobby: " + Printable(message.command) + " not handled yet; answered empty");
  Send(connection, Encode(message.command, std::string(1, '\0')));
}


Server::Connection* Server::ConnectionFor(SocketHandle fd) const {
  auto it = connections_.find(fd);
  return it != connections_.end() ? it->second.get() : nullptr;
}

Fields Server::GameRecord(const Game& game, const Connection& to) const {
  const auto address = [](const Connection& c) {
    return !c.address.empty() ? c.address : c.peer.substr(0, c.peer.find(':'));
  };
  Fields record;
  record["IDENT"] = std::to_string(game.id);
  record["NAME"] = game.name;
  record["HOST"] = game.host;
  record["SELF"] = to.user;
  record["PARAMS"] = game.params;
  record["PLATPARAMS"] = "";
  record["ROOM"] = game.room;
  record["CUSTFLAGS"] = game.custflags;
  record["SYSFLAGS"] = game.sysflags;
  record["PRIV"] = "0";
  record["MINSIZE"] = game.minsize;
  record["MAXSIZE"] = game.maxsize;
  record["SEED"] = std::to_string(game.seed);
  record["WHEN"] = "2006.2.10-0:00:00";
  record["AUTH"] = "";
  if (!game.session.empty()) {
    record["SESS"] = game.session;
  }
  uint32_t count = 0;
  for (SocketHandle fd : game.players) {
    const Connection* player = ConnectionFor(fd);
    if (!player) {
      continue;
    }
    const std::string n = std::to_string(count++);
    record["OPID" + n] = std::to_string(player->id);
    record["OPPO" + n] = player->user;
    record["ADDR" + n] = address(*player);
    record["LADDR" + n] = address(*player);
    record["MADDR" + n] = player->maddr;
    record["OPPART" + n] = "0";
    record["OPPARAM" + n] = player->user_params;
    record["OPFLAG" + n] = "0";
    record["PRES" + n] = "0";
  }
  record["COUNT"] = std::to_string(count);
  record["NUMPART"] = std::to_string(count);
  return record;
}

void Server::SendGameToPlayers(const Game& game) {
  for (SocketHandle fd : game.players) {
    if (Connection* player = ConnectionFor(fd)) {
      const Fields record = GameRecord(game, *player);
      Log("lobby -> +mgm to " + player->user + " " + Printable(FormatFields(record)));
      Send(*player, Encode("+mgm", record));
    }
  }
}

void Server::LeaveGame(Connection& connection) {
  auto it = games_.find(connection.game);
  connection.game = 0;
  if (it == games_.end()) {
    return;
  }
  Game& game = it->second;
  std::erase(game.players, connection.fd);
  if (game.players.empty()) {
    Log("lobby: game " + std::to_string(game.id) + " closed");
    games_.erase(it);
    return;
  }
  // The host gone, the next player hosts.
  if (game.host == connection.user) {
    if (const Connection* next = ConnectionFor(game.players.front())) {
      game.host = next->user;
    }
  }
  SendGameToPlayers(game);
}

void Server::SendWho(Connection& connection) {
  const std::string address =
      !connection.address.empty() ? connection.address
                                  : connection.peer.substr(0, connection.peer.find(':'));
  Fields who;
  who["I"] = std::to_string(connection.id);
  who["N"] = connection.user;
  who["M"] = connection.user;
  who["F"] = "0";
  who["A"] = address;
  who["LA"] = address;
  who["P"] = "0";
  who["S"] = "";
  // X: the XUID, "$" and hex, as the client sent it.
  who["X"] = connection.xuid;
  // MA: its Xbox Live address as it gave it in MADDR ("$" XUID "^" XNADDR). The
  // session code takes the console's own XUID from it (ConnApiOnline
  // 0x82587D00, self+0x1E0) and compares it with the host's MADDR to tell
  // that it hosts a game it created; empty, it never did, and left the game.
  who["MA"] = connection.maddr;
  // G: the game the user is in. Once in one, the title checks it every tick
  // (0x8236D2D8) and after 200 with G still 0 gives the game up -
  // "$NetworkingNoGame", "the game you were in no longer exists".
  who["G"] = std::to_string(connection.game);
  who["AT"] = "";
  who["CL"] = "0";
  who["LV"] = "0";
  who["MD"] = "0";
  who["HW"] = "0";
  who["RP"] = "0";
  Log("lobby -> +who " + Printable(FormatFields(who)));
  Send(connection, Encode("+who", who));
}

}  // namespace ealobby
