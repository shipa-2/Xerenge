#!/usr/bin/env python3
"""Restore function entry points that a boundary list hides inside another function.

A .pdata record describes a region covered by one unwind rule, not necessarily a
single function: a run of adjacent leaf routines sharing a frame layout is often
recorded as one entry. Carrying such a record over as a function boundary yields
a recompiled function that starts at the first routine and swallows the rest, so
every routine after the first loses its entry point. A direct branch to one
still resolves - it becomes a jump inside the generated function - but an
indirect call through a pointer cannot be dispatched, and the runtime drops it.

That is not a rare corner. The C runtime starts a title by walking a
null-terminated table of static-constructor pointers; when that table addresses
a swallowed region every C++ global in the image stays unconstructed, and the
title deadlocks later on, far from the cause.

The addresses to repair are taken from the runtime's own report of indirect
calls it could not dispatch, not guessed from the image. Scanning the image for
pointer tables instead finds thousands of candidates, and splitting on them
wrecks the recompilation: a switch statement compiles to a table addressing the
middle of the function it belongs to, so the two cases cannot be told apart from
the data alone. A dispatch that actually failed is unambiguous - the target is
an entry point and nothing reaches it.

The image still gets a veto. A routine begins where the previous one stopped, so
the instruction before an entry point never falls through; if it does, the
report is describing something other than a hidden routine and the boundary is
left alone.
"""

import argparse
import re
import struct
from pathlib import Path

IMAGE_BASE = 0x82000000
ENTRY_RE = re.compile(r"\{\s*address\s*=\s*0x([0-9A-Fa-f]+)\s*,\s*size\s*=\s*0x([0-9A-Fa-f]+)")
REPORT_RE = re.compile(r"unresolved PPC indirect target:\s*0x([0-9A-Fa-f]+)")


def is_entry_point(image: bytes, address: int) -> bool:
    """Whether a routine can begin here, judged by the instruction before it."""
    offset = address - IMAGE_BASE - 4
    if offset < 0 or offset + 4 > len(image):
        return False
    (previous,) = struct.unpack_from(">I", image, offset)
    if previous in (0x00000000, 0x60000000):
        return True  # padding between routines
    opcode = previous >> 26
    link = previous & 1
    if opcode == 18:
        return link == 0  # b/ba, but not the bl that falls through
    if opcode == 19:
        extended = (previous >> 1) & 0x3FF
        branch_option = (previous >> 21) & 0x1F
        if extended in (16, 528) and link == 0:
            return branch_option & 0b10100 == 0b10100  # blr/bctr, unconditional
    return False


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path, help="decrypted image")
    parser.add_argument("toml", type=Path, help="config holding functions = [...]")
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "reports",
        type=Path,
        nargs="+",
        help="runtime logs naming indirect targets that failed to dispatch",
    )
    args = parser.parse_args()

    image = args.image.read_bytes()
    reported = {
        int(m.group(1), 16)
        for report in args.reports
        for m in REPORT_RE.finditer(report.read_text(errors="replace"))
    }
    targets = sorted(t for t in reported if is_entry_point(image, t))
    print(f"indirect targets the runtime could not dispatch: {len(reported)}")
    print(f"of those, confirmed entry points: {len(targets)}")

    text = args.toml.read_text()
    functions = sorted(
        (int(m.group(1), 16), int(m.group(2), 16)) for m in ENTRY_RE.finditer(text)
    )

    split: list[tuple[int, int]] = []
    divided = 0
    for address, size in functions:
        interior = [t for t in targets if address < t < address + size]
        if not interior:
            split.append((address, size))
            continue
        divided += 1
        bounds = [address] + interior + [address + size]
        for begin, end in zip(bounds, bounds[1:]):
            split.append((begin, end - begin))

    body = "".join("    { address = 0x%08X, size = 0x%X },\n" % e for e in split)
    rewritten = ENTRY_RE.sub("", text)
    rewritten = re.sub(
        r"functions\s*=\s*\[[^\]]*\]",
        "functions = [\n" + body + "]",
        rewritten,
        count=1,
        flags=re.S,
    )
    args.output.write_text(rewritten)
    print(f"boundaries that hid an entry point: {divided}")
    print(f"boundaries before / after: {len(functions)} / {len(split)}")


if __name__ == "__main__":
    main()
