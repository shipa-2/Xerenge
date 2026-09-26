#!/bin/sh
# Builds the C++ replacements for the Python setup/shader tools.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/tools/cxx/build"
cmake -S "$root/tools/cxx" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DOPENSSL_ROOT_DIR=/usr \
    -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/libcrypto.so \
    -DOPENSSL_SSL_LIBRARY=/usr/lib/libssl.so
cmake --build "$build" -j"$(nproc)"
cmake --install "$build" --prefix "$root/tools"

install -d "$root/scripts" "$root/rexglue/tools"
cp -f "$root/tools/bin/extract-image" "$root/scripts/extract-image"
for tool in dump_xex_image build_runtime_shaders rehost_runtime_shader \
    wrap_runtime_pixel_shader wrap_runtime_vertex_shader prepare_aux_shader_cache \
    make_runtime_shader_recipe; do
    cp -f "$root/tools/bin/$tool" "$root/rexglue/tools/$tool"
done
echo "done. tools in $root/tools/bin (extract-image also in scripts/)"
