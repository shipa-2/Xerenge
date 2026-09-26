#!/bin/sh
# Fetches the two other pieces the renderer is built from, and builds the
# shader translator.
#
#   plume         the Vulkan layer the plume renderer draws through (our fork:
#                 it adds reading a texture back, and mailbox presentation)
#   XenosRecomp   translates the title's Xenos shaders to SPIR-V (our fork:
#                 literal constants, the guest vertex index, 2D-array blending)
#
# Both are separate checkouts beside rexglue/, like the SDK.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
plume="$root/plume"
xenos="$root/XenosRecomp"
plume_remote=${XERENGE_PLUME_REMOTE:-https://github.com/shipa-2/plume.git}
xenos_remote=${XERENGE_XENOSRECOMP_REMOTE:-https://github.com/shipa-2/XenosRecomp-xerenge.git}

if [ ! -d "$plume/.git" ]; then
    echo "== fetching plume into $plume"
    git clone --depth 1 "$plume_remote" "$plume"
else
    echo "== plume is already there: $plume"
fi
echo "== plume submodules"
git -C "$plume" submodule update --init --recursive --depth 1

if [ ! -d "$xenos/.git" ]; then
    echo "== fetching XenosRecomp into $xenos"
    git clone "$xenos_remote" "$xenos"
else
    echo "== XenosRecomp is already there: $xenos"
fi
echo "== XenosRecomp submodules"
git -C "$xenos" submodule update --init --recursive --depth 1

echo "== building XenosRecomp"
cmake -S "$xenos" -B "$xenos/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$xenos/build" --target XenosRecomp -j"$(nproc)"

echo
echo "done. Shader translator: $xenos/build/XenosRecomp/XenosRecomp"
