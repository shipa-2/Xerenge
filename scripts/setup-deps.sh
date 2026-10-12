#!/bin/sh
# Fetches the two other pieces the renderer is built from, and builds the
# shader translator.
#
#   plume         the Vulkan layer the plume renderer draws through (our fork:
#                 it adds reading a texture back, and mailbox presentation)
#   XenosRecomp   translates the title's Xenos shaders to SPIR-V (our fork:
#                 literal constants, the guest vertex index, 2D-array blending),
#                 with scripts/patches/xenosrecomp-*.patch applied over it
#
# Both are separate checkouts beside rexglue/, like the SDK.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
. "$root/scripts/platform.sh"
plume="$root/plume"
xenos="$root/XenosRecomp"
plume_remote=${XERENGE_PLUME_REMOTE:-https://github.com/shipa-2/plume-xerenge.git}
xenos_remote=${XERENGE_XENOSRECOMP_REMOTE:-https://github.com/shipa-2/XenosRecomp-xerenge.git}

if [ ! -d "$plume/.git" ]; then
    echo "== fetching plume into $plume"
    git clone --depth 1 "$plume_remote" "$plume"
else
    echo "== plume is already there: $plume; updating"
    git -C "$plume" fetch --depth 1 origin
    git -C "$plume" reset --hard origin/HEAD
fi
echo "== plume submodules"
retry git -C "$plume" submodule update --init --recursive --depth 1

if [ ! -d "$xenos/.git" ]; then
    echo "== fetching XenosRecomp into $xenos"
    git clone "$xenos_remote" "$xenos"
else
    echo "== XenosRecomp is already there: $xenos; updating"
    git -C "$xenos" fetch origin
    git -C "$xenos" reset --hard origin/HEAD
fi
# Our changes to XenosRecomp that its repository does not have yet, applied
# over what was just fetched. One already there is left as it is; one that
# neither applies nor is there means the fork moved under it, and building
# without it would quietly bring the problem it fixes back.
for patch in "$root"/scripts/patches/xenosrecomp-*.patch; do
    [ -f "$patch" ] || continue
    name=$(basename "$patch")
    if git -C "$xenos" apply --reverse --check "$patch" 2>/dev/null; then
        echo "== XenosRecomp already has $name"
    elif git -C "$xenos" apply --check "$patch" 2>/dev/null; then
        echo "== applying $name to XenosRecomp"
        git -C "$xenos" apply "$patch"
    else
        echo "$name neither applies to XenosRecomp at $(git -C "$xenos" rev-parse --short HEAD) nor is already applied - update the patch" >&2
        exit 1
    fi
done
echo "== XenosRecomp submodules"
retry git -C "$xenos" submodule update --init --recursive --depth 1

echo "== building XenosRecomp"
# On a Mac XenosRecomp also compiles every shader to Metal AIR, which needs the
# metal compiler from a full Xcode. plume takes SPIR-V only, so leave it out.
xenos_options=
if [ "$XR_OS" = mac ]; then
    xenos_options=-DXENOS_RECOMP_AIR=OFF
fi
cmake -S "$xenos" -B "$xenos/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ $xenos_options
cmake --build "$xenos/build" --target XenosRecomp -j"$XR_JOBS"

echo
echo "done. Shader translator: $xenos/build/XenosRecomp/XenosRecomp$XR_EXE"
