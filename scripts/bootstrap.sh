#!/bin/sh
# Everything from a disc image to a running game, in one command.
#
#   ./scripts/bootstrap.sh <image.iso> [game directory]
#
# The image is checked against the hash of the retail European release before
# anything else: an image that differs is not necessarily broken, but the
# function boundaries in functions.toml were found in this one, and a different
# build will not line up with them.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
image=$1
game=${2:-$root/game}

RETAIL_SHA256=34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4

if [ -z "$image" ]; then
    echo "give the path to a disc image" >&2
    echo "  $0 <image.iso> [game directory]" >&2
    exit 1
fi

echo "== checking the environment"
"$root/scripts/check-prerequisites.sh"

echo
echo "== extracting the image into $game"
if [ -f "$game/default.xex" ]; then
    echo "already extracted, skipping"
else
    python3 "$root/scripts/extract-image.py" "$image" "$game" --sha256 "$RETAIL_SHA256"
fi

echo
echo "== SDK"
"$root/scripts/setup-sdk.sh"

echo
echo "== building the title"
"$root/scripts/build.sh" --game "$game"

echo
echo "All set. To run:"
echo "  cd $root/xerenge-rex && ./run.sh"
