#!/bin/sh
# A lobby server of your own for the online mode (a stand-in for EA's, which are gone).
#
#   ./lobby.sh [--host A] [--directory-port P] [--lobby-port P] [--advertise A]
#
# It needs a C++17 compiler and nothing else; the server is three source files and is built on the first run
# into build-lobby/. Players point their game at it with `./run.sh --lobby-server=<this machine's address>`
# (ports 31860 and 31861 TCP must be reachable). While it runs, the games do not start lobbies of their own:
# every game on it is found by every player, and it keeps running when a player quits.
set -e
cd "$(dirname "$0")"
out=build-lobby/ealobby
if [ ! -x "$out" ] || [ src/ealobby/server.cpp -nt "$out" ] || [ src/ealobby/server.h -nt "$out" ] ||
   [ src/ealobby/aries.cpp -nt "$out" ] || [ src/ealobby/aries.h -nt "$out" ] || [ tools/ealobby/main.cpp -nt "$out" ]; then
    mkdir -p build-lobby
    echo "building the lobby server..." >&2
    ${CXX:-c++} -std=c++17 -O2 -pthread -Isrc \
        tools/ealobby/main.cpp src/ealobby/aries.cpp src/ealobby/server.cpp -o "$out"
fi
exec "$out" "$@"
