#!/usr/bin/env python3
"""Rebuild the runtime-assembled shaders from the user's disc, by recipe.

Reads tools/runtime_shaders.recipe (see make_runtime_shader_recipe.py) and
writes one container per shader into the output directory, ready for
XenosRecomp - the same containers translate_runtime_shaders.sh makes from a
running game's dumps, without having to run the game first. Each result is
checked against the SHA-256 in the recipe, so a different disc revision is
reported rather than turned into wrong shaders.

Usage:
    build_runtime_shaders.py recipe guest-image.bin game-dir out-dir
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rehost_runtime_shader import INSTRUCTION_SIZE, find_containers  # noqa: E402

TOOLS = Path(__file__).resolve().parent
WRAPPERS = {"VS": TOOLS / "wrap_runtime_vertex_shader.py",
            "PS": TOOLS / "wrap_runtime_pixel_shader.py"}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Sources:
    def __init__(self, image: Path, game: Path):
        self.image = image
        self.game = game
        self.data: dict[str, bytes] = {}
        self.templates: dict[str, dict[str, tuple[int, int, int]]] = {}

    def read(self, name: str) -> bytes:
        if name not in self.data:
            path = self.image if name == "image" else self.game / name
            self.data[name] = path.read_bytes()
        return self.data[name]

    def template(self, name: str, digest: str):
        """(container bytes, virtual size) of the container whose microcode hashes to digest."""
        if name not in self.templates:
            data = self.read(name)
            # The first container with that microcode, as the recipe's writer
            # picks - several can share it with different declarations.
            found = {}
            for o, v, p in find_containers(data):
                found.setdefault(sha(data[o + v:o + v + p]), (o, v, p))
            self.templates[name] = found
        found = self.templates[name].get(digest)
        if not found:
            return None
        offset, virtual_size, physical_size = found
        return bytearray(self.read(name)[offset:offset + virtual_size + physical_size]), virtual_size

    def raw(self, name: str, digest: str, size: int):
        data = self.read(name)
        for offset in range(0, len(data) - size + 1, 4):
            chunk = data[offset:offset + size]
            if sha(chunk) == digest:
                return chunk
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("recipe", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("game", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    sources = Sources(args.image, args.game)
    built = failed = 0
    for line in args.recipe.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        stage, name, digest, kind, source, *rest = line.split()
        target = args.output / f"{name}.container"
        try:
            if kind == "template":
                found = sources.template(source, rest[0])
                if not found:
                    raise LookupError(f"no template in {source}")
                container, virtual_size = found
                for field in rest[1:]:
                    index, word, mask, value = field.split(":")
                    at = virtual_size + int(index) * INSTRUCTION_SIZE + int(word) * 4
                    mask, value = int(mask, 16), int(value, 16)
                    old = struct.unpack_from(">I", container, at)[0]
                    struct.pack_into(">I", container, at, (old & ~mask) | value)
                if sha(bytes(container[virtual_size:])) != digest:
                    raise LookupError("patched result does not match")
                target.write_bytes(container)
            elif kind == "raw":
                size = int(name.rsplit("_", 1)[1])
                code = sources.raw(source, digest, size)
                if code is None:
                    raise LookupError(f"not found in {source}")
                micro = args.output / f"{name}.bin"
                micro.write_bytes(code)
                subprocess.run([sys.executable, str(WRAPPERS[stage]), str(micro), str(target),
                                *rest], check=True, stdout=subprocess.DEVNULL)
                micro.unlink()
            else:
                raise LookupError(f"unknown kind {kind}")
            built += 1
        except (LookupError, OSError, subprocess.CalledProcessError) as error:
            print(f"{name}: {error}", file=sys.stderr)
            failed += 1

    print(f"runtime shaders: {built} rebuilt from the disc, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
