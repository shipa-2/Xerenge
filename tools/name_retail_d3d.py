"""Name the retail build's functions using the beta's symbols.

The two builds contain nearly the same code at different addresses, so a
function can be recognised by its content. The sequence of instruction opcodes
ignores addresses and immediates, which is exactly what relocation changes, so
it survives the move; a signature unique on both sides identifies the same
function in each.

Unlike transplant_symbols.py this does not need a .pdata table on the target:
the recompiler already enumerated every retail function, so the address list
comes from its own generated registration table and the sizes from the gaps
between consecutive entries.
"""
import re
import struct
import sys
from pathlib import Path

IMAGE_BASE = 0x82000000
PE_BASE = 0x400000


def opcode_signature(code: bytes) -> bytes:
    # Primary opcode plus, for the forms that carry one, the extended opcode.
    # Immediates and displacements are dropped: those are what differ between
    # two builds of the same source.
    out = bytearray()
    for i in range(0, len(code) - 3, 4):
        word = struct.unpack_from(">I", code, i)[0]
        primary = word >> 26
        out.append(primary)
        if primary in (19, 31, 59, 63):
            out += struct.pack(">H", (word >> 1) & 0x3FF)
    return bytes(out)


def beta_functions(pe: bytes, symbols: Path):
    # .pdata holds absolute addresses here, not RVAs, and its length field is
    # not worth decoding: the gap to the next entry gives the same answer and
    # cannot be misread. .text is not page-aligned in the file, so the raw
    # offset trails the virtual address by a fixed amount.
    TEXT_VA, TEXT_RAW = 0x90000, 0x8F400
    file_delta = PE_BASE + TEXT_VA - TEXT_RAW
    pdata = pe[0x7C800:0x7C800 + 0x12B58]
    begins = sorted(
        {struct.unpack_from(">I", pdata, off)[0] for off in range(0, len(pdata), 8)} - {0}
    )
    sizes = {a: begins[i + 1] - a for i, a in enumerate(begins[:-1])}
    named = {}
    for line in symbols.read_text(errors="replace").splitlines():
        m = re.match(r"^0x([0-9A-Fa-f]+)\s+\.text\s+function\s+(\S+)", line)
        if m:
            named[int(m.group(1), 16) - IMAGE_BASE + PE_BASE] = m.group(2)
    out = {}
    for addr, name in named.items():
        size = sizes.get(addr)
        if not size or size < 32 or size > 0x8000:
            continue
        off = addr - file_delta
        if off < 0 or off + size > len(pe):
            continue
        out.setdefault(opcode_signature(pe[off:off + size]), []).append(name)
    return out


def retail_functions(image: bytes, registration: Path):
    addresses = sorted(
        int(m, 16) for m in re.findall(r"\{\s*(0x[0-9A-Fa-f]{8}),", registration.read_text())
    )
    out = {}
    for i, addr in enumerate(addresses[:-1]):
        size = addresses[i + 1] - addr
        if size < 32 or size > 0x8000:
            continue
        off = addr - IMAGE_BASE
        if off + size > len(image):
            continue
        out.setdefault(opcode_signature(image[off:off + size]), []).append(addr)
    return out


def main() -> None:
    pe = Path(sys.argv[1]).read_bytes()
    beta = beta_functions(pe, Path(sys.argv[2]))
    image = Path(sys.argv[3]).read_bytes()
    retail = retail_functions(image, Path(sys.argv[4]))
    named = 0
    with Path(sys.argv[5]).open("w") as out:
        for signature, names in beta.items():
            addresses = retail.get(signature)
            # A signature shared by several functions on either side names none
            # of them: a wrong name is worse than no name here.
            if not addresses or len(names) != 1 or len(addresses) != 1:
                continue
            out.write(f"0x{addresses[0]:08X}\t{names[0]}\n")
            named += 1
    print(f"beta signatures {len(beta)}, retail signatures {len(retail)}, "
          f"named {named}")


main()
