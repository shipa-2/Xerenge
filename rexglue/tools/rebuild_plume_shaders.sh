#!/bin/sh
# Decrypt default.xex, scan containers with XenosRecomp, emit generated/shader_cache.cpp.
set -e
cd "$(dirname "$0")/.."

# The game the manifest names, unless one is given.
GAME_ROOT=$(sed -n 's/^game_root *= *"\(.*\)"/\1/p' burnout_manifest.toml | head -1)
XEX=${1:-$GAME_ROOT/default.xex}
# XenosRecomp beside the project in the repository (../XenosRecomp), or beside
# a working copy kept elsewhere (../Xerenge/XenosRecomp).
for candidate in ../XenosRecomp ../Xerenge/XenosRecomp; do
    [ -d "$candidate/XenosRecomp" ] && XENOS_ROOT=$candidate && break
done
XENOS_ROOT=${XENOS_ROOT:-../XenosRecomp}
RECOMP=${XENOSRECOMP:-$XENOS_ROOT/build/XenosRecomp/XenosRecomp}
HEADER=${XENOSRECOMP_HEADER:-$XENOS_ROOT/XenosRecomp/shader_common.h}
SCAN=generated/xenos-scan
OUT=generated/shader_cache.cpp

if [ ! -x "$RECOMP" ]; then
    echo "XenosRecomp not found at $RECOMP" >&2
    exit 1
fi

tools/dump_xex_image "$XEX" -o "$SCAN/guest-image.bin"
mkdir -p generated/xenos-hlsl
XENOS_RECOMP_DUMP_SOURCE_DIR="$(pwd)/generated/xenos-hlsl" \
    "$RECOMP" "$SCAN" "$OUT" "$HEADER"
echo "shader cache: $OUT"
