#!/usr/bin/env python3
"""Rebuild a runtime-assembled shader's container from the template it came from.

The title builds some shaders by patching a few vertex-fetch instructions into
a shader that does exist in the executable. The patched copy has a different
hash, so the static scan never translates it - but the template's container is
right there in the image, with the real constant names, vertex element and
interpolator declarations.

So rather than reconstructing declarations from the microcode (a reconstruction
can only guess at semantics that were never encoded in it), this finds the
template by matching instructions and reuses its container verbatim, swapping
in the captured microcode. Everything but the patched instructions then comes
from the game's own data.

Usage:
    rehost_runtime_shader.py guest-image.bin captured.bin out.container
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

CONTAINER_VERSIONS = (0x102A1100, 0x102A0E00)
HEADER_SIZE = 36
INSTRUCTION_SIZE = 12


def find_containers(image: bytes):
    """Yield (offset, virtual_size, physical_size) for every container found.

    Mirrors XenosRecomp's scan: probe every 4 bytes for the version signature,
    sanity-check the sizes, and skip past a container once accepted.
    """
    i = 0
    limit = len(image) - HEADER_SIZE
    while i < limit:
        flags = struct.unpack_from(">I", image, i)[0]
        if (flags & 0xFFFFFF00) in CONTAINER_VERSIONS:
            virtual_size, physical_size = struct.unpack_from(">II", image, i + 4)
            total = virtual_size + physical_size
            if 0 < total <= len(image) - i and physical_size % INSTRUCTION_SIZE == 0:
                yield i, virtual_size, physical_size
                i += total
                continue
        i += 4


def similarity(a: bytes, b: bytes) -> float:
    """Fraction of instructions the two microcode blocks share position-wise."""
    if len(a) != len(b) or not a:
        return 0.0
    count = len(a) // INSTRUCTION_SIZE
    same = sum(
        1
        for i in range(count)
        if a[i * INSTRUCTION_SIZE:(i + 1) * INSTRUCTION_SIZE]
        == b[i * INSTRUCTION_SIZE:(i + 1) * INSTRUCTION_SIZE]
    )
    return same / count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path, help="decrypted guest image")
    parser.add_argument("microcode", type=Path, help="captured runtime microcode")
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--min-similarity",
        type=float,
        default=0.6,
        help="reject a template sharing fewer instructions than this (default 0.6)",
    )
    args = parser.parse_args()

    image = args.image.read_bytes()
    code = args.microcode.read_bytes()
    if not code or len(code) % INSTRUCTION_SIZE:
        parser.error("microcode must be whole 12-byte instructions")

    best = None
    for offset, virtual_size, physical_size in find_containers(image):
        if physical_size != len(code):
            continue
        template = image[offset + virtual_size:offset + virtual_size + physical_size]
        score = similarity(template, code)
        if best is None or score > best[0]:
            best = (score, offset, virtual_size, physical_size)

    if best is None:
        print(f"{args.microcode.name}: no container of {len(code)} bytes in the image")
        return 1

    score, offset, virtual_size, physical_size = best
    differing = int(round((1.0 - score) * (len(code) // INSTRUCTION_SIZE)))
    if score < args.min_similarity:
        print(
            f"{args.microcode.name}: best template at {offset:#x} shares only "
            f"{score:.0%} of instructions - too little to call it the same shader"
        )
        return 1

    container = bytearray(image[offset:offset + virtual_size + physical_size])
    container[virtual_size:] = code
    args.output.write_bytes(container)
    print(
        f"{args.microcode.name}: template at {offset:#x}, {score:.0%} of instructions "
        f"identical ({differing} patched) -> {args.output.name}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
