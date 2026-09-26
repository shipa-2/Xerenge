#!/bin/sh
# Translate the shaders the static XEX scan cannot see.
#
# Some of this title's shaders are assembled at runtime, so they exist in the
# executable only as the code that builds them - rebuild_plume_shaders.sh scans
# the guest image and never finds them, and every draw using one falls back to
# the passthrough shader. The renderer reports them as
#   plume: IM_LOAD VS ucode=<hash> cache MISS
# and dumps both the microcode and the container declarations derived from it
# into generated/ucode-dump/ (<TYPE>_<hash>_<size>.bin plus .wrapargs).
#
# This wraps each of those dumps in a container, runs XenosRecomp over them and
# emits a cache the build can link alongside the scanned one. Run the game
# first, reaching the screens whose geometry is missing, so the dumps exist.
#
# With --from-disc it needs no dumps: the containers are rebuilt from the disc
# by tools/runtime_shaders.recipe (see build_runtime_shaders), which is what
# a fresh installation does. make_runtime_shader_recipe refreshes the recipe
# from a set of dumps.
#
# Every shader the game loads is dumped and described, not only the ones that
# missed, so the result supersedes generated/shader_cache_bootstrap.cpp rather
# than having to be linked beside it. That older cache was itself built from
# hand-wrapped captures whose containers were not kept, which is why it could
# not be extended - the point of describing everything is that this one can be
# rebuilt from any run.
set -e
cd "$(dirname "$0")/.."

# XenosRecomp beside the project in the repository (../XenosRecomp), or beside
# a working copy kept elsewhere (../Xerenge/XenosRecomp).
for candidate in ../XenosRecomp ../Xerenge/XenosRecomp; do
    [ -d "$candidate/XenosRecomp" ] && XENOS_ROOT=$candidate && break
done
XENOS_ROOT=${XENOS_ROOT:-../XenosRecomp}
RECOMP=${XENOSRECOMP:-$XENOS_ROOT/build/XenosRecomp/XenosRecomp}
HEADER=${XENOSRECOMP_HEADER:-$XENOS_ROOT/XenosRecomp/shader_common.h}
# Our wrappers, not XenosRecomp's: the stock vertex one cannot declare
# samplers and the stock pixel one cannot declare interpolators, and shaders
# here need both. Output is otherwise byte-identical to the stock layout.
VS_WRAP=${XENOSRECOMP_VS_WRAP:-tools/wrap_runtime_vertex_shader}
PS_WRAP=tools/wrap_runtime_pixel_shader
RENAME=${XENOSRECOMP_RENAME:-tools/prepare_aux_shader_cache}

DUMP=generated/ucode-dump
IMAGE=${XERENGE_GUEST_IMAGE:-generated/xenos-scan/guest-image.bin}
CONTAINERS=generated/runtime-containers
RAW=generated/shader_cache_runtime_raw.cpp
OUT=generated/shader_cache_runtime.cpp

FROM_DISC=0
[ "$1" = "--from-disc" ] && FROM_DISC=1

for tool in "$RECOMP" ; do
    [ -x "$tool" ] || { echo "XenosRecomp not found at $tool" >&2; exit 1; }
done
[ -f "$VS_WRAP" ] || { echo "vertex wrapper not found at $VS_WRAP" >&2; exit 1; }

rm -rf "$CONTAINERS"
mkdir -p "$CONTAINERS"

if [ "$FROM_DISC" = 1 ]; then
    GAME_ROOT=$(sed -n 's/^game_root *= *"\(.*\)"/\1/p' burnout_manifest.toml | head -1)
    tools/build_runtime_shaders tools/runtime_shaders.recipe "$IMAGE" \
        "${XERENGE_GAME_ROOT:-$GAME_ROOT}" "$CONTAINERS"
    wrapped=$(ls "$CONTAINERS" | wc -l)
    rehosted=$wrapped
    args=
else
    args=$(ls "$DUMP"/*.wrapargs 2>/dev/null || true)
fi
if [ "$FROM_DISC" = 0 ] && [ -z "$args" ]; then
    echo "no .wrapargs in $DUMP - run the game and reach the screens that are" >&2
    echo "missing geometry, so the untranslated shaders get dumped" >&2
    exit 1
fi

if [ "$FROM_DISC" = 0 ]; then
wrapped=0
rehosted=0
fi
for argfile in $args; do
    base=$(basename "$argfile" .wrapargs)
    micro="$DUMP/$base.bin"
    if [ ! -f "$micro" ]; then
        echo "skipping $base: no microcode beside its arguments" >&2
        continue
    fi

    # Most of these shaders are the game's own, with a couple of vertex fetch
    # instructions patched in at load time. When the template can be found in
    # the image, reuse its container: the declarations and constant names then
    # come from the game rather than from anything reconstructed here.
    if [ -f "$IMAGE" ] && \
       tools/rehost_runtime_shader "$IMAGE" "$micro" \
               "$CONTAINERS/$base.container" 2>/dev/null; then
        rehosted=$((rehosted + 1))
        wrapped=$((wrapped + 1))
        continue
    fi

    # No template - the shader really was built from nothing, so fall back to
    # a container described by what the microcode itself reveals.
    decl=$(grep -v '^#' "$argfile" | grep -v '^[[:space:]]*$' | head -1)
    case "$base" in
        VS_*) wrapper="$VS_WRAP" ;;
        PS_*) wrapper="$PS_WRAP" ;;
        *) echo "skipping $base: cannot tell vertex from pixel" >&2; continue ;;
    esac
    # shellcheck disable=SC2086 # decl is a deliberate argument list
    "$wrapper" "$micro" "$CONTAINERS/$base.container" $decl
    echo "wrapped $base (derived declarations)"
    wrapped=$((wrapped + 1))
done

[ "$wrapped" -gt 0 ] || { echo "nothing wrapped" >&2; exit 1; }

# Dump the generated HLSL as well: the runtime reads it back to learn which
# input locations each shader actually consumes, which is what lets guest
# vertex data be placed where the translated shader expects it.
XENOS_RECOMP_DUMP_SOURCE_DIR="$(pwd)/generated/xenos-hlsl" \
    "$RECOMP" "$CONTAINERS" "$RAW" "$HEADER"
"$RENAME" "$RAW" "$OUT" --suffix Bootstrap

echo
echo "wrote $OUT ($wrapped shader(s): $rehosted from their own template, \
$((wrapped - rehosted)) from derived declarations)"

# The bootstrap slot is the one for runtime-assembled shaders, and this cache
# is meant to take it over completely - so say plainly if it covers less than
# what is already linked there, because that would lose shaders.
old=generated/shader_cache_bootstrap.cpp
if [ -f "$old" ]; then
    old_n=$(grep -coE '0x[0-9A-Fa-f]{16}' "$old" || true)
    new_n=$(grep -coE '0x[0-9A-Fa-f]{16}' "$OUT" || true)
    echo "existing bootstrap cache mentions $old_n hashes, this one $new_n"
    echo "compare before replacing it - a shader only in the old one is lost:"
    echo "  comm -23 <(grep -oE '0x[0-9A-Fa-f]{16}' $old | sort -u) \\"
    echo "           <(grep -oE '0x[0-9A-Fa-f]{16}' $OUT | sort -u)"
fi
echo
echo "link it by configuring with:"
echo "  -DXERENGE_SHADER_CACHE_BOOTSTRAP_CPP=$(pwd)/$OUT"
echo "then check the log: the IM_LOAD lines should resolve instead of"
echo "reporting cache MISS."
