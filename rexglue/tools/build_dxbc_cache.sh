#!/bin/sh
# DXBC (shader model 5.1, fxc) for every translated shader, for D3D12 where
# DXIL is not taken - the Xbox One. Runs on Windows (fxc is Windows only), over
# the HLSL XenosRecomp dumped into generated/xenos-hlsl; writes
# generated/shader_cache_dxbc.cpp, linked with -DXERENGE_SHADER_CACHE_DXBC_CPP=.
#
# A shader with specialisation constants is compiled once per combination of
# the ones it tests: fxc has no library to link a value into later.
set -e
cd "$(dirname "$0")/.."

FXC=${FXC:-$(ls "/c/Program Files (x86)/Windows Kits/10/bin/"*/x64/fxc.exe 2>/dev/null | sort | tail -1)}
if [ ! -x "$FXC" ]; then
    echo "fxc not found (set FXC=)" >&2
    exit 1
fi
SRC=generated/xenos-hlsl
WORK=generated/dxbc
OUT=generated/shader_cache_dxbc.cpp
rm -rf "$WORK"
mkdir -p "$WORK"

# The bits shader_common.h gives the specialisation constants Burnout uses.
spec_bit() {
    case $1 in
        SPEC_CONSTANT_R11G11B10_NORMAL) echo 1 ;;
        SPEC_CONSTANT_ALPHA_TEST) echo 2 ;;
        SPEC_CONSTANT_BLOOM_IN_PLACE) echo 64 ;;
        *) echo 0 ;;
    esac
}

entries=""
count=0
failed=0
for f in "$SRC"/*.hlsl; do
    h=$(basename "$f" .hlsl)
    if grep -q "^Interpolators shaderMain" "$f"; then
        target=vs_5_1 stage=XENOS_VERTEX_SHADER
    else
        target=ps_5_1 stage=XENOS_PIXEL_SHADER
    fi
    # A dump from an older XenosRecomp has neither XENOS_FXC's block nor the forms
    # fxc can parse: give it both.
    src=$WORK/src_$h.hlsl
    : > "$src"
    if ! grep -q XENOS_FXC "$f"; then
        printf '#define texture xenos_texture\n#define select(c, a, b) ((c) ? (a) : (b))\nuint xenos_spec() { return XENOS_SPEC_CONSTANTS; }\n#define g_SpecConstants xenos_spec\n' > "$src"
    fi
    sed -e "s/DEFINE_SHARED_CONSTANTS()/DEFINE_SHARED_CONSTANTS/" -e "s/\[\[vk::location([0-9]*)\]\] *//" "$f" >> "$src"
    mask=0
    for name in $(grep -o "g_SpecConstants() & SPEC_CONSTANT_[A-Z0-9_]*" "$f" | sed 's/.*& //' | sort -u); do
        mask=$((mask | $(spec_bit "$name")))
    done
    # Every subset of the mask.
    spec=0
    while :; do
        name=k${h}_$spec
        if "$FXC" //nologo //T $target //E shaderMain //O3 //enable_unbounded_descriptor_tables \
            //D $stage=1 //D XENOS_FXC=1 //D XENOS_SPEC_CONSTANTS=$spec \
            //Fh "$WORK/$name.h" //Vn $name "$src" > "$WORK/$name.log" 2>&1; then
            entries="$entries$h $spec $mask $name
"
            count=$((count + 1))
        else
            echo "fxc failed: $h spec $spec (see $WORK/$name.log)" >&2
            failed=$((failed + 1))
        fi
        [ $spec -eq $mask ] && break
        spec=$(( (spec - mask) & mask ))
    done
done

{
    echo "// Written by tools/build_dxbc_cache.sh: fxc's shader model 5.1 DXBC for each"
    echo "// translated shader and specialisation value."
    echo "#include <cstddef>"
    echo "#include <cstdint>"
    echo "#include \"shader_cache.h\""
    echo "typedef unsigned char BYTE;"
    echo "$entries" | while read -r h spec mask name; do
        if [ -n "$name" ]; then echo "#include \"dxbc/$name.h\""; fi
    done
    echo "const DxbcCacheEntry g_dxbcCacheEntries[] = {"
    echo "$entries" | while read -r h spec mask name; do
        if [ -n "$name" ]; then echo "  { 0x$h, $spec, $mask, $name, sizeof($name) },"; fi
    done
    echo "};"
    echo "const size_t g_dxbcCacheEntryCount = $count;"
} > "$OUT"
echo "dxbc cache: $OUT ($count variants, $failed failed)"
