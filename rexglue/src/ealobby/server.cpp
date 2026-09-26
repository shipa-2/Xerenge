#include "ealobby/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>

namespace ealobby {
namespace {

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

void SetNonBlocking(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
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

int Server::Listen(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    Log("ealobby: socket: " + std::string(std::strerror(errno)));
    return -1;
  }
  const int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
    Log("ealobby: bad listen address " + options_.host);
    close(fd);
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(fd, 16) < 0) {
    Log("ealobby: cannot listen on " + options_.host + ":" + std::to_string(port) + ": " +
        std::strerror(errno));
    close(fd);
    return -1;
  }
  SetNonBlocking(fd);
  return fd;
}

bool Server::Start() {
  if (running_) {
    return true;
  }
  directory_listener_ = Listen(options_.directory_port);
  lobby_listener_ = Listen(options_.lobby_port);
  if (directory_listener_ < 0 || lobby_listener_ < 0 || pipe(wake_) < 0) {
    Stop();
    return false;
  }
  SetNonBlocking(wake_[0]);
  running_ = true;
  thread_ = std::thread([this] { Run(); });
  Log("ealobby: directory on " + options_.host + ":" + std::to_string(options_.directory_port) +
      ", lobby on port " + std::to_string(options_.lobby_port));
  return true;
}

void Server::Stop() {
  if (running_.exchange(false) && wake_[1] >= 0) {
    const char byte = 0;
    [[maybe_unused]] ssize_t written = write(wake_[1], &byte, 1);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  for (auto& [fd, connection] : connections_) {
    close(fd);
  }
  connections_.clear();
  for (int* fd : {&directory_listener_, &lobby_listener_, &wake_[0], &wake_[1]}) {
    if (*fd >= 0) {
      close(*fd);
      *fd = -1;
    }
  }
}

void Server::Run() {
  while (running_) {
    std::vector<pollfd> fds;
    fds.push_back({wake_[0], POLLIN, 0});
    fds.push_back({directory_listener_, POLLIN, 0});
    fds.push_back({lobby_listener_, POLLIN, 0});
    for (auto& [fd, connection] : connections_) {
      fds.push_back({fd, short(POLLIN | (connection->out.empty() ? 0 : POLLOUT)), 0});
    }
    if (poll(fds.data(), fds.size(), 1000) < 0) {
      if (errno == EINTR) {
        continue;
      }
      Log("ealobby: poll: " + std::string(std::strerror(errno)));
      break;
    }
    if (fds[1].revents & POLLIN) {
      Accept(directory_listener_, Role::kDirectory);
    }
    if (fds[2].revents & POLLIN) {
      Accept(lobby_listener_, Role::kLobby);
    }
    std::vector<int> closed;
    for (size_t i = 3; i < fds.size(); ++i) {
      auto found = connections_.find(fds[i].fd);
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
        closed.push_back(fds[i].fd);
      }
    }
    for (int fd : closed) {
      connections_.erase(fd);
    }
  }
}

void Server::Accept(int listener, Role role) {
  for (;;) {
    sockaddr_in peer = {};
    socklen_t peer_length = sizeof(peer);
    const int fd = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (fd < 0) {
      return;
    }
    SetNonBlocking(fd);
    const int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    auto connection = std::make_unique<Connection>();
    connection->fd = fd;
    connection->role = role;
    connection->peer = AddressString(peer) + ":" + std::to_string(ntohs(peer.sin_port));
    sockaddr_in local = {};
    socklen_t local_length = sizeof(local);
    getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length);
    connection->local_address = AddressString(local);
    Log(std::string(role == Role::kDirectory ? "directory" : "lobby") + ": connection from " +
        connection->peer);
    connections_[fd] = std::move(connection);
  }
}

void Server::Receive(Connection& connection) {
  uint8_t chunk[4096];
  for (;;) {
    const ssize_t received = recv(connection.fd, chunk, sizeof(chunk), 0);
    if (received > 0) {
      connection.in.insert(connection.in.end(), chunk, chunk + received);
      continue;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    Log(std::string(connection.role == Role::kDirectory ? "directory" : "lobby") + ": " +
        connection.peer + " closed");
    close(connection.fd);
    connection.fd = -1;
    return;
  }
  Message message;
  while (connection.fd >= 0 && TakeMessage(&connection.in, &message)) {
    Handle(connection, message);
  }
}

void Server::Flush(Connection& connection) {
  while (!connection.out.empty()) {
    const ssize_t sent = send(connection.fd, connection.out.data(), connection.out.size(),
                              MSG_NOSIGNAL);
    if (sent <= 0) {
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      close(connection.fd);
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
    Log("lobby -> news " + Printable(FormatFields(reply)));
    Send(connection, Encode("news", reply));
    return;
  }
  // Not handled yet: an empty success, to see what comes next.
  Log("lobby: " + Printable(message.command) + " not handled yet; answered empty");
  Send(connection, Encode(message.command, std::string(1, '\0')));
}

}  // namespace ealobby
