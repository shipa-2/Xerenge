#!/usr/bin/env python3
"""Check the guest addresses hard-coded in the runtime against the image it runs.

The runtime patches, traces and overrides guest code by address. Those addresses
belong to one particular build of the title, and this project has two: work
started on the Beta 5 image and moved to the retail one. An address carried over
unchanged does not announce itself - it simply points at whatever happens to sit
there now, which is usually the middle of an unrelated function, so the override
either never fires or fires on the wrong code.

Both builds can be named: Beta 5 ships symbols, and transplant_symbols.py
carries those names onto the retail image by matching opcode sequences. That
gives two dictionaries - retail address to name, and name to retail address -
and a Beta 5 address can be looked up in its own symbols and translated.

Every address is reported with what it is in the retail image, and flagged when
it names something in Beta 5 that lives elsewhere in retail.
"""

import argparse
import re
from pathlib import Path

IMAGE_BASE = 0x82000000
PE_PREFERRED_BASE = 0x400000
ADDRESS_RE = re.compile(r"\b0x(82[0-9A-Fa-f]{6})\b")
BETA_SYMBOL_RE = re.compile(r"^0x([0-9A-Fa-f]+)\s+\.text\s+function\s+(\S+)")
RETAIL_SYMBOL_RE = re.compile(r"^0x([0-9A-Fa-f]+)\t(\S+)")
MAPPING_RE = re.compile(r"\{\s*0x([0-9A-Fa-f]+),")


def load_beta_symbols(path: Path) -> dict[int, str]:
    names: dict[int, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        match = BETA_SYMBOL_RE.match(line)
        if match:
            guest = IMAGE_BASE + int(match.group(1), 16) - PE_PREFERRED_BASE
            names.setdefault(guest, match.group(2))
    return names


def load_retail_symbols(path: Path) -> dict[int, str]:
    names: dict[int, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        match = RETAIL_SYMBOL_RE.match(line)
        if match:
            names[int(match.group(1), 16)] = match.group(2)
    return names


def enclosing(names: dict[int, str], sorted_addresses: list[int], address: int):
    """The named function an address falls inside, and how far into it."""
    import bisect

    index = bisect.bisect_right(sorted_addresses, address) - 1
    if index < 0:
        return None, 0
    begin = sorted_addresses[index]
    return names[begin], address - begin


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("sources", type=Path, nargs="+")
    parser.add_argument("--beta-symbols", type=Path, required=True)
    parser.add_argument("--retail-symbols", type=Path, required=True)
    parser.add_argument("--retail-mapping", type=Path, required=True,
                        help="ppc_func_mapping.cpp from the retail recompilation")
    args = parser.parse_args()

    beta = load_beta_symbols(args.beta_symbols)
    retail = load_retail_symbols(args.retail_symbols)
    retail_by_name = {name: address for address, name in retail.items()}
    retail_starts = {
        int(m.group(1), 16)
        for m in MAPPING_RE.finditer(args.retail_mapping.read_text())
    }
    beta_sorted = sorted(beta)
    retail_sorted = sorted(retail)

    found: dict[int, list[str]] = {}
    for source in args.sources:
        for number, line in enumerate(source.read_text().splitlines(), 1):
            for match in ADDRESS_RE.finditer(line):
                address = int(match.group(1), 16)
                found.setdefault(address, []).append(f"{source.name}:{number}")

    wrong: list[tuple[int, str, int, list[str]]] = []
    unknown: list[tuple[int, list[str]]] = []
    for address in sorted(found):
        retail_name, retail_offset = enclosing(retail, retail_sorted, address)
        beta_name, beta_offset = enclosing(beta, beta_sorted, address)
        is_start = address in retail_starts
        # The strong signal is an address that begins a function in Beta 5 but
        # lands *inside* one in retail: it is not an entry point there at all,
        # so an override placed on it can never fire correctly. An address that
        # begins a function in both images is ambiguous - it may well have been
        # chosen for retail - so it is only reported, not corrected.
        stale = (beta_name is not None and beta_offset == 0 and
                 beta_name != retail_name and beta_name in retail_by_name and
                 not is_start)
        if stale:
            wrong.append((address, beta_name, retail_by_name[beta_name], found[address]))
        elif retail_name is None or (not is_start and retail_offset > 0x40):
            unknown.append((address, found[address]))

    print(f"guest addresses hard-coded in the runtime: {len(found)}")
    print(f"written for Beta 5 and wrong for retail: {len(wrong)}")
    for address, name, corrected, sites in wrong:
        print(f"  0x{address:08X} -> 0x{corrected:08X}  {name[:70]}")
        print(f"      {', '.join(sites)}")
    print(f"\nnot a known retail function start, worth a look: {len(unknown)}")
    for address, sites in unknown[:40]:
        print(f"  0x{address:08X}  {', '.join(sites[:3])}")


if __name__ == "__main__":
    main()
