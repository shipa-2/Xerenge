#!/bin/sh
# Builds the C++ replacements for the Python setup/shader tools.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
. "$root/scripts/platform.sh"
build="$root/tools/cxx/build"
# Ninja and clang as everywhere else; left to itself CMake picks Visual
# Studio and MSVC on Windows. A tree configured with another generator is
# only build output, so it is started afresh.
if [ -f "$build/CMakeCache.txt" ] && ! grep -q "^CMAKE_GENERATOR:INTERNAL=Ninja$" "$build/CMakeCache.txt"; then
    rm -rf "$build"
fi
cmake -S "$root/tools/cxx" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j"$XR_JOBS"
cmake --install "$build" --prefix "$root/tools"

install -d "$root/scripts" "$root/rexglue/tools"
cp -f "$root/tools/bin/extract-image$XR_EXE" "$root/scripts/extract-image$XR_EXE"
for tool in dump_xex_image build_runtime_shaders rehost_runtime_shader \
    wrap_runtime_pixel_shader wrap_runtime_vertex_shader prepare_aux_shader_cache \
    make_runtime_shader_recipe; do
    cp -f "$root/tools/bin/$tool$XR_EXE" "$root/rexglue/tools/$tool$XR_EXE"
done
echo "done. tools in $root/tools/bin (extract-image also in scripts/)"
