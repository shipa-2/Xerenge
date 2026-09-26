#!/usr/bin/env python3
"""Describe the runtime-assembled shaders so an installation can rebuild them.

translate_runtime_shaders.sh needs microcode dumped from a running game, which
a fresh installation does not have. Every one of those shaders, though, comes
from something on the disc: a shader container in the executable or in a
graphics/*.obj file with a few vertex-fetch instructions patched at load time,
or (rarely) bare microcode in the executable. This writes down where each one
comes from - by hash, never by content - and which bits of which instructions
the title patches, so build_runtime_shaders.py can redo it from the user's own
disc. Nothing from the game is written: a template is named by the SHA-256 of
its microcode, and a patch is a mask and value over one instruction word.

Usage:
    make_runtime_shader_recipe.py guest-image.bin game-dir dump-dir out.recipe
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rehost_runtime_shader import (  # noqa: E402
    INSTRUCTION_SIZE, find_containers, similarity)

MIN_SIMILARITY = 0.6


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sources(image: Path, game: Path):
    """(name, bytes) of every file a template may live in."""
    yield "image", image.read_bytes()
    for path in sorted((game / "graphics").glob("*.obj")):
        yield str(path.relative_to(game)), path.read_bytes()


def best_template(code: bytes, files):
    best = None
    for name, data in files:
        for offset, virtual_size, physical_size in find_containers(data):
            if physical_size != len(code):
                continue
            micro = data[offset + virtual_size:offset + virtual_size + physical_size]
            score = similarity(micro, code)
            if best is None or score > best[0]:
                best = (score, name, micro)
    return best if best and best[0] >= MIN_SIMILARITY else None


def patches(template: bytes, code: bytes) -> list[str]:
    out = []
    for i in range(len(code) // INSTRUCTION_SIZE):
        a = struct.unpack_from(">3I", template, i * INSTRUCTION_SIZE)
        b = struct.unpack_from(">3I", code, i * INSTRUCTION_SIZE)
        for word in range(3):
            mask = a[word] ^ b[word]
            if mask:
                out.append(f"{i}:{word}:{mask:08x}:{b[word] & mask:08x}")
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("game", type=Path, help="extracted disc (holds default.xex)")
    parser.add_argument("dump", type=Path, help="generated/ucode-dump")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    files = list(sources(args.image, args.game))
    lines = [
        "# Runtime-assembled shaders, rebuilt at install time by",
        "# build_runtime_shaders.py from the user's own disc. Written by",
        "# make_runtime_shader_recipe.py; holds hashes and patch masks only.",
        "#",
        "# <stage> <name> <sha256 of result> template <file> <sha256 of template> [patches]",
        "# <stage> <name> <sha256 of result> raw <file> <declarations for the wrapper>",
    ]
    missing = 0
    for micro in sorted(args.dump.glob("*.bin")):
        name = micro.stem
        stage = name.split("_", 1)[0]
        code = micro.read_bytes()
        found = best_template(code, files)
        if found:
            _, source, template = found
            fields = patches(template, code)
            lines.append(" ".join([stage, name, sha(code), "template", source, sha(template),
                                   *fields]))
            continue
        # No container: look for the bare microcode.
        source = next((n for n, data in files if data.find(code) >= 0), None)
        wrapargs = micro.with_suffix(".wrapargs")
        if source and wrapargs.exists():
            decl = next((l.strip() for l in wrapargs.read_text().splitlines()
                         if l.strip() and not l.startswith("#")), "")
            lines.append(" ".join([stage, name, sha(code), "raw", source, decl]))
            continue
        print(f"{name}: no source found on the disc", file=sys.stderr)
        missing += 1

    args.output.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.output}: {len(lines) - 6} shaders, {missing} without a source")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
