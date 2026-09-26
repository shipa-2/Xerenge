#!/usr/bin/env python3
"""Decrypt Burnout's BASIC-compressed XEX2 into a PE image XenosRecomp can scan."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from Crypto.Cipher import AES

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def dump_xex(xex_path: Path, output: Path) -> None:
    xex = xex_path.read_bytes()
    if xex[:4] != b"XEX2":
        raise SystemExit(f"{xex_path} is not an XEX2 image")

    header_size = u32(xex, 8)
    security_offset = u32(xex, 16)
    header_count = u32(xex, 20)
    image_size = u32(xex, security_offset + 4)
    aes_key = xex[security_offset + 0x150 : security_offset + 0x160]

    fmt_off = None
    off = 24
    for _ in range(header_count):
        key, value = struct.unpack_from(">II", xex, off)
        off += 8
        if key == 0x000003FF:
            fmt_off = value
    if fmt_off is None:
        raise SystemExit("XEX has no FILE_FORMAT_INFO header")

    info_size, enc, comp = struct.unpack_from(">IHH", xex, fmt_off)
    if comp != 1:
        raise SystemExit(f"unsupported XEX compression {comp} (need BASIC=1)")
    if enc not in (0, 1):
        raise SystemExit(f"unsupported XEX encryption {enc}")

    block_count = (info_size - 8) // 8
    blocks = [
        struct.unpack_from(">II", xex, fmt_off + 8 + n * 8) for n in range(block_count)
    ]
    total_data = sum(data_size for data_size, _zero in blocks)
    ciphertext = xex[header_size : header_size + total_data]
    if len(ciphertext) != total_data:
        raise SystemExit("XEX image is truncated")

    if enc == 1:
        session = AES.new(RETAIL_KEY, AES.MODE_CBC, iv=bytes(16)).decrypt(aes_key)
        pad = (16 - (len(ciphertext) % 16)) % 16
        plaintext = AES.new(session, AES.MODE_CBC, iv=bytes(16)).decrypt(
            ciphertext + b"\x00" * pad
        )
    else:
        plaintext = ciphertext

    image = bytearray(image_size)
    dest = 0
    src = 0
    for data_size, zero_size in blocks:
        image[dest : dest + data_size] = plaintext[src : src + data_size]
        dest += data_size + zero_size
        src += data_size

    if image[:2] != b"MZ":
        session = AES.new(DEVKIT_KEY, AES.MODE_CBC, iv=bytes(16)).decrypt(aes_key)
        pad = (16 - (len(ciphertext) % 16)) % 16
        plaintext = AES.new(session, AES.MODE_CBC, iv=bytes(16)).decrypt(
            ciphertext + b"\x00" * pad
        )
        dest = 0
        src = 0
        image = bytearray(image_size)
        for data_size, zero_size in blocks:
            image[dest : dest + data_size] = plaintext[src : src + data_size]
            dest += data_size + zero_size
            src += data_size
        if image[:2] != b"MZ":
            raise SystemExit("decrypted image is not a PE (MZ)")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    print(f"wrote {output} ({len(image)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "xex",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "xerenge-release/game/default.xex",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "generated/xenos-scan/guest-image.bin",
    )
    args = parser.parse_args()
    dump_xex(args.xex, args.output)


if __name__ == "__main__":
    main()
