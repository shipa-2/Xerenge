#!/usr/bin/env python3
"""Name the functions of a build that has no symbols, using one that has.

Two builds of the same title made close together contain nearly the same code
at different addresses. Function names can therefore be carried across by
content: the sequence of instruction opcodes ignores addresses and immediates,
so it survives relocation, and a signature unique on both sides identifies the
same function in each.

Only exact, unambiguous matches are emitted. A signature shared by several
functions in either build names none of them - a stub that appears a dozen
times cannot be told apart from its twins, and a wrong name is worse than no
name when the whole point is to know what code is running.
"""

import argparse
import hashlib
import re
import struct
from pathlib import Path

IMAGE_BASE = 0x82000000
PE_PREFERRED_BASE = 0x400000
SYMBOL_RE = re.compile(r"^0x([0-9A-Fa-f]+)\s+\.text\s+function\s+(\S+)")


def read_pdata(image: bytes, pdata_rva: int, pdata_size: int) -> dict[int, int]:
    functions: dict[int, int] = {}
    for offset in range(pdata_rva, pdata_rva + pdata_size, 8):
        begin, info = struct.unpack_from(">II", image, offset)
        if begin == 0 and info == 0:
            continue
        length = ((info >> 8) & 0x3FFFFF) * 4
        if begin >= IMAGE_BASE and 0 < length <= 0x20000:
            functions[begin] = length
    return functions


def signature(image: bytes, address: int, length: int) -> bytes | None:
    offset = address - IMAGE_BASE
    if offset < 0 or offset + length > len(image):
        return None
    opcodes = bytes((image[offset + i] >> 2) for i in range(0, length, 4))
    return hashlib.blake2b(opcodes, digest_size=16).digest()


def unique_signatures(image: bytes, pdata: dict[int, int]) -> dict[bytes, int]:
    buckets: dict[bytes, list[int]] = {}
    for address, length in pdata.items():
        key = signature(image, address, length)
        if key is not None:
            buckets.setdefault(key, []).append(address)
    return {key: found[0] for key, found in buckets.items() if len(found) == 1}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_image", type=Path)
    parser.add_argument("source_symbols", type=Path, help="address/section/kind/name table")
    parser.add_argument("target_image", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source-pdata", default="0x7C800,0x12B58")
    parser.add_argument("--target-pdata", default="0x7C800,0x12B58")
    args = parser.parse_args()

    def bounds(text: str) -> tuple[int, int]:
        rva, size = text.split(",")
        return int(rva, 0), int(size, 0)

    source = args.source_image.read_bytes()
    target = args.target_image.read_bytes()
    source_pdata = read_pdata(source, *bounds(args.source_pdata))
    target_map = unique_signatures(target, read_pdata(target, *bounds(args.target_pdata)))

    named: dict[int, str] = {}
    ambiguous: set[int] = set()
    unmatched = 0
    for line in args.source_symbols.read_text(errors="replace").splitlines():
        match = SYMBOL_RE.match(line)
        if not match:
            continue
        address = IMAGE_BASE + int(match.group(1), 16) - PE_PREFERRED_BASE
        length = source_pdata.get(address)
        if length is None:
            unmatched += 1
            continue
        key = signature(source, address, length)
        carried = target_map.get(key) if key is not None else None
        if carried is None:
            unmatched += 1
            continue
        if carried in named and named[carried] != match.group(2):
            ambiguous.add(carried)
            continue
        named[carried] = match.group(2)

    for address in ambiguous:
        named.pop(address, None)

    with args.output.open("w") as out:
        out.write("# Function names carried over from a build that has symbols.\n")
        out.write("# Each was matched by opcode sequence and was unique on both sides.\n")
        for address in sorted(named):
            out.write("0x%08X\t%s\n" % (address, named[address]))
    print(f"functions named: {len(named)}")
    print(f"symbols that could not be placed: {unmatched}")
    print(f"dropped as ambiguous: {len(ambiguous)}")


if __name__ == "__main__":
    main()
