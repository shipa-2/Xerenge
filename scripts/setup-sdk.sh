#!/bin/sh
# Fetches and builds the recompiler and system layer.
#
# The SDK is a separate checkout rather than vendored, and it is our fork: it
# carries fixes this title needs, including the vector pack defect that made the
# whole frontend draw blue. Upstream stays a remote, so the changes remain
# rebasable onto it.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
. "$root/scripts/platform.sh"
sdk="$root/rexglue-sdk"
# HTTPS, so that no SSH key is needed to fetch it.
fork=${XERENGE_SDK_REMOTE:-https://github.com/shipa-2/rexglue-xerenge.git}

if [ ! -d "$sdk/.git" ]; then
    echo "== fetching the SDK into $sdk"
    git clone "$fork" "$sdk"
    git -C "$sdk" remote add upstream https://github.com/rexglue/rexglue-sdk 2>/dev/null || true
else
    echo "== the SDK is already there: $sdk"
fi

echo "== submodules"
git -C "$sdk" submodule update --init --recursive --depth 1

echo "== building the SDK"
cmake --preset $XR_PRESET -S "$sdk"
cmake --build "$sdk/out/build/$XR_PRESET" --config Release --target rexglue -j"$XR_JOBS"

echo
echo "done. Code generator: $sdk/out/$XR_PRESET/rexglue$XR_EXE"
echo "Runtime libraries:  $sdk/out/$XR_PRESET"
