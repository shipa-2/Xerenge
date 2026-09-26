#!/usr/bin/env python3
"""Rename XenosRecomp shader_cache.cpp symbols so a second cache can link.

The primary generated cache already occupies g_shaderCacheEntries. Runtime
wraps (shader_cache_extra.cpp) use the Extra suffix. Fetch-patched bootstrap
vertex shaders from an earlier capture use Bootstrap.
"""
from __future__ import annotations

import argparse
from pathlib import Path

SYMBOLS = [
    "g_shaderCacheEntries",
    "g_shaderCacheEntryCount",
    "g_shaderMicrocodeEntries",
    "g_shaderMicrocodeEntryCount",
    "g_compressedSpirvCache",
    "g_spirvCacheCompressedSize",
    "g_spirvCacheDecompressedSize",
]


def transform(src: str, suffix: str) -> str:
    for name in sorted(SYMBOLS, key=len, reverse=True):
        src = src.replace(name, name + suffix)
    for name in (
        f"g_shaderCacheEntryCount{suffix}",
        f"g_shaderMicrocodeEntryCount{suffix}",
        f"g_spirvCacheCompressedSize{suffix}",
        f"g_spirvCacheDecompressedSize{suffix}",
    ):
        src = src.replace(f"const size_t {name}", f"extern const size_t {name}")
    src = src.replace(
        f"const uint8_t g_compressedSpirvCache{suffix}[]",
        f"extern const uint8_t g_compressedSpirvCache{suffix}[]",
    )
    return src


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--suffix", required=True)
    args = parser.parse_args()
    text = args.input.read_text()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(transform(text, args.suffix))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
