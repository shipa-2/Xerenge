#!/bin/sh
# Builds the title. Code generation runs as part of the build: the recompiler
# recovers the whole retail image by itself in a few seconds, taking the handful
# of function boundaries it cannot see - the ones reached only through a vtable
# slot - from functions.toml.
#
#   ./scripts/build.sh [--game <game directory>]
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
project="$root/xerenge-rex"
sdk="$root/rexglue-sdk"
game=

while [ $# -gt 0 ]; do
    case "$1" in
        --game) game=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ ! -d "$sdk" ]; then
    echo "no SDK at $sdk - run ./scripts/setup-sdk.sh first" >&2
    exit 1
fi

if [ -n "$game" ]; then
    game=$(cd "$game" && pwd)
    if [ ! -f "$game/default.xex" ]; then
        echo "$game holds no default.xex" >&2
        exit 1
    fi
    # The manifest names the image and the data directory; point both at this
    # copy of the game rather than leaving a path from another machine.
    python3 - "$project/burnout_manifest.toml" "$game" <<'PY'
import re, sys
path, game = sys.argv[1], sys.argv[2]
text = open(path).read()
text = re.sub(r'(?m)^game_root = .*$', 'game_root = "%s"' % game, text)
text = re.sub(r'(?m)^file_path = .*$', 'file_path = "%s/default.xex"' % game, text)
open(path, 'w').write(text)
PY
    echo "== the manifest now points at $game"
fi

echo "== configuring"
# Two flags are not optional and are not in the SDK's own project template:
# the SSSE3 baseline, because the SDK's memory code uses those intrinsics while
# a generated project sets no architecture, and clang, which is what this has
# been built and run with.
cmake -S "$project" -B "$project/build" \
      -DREXSDK_DIR="$sdk" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2

echo "== building"
cmake --build "$project/build" -j"$(nproc)"

echo
echo "done: $project/build/burnout"
echo "to run: $project/run.sh"
