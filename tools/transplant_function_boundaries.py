#!/usr/bin/env python3
"""Carry function boundaries from a build that has symbols to one that does not.

XenonRecomp needs to know where functions begin and end. The .pdata section
supplies that only for functions with a stack frame; leaf functions are missing
from it, and without a PDB the analyzer guesses at those and regularly splits a
real function into pieces, which makes the recompiler emit unresolvable
branches.

Two builds of the same title made close together contain nearly the same code
at different addresses. This matches functions between them by content - the
sequence of instruction opcodes, which survives relocation because it ignores
addresses and immediates - and uses the matched pairs as anchors to translate a
known-good boundary list from the source build into the target's addresses.

Every translated entry is verified by comparing the opcode sequence at the
mapped address before it is emitted, so a boundary is only carried across when
the code there really is the same function.
"""

import argparse
import bisect
import hashlib
import re
import struct
from pathlib import Path

IMAGE_BASE = 0x82000000
PDATA_RVA = 0x7C800
PDATA_SIZE = 0x12B58
ENTRY_RE = re.compile(r"\{\s*address\s*=\s*0x([0-9A-Fa-f]+)\s*,\s*size\s*=\s*0x([0-9A-Fa-f]+)")


def read_pdata(image: bytes) -> dict[int, int]:
    functions: dict[int, int] = {}
    for offset in range(PDATA_RVA, PDATA_RVA + PDATA_SIZE, 8):
        begin, info = struct.unpack_from(">II", image, offset)
        if begin == 0 and info == 0:
            continue
        length = ((info >> 8) & 0x3FFFFF) * 4
        if begin >= IMAGE_BASE and 0 < length <= 0x20000:
            functions[begin] = length
    return functions


def opcodes(image: bytes, address: int, length: int) -> bytes | None:
    offset = address - IMAGE_BASE
    if offset < 0 or offset + length > len(image):
        return None
    # Only the 6-bit primary opcode of each instruction, so relocated addresses
    # and changed immediates do not perturb the signature.
    return bytes((image[offset + i] >> 2) for i in range(0, length, 4))


def signature(image: bytes, address: int, length: int) -> bytes | None:
    ops = opcodes(image, address, length)
    return hashlib.blake2b(ops, digest_size=16).digest() if ops else None


def build_anchors(source: bytes, target: bytes) -> list[tuple[int, int]]:
    """Pairs of (source address, target address) for functions matched 1:1."""
    def unique_signatures(image: bytes) -> dict[bytes, int]:
        buckets: dict[bytes, list[int]] = {}
        for address, length in read_pdata(image).items():
            key = signature(image, address, length)
            if key is not None:
                buckets.setdefault(key, []).append(address)
        return {key: found[0] for key, found in buckets.items() if len(found) == 1}

    source_map = unique_signatures(source)
    target_map = unique_signatures(target)
    shared = source_map.keys() & target_map.keys()
    return sorted((source_map[key], target_map[key]) for key in shared)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_image", type=Path, help="decrypted image that has known boundaries")
    parser.add_argument("target_image", type=Path, help="decrypted image that needs them")
    parser.add_argument("source_toml", type=Path, help="TOML containing the known functions = [...]")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    source = args.source_image.read_bytes()
    target = args.target_image.read_bytes()

    anchors = build_anchors(source, target)
    anchor_sources = [pair[0] for pair in anchors]
    print(f"content-matched anchor functions: {len(anchors)}")

    carried: list[tuple[int, int]] = []
    unverified = 0
    for match in ENTRY_RE.finditer(args.source_toml.read_text()):
        address = int(match.group(1), 16)
        length = int(match.group(2), 16)
        if length == 0 or length > 0x20000:
            continue
        # A short function's opcode sequence is not distinctive enough to
        # confirm a mapping - plenty of unrelated three-instruction stubs look
        # alike - so only carry functions long enough for the check to mean
        # something.
        if length < 0x20:
            unverified += 1
            continue
        index = bisect.bisect_right(anchor_sources, address) - 1
        if index < 0 or index + 1 >= len(anchors):
            continue
        source_anchor, target_anchor = anchors[index]
        next_source, next_target = anchors[index + 1]
        # Require the function to sit between two anchors that moved by the
        # same amount. If the two deltas disagree, code was inserted or removed
        # inside this span and any address mapped through it is a guess.
        if (target_anchor - source_anchor) != (next_target - next_source):
            unverified += 1
            continue
        if not (source_anchor <= address and address + length <= next_source):
            unverified += 1
            continue
        mapped = address + (target_anchor - source_anchor)
        expected = opcodes(source, address, length)
        actual = opcodes(target, mapped, length)
        if expected is None or actual is None or expected != actual:
            unverified += 1
            continue
        carried.append((mapped, length))

    carried.sort()
    merged: list[tuple[int, int]] = []
    for address, length in carried:
        if merged and address < merged[-1][0] + merged[-1][1]:
            continue  # already covered by a previously carried function
        merged.append((address, length))

    with args.output.open("w") as out:
        out.write("# Function boundaries carried over from a build with symbols.\n")
        out.write("# Each entry was verified by matching the opcode sequence at the\n")
        out.write("# translated address, so only genuinely identical code is listed.\n")
        out.write("functions = [\n")
        for address, length in merged:
            out.write("    { address = 0x%08X, size = 0x%X },\n" % (address, length))
        out.write("]\n")
    print(f"boundaries carried across and verified: {len(merged)}")
    print(f"rejected because the code did not match: {unverified}")


if __name__ == "__main__":
    main()
