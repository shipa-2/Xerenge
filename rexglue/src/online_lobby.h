#pragma once

namespace xerenge::online {

// With --online and no --lobby_server: starts this copy's lobby server and its
// LAN beacon. Called when the game starts; a no-op otherwise, and when called
// again.
void StartLobbyNetwork();

}  // namespace xerenge::online
