#!/bin/sh
# Builds the title. Code generation runs as part of the build: the recompiler
# recovers the whole retail image by itself in a few seconds, taking the handful
# of function boundaries it cannot see - the ones reached only through a vtable
# slot - from functions.toml.
#
#   ./scripts/build.sh [--game <game directory>] [--generate-only]
#
# --generate-only stops after the code and the shaders are generated: what a
# cross build (the Android one) needs from this machine, without the host game.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
. "$root/scripts/platform.sh"
project="$root/rexglue"
sdk="$root/rexglue-sdk"
game=
generate_only=

if [ ! -x "$root/tools/bin/patch_burnout_manifest" ]; then
    "$root/scripts/build-tools.sh"
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --game) game=$2; shift 2 ;;
        --generate-only) generate_only=1; shift ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ ! -d "$sdk" ]; then
    echo "no SDK at $sdk - run ./scripts/setup-sdk.sh first" >&2
    exit 1
fi

if [ -n "$game" ]; then
    # Written into the manifest and read back by native tools, so on Windows
    # it has to be D:/... rather than the shell's own /d/... spelling.
    if [ "$XR_OS" = windows ]; then
        game=$(cd "$game" && pwd -W)
    else
        game=$(cd "$game" && pwd)
    fi
    if [ ! -f "$game/default.xex" ]; then
        echo "$game holds no default.xex" >&2
        exit 1
    fi
    # The manifest names the image and the data directory; point both at this
    # copy of the game rather than leaving a path from another machine.
    "$root/scripts/xerenge-tool" patch_burnout_manifest "$project/burnout_manifest.toml" "$game"
    echo "== the manifest now points at $game"
fi

# The code generator turns the executable into C++ (generated/), and writes
# the CMake glue the project's CMakeLists.txt includes - so it runs first.
codegen=
for candidate in "$sdk/out/$XR_PRESET/Release/rexglue$XR_EXE" "$sdk/out/$XR_PRESET/rexglue$XR_EXE"; do
    [ -x "$candidate" ] && codegen=$candidate && break
done
if [ -z "$codegen" ]; then
    echo "no code generator in $sdk/out/$XR_PRESET - run ./scripts/setup-sdk.sh first" >&2
    exit 1
fi
echo "== generating the code"
(cd "$project" && "$codegen" codegen burnout_manifest.toml)

# The title's shaders, translated from this copy of the game: the renderer
# links them in, so this comes before configuring. They are the game's own
# code, which is why they are made here and not kept in the repository.
if [ ! -s "$project/generated/shader_cache.cpp" ]; then
    echo "== translating the game's shaders"
    if [ ! -x "$root/XenosRecomp/build/XenosRecomp/XenosRecomp$XR_EXE" ]; then
        echo "no shader translator - run ./scripts/setup-deps.sh first" >&2
        exit 1
    fi
    game_root=$(sed -n 's/^game_root *= *"\(.*\)"/\1/p' "$project/burnout_manifest.toml" | head -1)
    (cd "$project" && XENOSRECOMP="$root/XenosRecomp/build/XenosRecomp/XenosRecomp$XR_EXE" \
        XENOSRECOMP_HEADER="$root/XenosRecomp/XenosRecomp/shader_common.h" \
        ./tools/rebuild_plume_shaders.sh "$game_root/default.xex")
fi

# The shaders the title assembles while it runs are not in that scan. Each is
# rebuilt from its source on the disc by the recipe in tools/, so the game has
# every shader from its first frame without being run once to collect them.
if [ ! -s "$project/generated/shader_cache_runtime.cpp" ]; then
    echo "== translating the shaders the game assembles at runtime"
    (cd "$project" && XENOSRECOMP="$root/XenosRecomp/build/XenosRecomp/XenosRecomp$XR_EXE" \
        XENOSRECOMP_HEADER="$root/XenosRecomp/XenosRecomp/shader_common.h" \
        ./tools/translate_runtime_shaders.sh --from-disc)
fi

if [ -n "$generate_only" ]; then
    echo "== generated: $project/generated"
    exit 0
fi

# etcpak is a submodule of this repository, and a clone without
# --recurse-submodules leaves it empty; CMake then stops at xerenge-etcpak.
if [ ! -f "$root/rexglue/thirdparty/etcpak/ProcessRGB.cpp" ]; then
    echo "== fetching etcpak"
    retry git -C "$root" submodule update --init --depth 1 rexglue/thirdparty/etcpak
fi

echo "== configuring"
# Two flags are not optional and are not in the SDK's own project template:
# the SSSE3 baseline, because the SDK's memory code uses those intrinsics while
# a generated project sets no architecture, and clang, which is what this has
# been built and run with.
# Ninja on Windows, where CMake would otherwise pick Visual Studio and MSVC.
generator=
if [ "$XR_OS" = windows ]; then
    generator="-G Ninja"
fi
march=-march=x86-64-v2
if [ "$XR_OS" = mac ]; then
    # The SDK's mac-arm64 preset builds for this baseline as well.
    march=-march=armv8-a
fi
cmake -S "$project" -B "$project/build" $generator \
      -DREXSDK_DIR="$sdk" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS=$march -DCMAKE_CXX_FLAGS=$march

echo "== building"
cmake --build "$project/build" -j"$XR_JOBS"

echo
echo "done: $project/build/burnout$XR_EXE"
echo "to run: $project/run.sh"
