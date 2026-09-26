// A stand-in for EA's Burnout Revenge lobby servers, which are long gone.
//
// The title first asks a directory server where the lobby is (@dir, on the
// port it is built with, 31860), then connects to the lobby itself. This
// serves both, on its own thread. It is built up from what the title actually
// sends: a command it does not handle yet is logged and answered with an empty
// success, so every run shows the next thing the title wants.
//
// It runs standalone (tools/ealobby, for a server of your own) or inside the
// game, for two machines on a LAN that have no server between them.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ealobby/aries.h"

namespace ealobby {

// A socket descriptor, wide enough for both a POSIX fd (int) and a Windows
// SOCKET (an unsigned, pointer-sized handle); -1 means none/closed on both.
using SocketHandle = std::intptr_t;

struct Options {
  std::string host = "0.0.0.0";
  uint16_t directory_port = 31860;
  uint16_t lobby_port = 31861;
  // Where @dir sends clients. Empty: the address they reached the directory at.
  std::string advertise;
};

class Server {
 public:
  using LogFunction = std::function<void(const std::string&)>;

  explicit Server(Options options, LogFunction log);
  ~Server();

  // Opens both ports and starts serving. False (with the reason logged) when
  // a port cannot be opened - most often because another server already has it.
  bool Start();
  void Stop();

 private:
  enum class Role { kDirectory, kLobby };

  struct Connection {
    SocketHandle fd = -1;
    Role role = Role::kLobby;
    std::string peer;
    std::string local_address;  // the address the client reached us at
    std::vector<uint8_t> in;
    std::vector<uint8_t> out;
  };

  SocketHandle Listen(uint16_t port);
  void Run();
  void Accept(SocketHandle listener, Role role);
  void Receive(Connection& connection);
  void Flush(Connection& connection);
  void Handle(Connection& connection, const Message& message);
  void HandleDirectory(Connection& connection, const Message& message);
  void HandleLobby(Connection& connection, const Message& message);
  void Send(Connection& connection, const std::vector<uint8_t>& bytes);
  void Log(const std::string& line) const;

  Options options_;
  LogFunction log_;
  SocketHandle directory_listener_ = -1;
  SocketHandle lobby_listener_ = -1;
  // A pipe (POSIX) or a loopback socket pair (Windows) to interrupt poll()/
  // WSAPoll() when stopping.
  SocketHandle wake_[2] = {-1, -1};
  std::map<SocketHandle, std::unique_ptr<Connection>> connections_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  uint32_t next_session_ = 100000;
};

}  // namespace ealobby
