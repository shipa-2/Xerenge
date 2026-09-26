#!/usr/bin/env python3
"""Wrap captured Xenos vertex microcode in a container XenosRecomp can scan.

Extends XenosRecomp/tools/wrap_runtime_vertex_shader.py with --sampler. That
one declares the generic c[0..255] float4 catch-all and nothing else, so a
vertex shader that fetches a texture recompiles into code referring to an
s<N>_Texture2DDescriptorIndex that was never #defined, and the HLSL fails to
compile. XenosRecomp names an undeclared sampler s<constIndex> (see the
fallback in ShaderRecompiler::recompile for texture fetches), so declaring the
register under exactly that name makes the two agree.

--element addresses are instruction addresses: a declaration answers "what
does the vertex fetch at this point in the program read", so every vertex
fetch instruction needs one, even two reading the same fetch constant.

All declarations are derived at runtime from the microcode and written beside
the dump as VS_<hash>_<size>.wrapargs.
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

CONSTANT_INFO_SIZE = 20
CONSTANT_TABLE_OFFSET = 0x28
CONSTANT_TABLE_HEADER_SIZE = 28  # 7 dwords


def declaration(value: str) -> tuple[int, int, int]:
    try:
        first, usage, last = value.split(":")
        return int(first, 0), USAGES[usage.lower()], int(last, 0)
    except (ValueError, KeyError) as error:
        raise argparse.ArgumentTypeError(
            "expected ADDRESS:USAGE:INDEX, for example 3:position:0"
        ) from error


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("microcode", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--element", action="append", type=declaration, required=True)
    parser.add_argument("--interpolator", action="append", type=declaration, default=[])
    parser.add_argument(
        "--sampler",
        action="append",
        type=lambda v: int(v, 0),
        default=[],
        help="repeatable sampler register this shader fetches from",
    )
    args = parser.parse_args()

    code = args.microcode.read_bytes()
    if not code or len(code) % 4:
        parser.error("microcode must contain big-endian 32-bit words")

    samplers = sorted(set(args.sampler))
    if any(reg > 0xFFFF for reg in samplers):
        parser.error("sampler register does not fit the declaration")

    # Constant table: the float4 catch-all, then one entry per sampler. Names
    # follow the entries, and the shader body follows the names, so the offsets
    # depend on how many samplers were declared.
    constant_count = 1 + len(samplers)
    info_offset = CONSTANT_TABLE_OFFSET + CONSTANT_TABLE_HEADER_SIZE
    names_offset = info_offset + CONSTANT_INFO_SIZE * constant_count

    names = bytearray(b"c\0")
    name_offsets = {"c": 0}
    for reg in samplers:
        name_offsets[f"s{reg}"] = len(names)
        names += f"s{reg}\0".encode()

    shader_offset = (names_offset + len(names) + 3) & ~3
    array_offset = shader_offset + 0x24
    virtual_size = array_offset + 4 * (len(args.element) + len(args.interpolator))
    data = bytearray(virtual_size + len(code))

    def word(offset: int, value: int) -> None:
        struct.pack_into(">I", data, offset, value)

    def half(offset: int, value: int) -> None:
        struct.pack_into(">H", data, offset, value)

    # flags: 0x102A1101 base version, bit0 set = vertex shader.
    header = [0x102A1101, virtual_size, len(code), 0, CONSTANT_TABLE_OFFSET - 4, 0,
              shader_offset, 0, 0]
    for index, value in enumerate(header):
        word(index * 4, value)

    # D3DXSHADER_CONSTANTTABLE: Size, Creator, Version, Constants, ConstantInfo,
    # Flags, Target. Every offset in it is relative to the table itself.
    word(0x24, CONSTANT_TABLE_HEADER_SIZE + CONSTANT_INFO_SIZE * constant_count + len(names))
    for index, value in enumerate([CONSTANT_TABLE_HEADER_SIZE, 0, 0, constant_count,
                                   CONSTANT_TABLE_HEADER_SIZE, 0, 0]):
        word(CONSTANT_TABLE_OFFSET + index * 4, value)

    def put_constant(slot: int, name: str, register_set: int, register_index: int,
                     register_count: int) -> None:
        at = info_offset + slot * CONSTANT_INFO_SIZE
        word(at, names_offset - CONSTANT_TABLE_OFFSET + name_offsets[name])
        half(at + 4, register_set)
        half(at + 6, register_index)
        half(at + 8, register_count)

    put_constant(0, "c", 2, 0, 256)  # RegisterSet::Float4
    for slot, reg in enumerate(samplers, start=1):
        put_constant(slot, f"s{reg}", 3, reg, 1)  # RegisterSet::Sampler

    data[names_offset:names_offset + len(names)] = names

    # Shader base: physicalOffset, size, field8, fieldC, field10,
    # interpolatorInfo (count << 5); VertexShader adds field18 then the element
    # count.
    shader = [0, len(code), 0, 0, 0, len(args.interpolator) << 5,
              0, len(args.element), 0]
    for index, value in enumerate(shader):
        word(shader_offset + index * 4, value)

    for index, (address, usage, usage_index) in enumerate(args.element):
        word(array_offset + index * 4,
             address | (usage << 12) | (usage_index << 16))
    interpolator_offset = array_offset + len(args.element) * 4
    for index, (usage_index, usage, register) in enumerate(args.interpolator):
        word(interpolator_offset + index * 4,
             usage_index | (usage << 4) | (register << 8))

    data[virtual_size:] = code
    args.output.write_bytes(data)


if __name__ == "__main__":
    main()
