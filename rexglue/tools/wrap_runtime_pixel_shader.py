#!/usr/bin/env python3
"""Wrap captured Xenos pixel microcode in a container XenosRecomp can scan.

Supersedes xerenge-release/wrap_pixel_shader.py, which declares no
interpolators at all. XenosRecomp links a vertex output to a pixel input by
the name it builds from (usage, usageIndex) - see addShaderInterpolator in
shader_recompiler.cpp - so a pixel shader that declares none receives nothing
from the vertex stage, however well the vertex side is described. The
declarations to pass here are derived at runtime from the microcode itself
and written next to the dump as PS_<hash>_<size>.wrapargs.

Layout mirrors XenosRecomp/tools/wrap_runtime_vertex_shader.py. A raw capture
carries no record of which sampler registers the shader reads, so s0 is
declared unconditionally, as in the original pixel wrapper.
"""

import argparse
import struct
from pathlib import Path

USAGES = {
    "position": 0,
    "blendweight": 1,
    "blendindices": 2,
    "normal": 3,
    "pointsize": 4,
    "texcoord": 5,
    "tangent": 6,
    "binormal": 7,
    "color": 10,
}


def declaration(value: str) -> tuple[int, int, int]:
    try:
        usage_index, usage, register = value.split(":")
        return int(usage_index, 0), USAGES[usage.lower()], int(register, 0)
    except (ValueError, KeyError) as error:
        raise argparse.ArgumentTypeError(
            "expected USAGE_INDEX:USAGE:REGISTER, for example 0:texcoord:0"
        ) from error


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("microcode", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--interpolator",
        action="append",
        type=declaration,
        default=[],
        help="repeatable; omit only for a shader that reads no varyings",
    )
    parser.add_argument(
        "--outputs",
        type=lambda v: int(v, 0),
        default=0x1,
        help="PixelShaderOutputs mask (default COLOR0 only)",
    )
    args = parser.parse_args()

    code = args.microcode.read_bytes()
    if not code or len(code) % 4:
        parser.error("microcode must contain big-endian 32-bit words")

    interpolators = args.interpolator
    if len(interpolators) > 16:
        parser.error("at most 16 interpolators fit in interpolatorInfo")

    shader_offset = 0x74
    # PixelShader is Shader + field18 + outputs, then the interpolator array.
    array_offset = shader_offset + 0x24
    virtual_size = array_offset + 4 * len(interpolators)
    data = bytearray(virtual_size + len(code))

    def word(offset: int, value: int) -> None:
        struct.pack_into(">I", data, offset, value)

    def half(offset: int, value: int) -> None:
        struct.pack_into(">H", data, offset, value)

    # flags: 0x102A1100 base version, bit0 clear = pixel shader.
    header = [0x102A1100, virtual_size, len(code), 0, 0x24, 0,
              shader_offset, 0, 0]
    for index, value in enumerate(header):
        word(index * 4, value)

    # D3DXSHADER_CONSTANTTABLE at 0x28 with two CONSTANTINFO entries at 0x44:
    # the generic c[0..255] float4 catch-all and the s0 sampler.
    word(0x24, 56)
    for index, value in enumerate([28, 0, 0, 2, 28, 0, 0]):
        word(0x28 + index * 4, value)
    float4_constant = 0x44
    word(float4_constant, 68)  # name at 0x28+68 = 0x6C
    half(float4_constant + 4, 2)  # RegisterSet::Float4
    half(float4_constant + 6, 0)
    half(float4_constant + 8, 256)
    sampler_constant = 0x58
    word(sampler_constant, 70)  # name at 0x28+70 = 0x6E
    half(sampler_constant + 4, 3)  # RegisterSet::Sampler
    half(sampler_constant + 6, 0)  # register s0
    half(sampler_constant + 8, 1)
    data[0x6C:0x6E] = b"c\0"
    data[0x6E:0x71] = b"s0\0"

    # Shader base: physicalOffset, size, field8, fieldC, field10,
    # interpolatorInfo (count << 5); PixelShader adds field18 then outputs.
    shader = [0, len(code), 0, 0, 0, len(interpolators) << 5,
              0, args.outputs, 0]
    for index, value in enumerate(shader):
        word(shader_offset + index * 4, value)

    for index, (usage_index, usage, register) in enumerate(interpolators):
        word(array_offset + index * 4,
             usage_index | (usage << 4) | (register << 8))

    data[virtual_size:] = code
    args.output.write_bytes(data)


if __name__ == "__main__":
    main()
