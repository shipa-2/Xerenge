#!/usr/bin/env python3
"""Extract an Xbox 360 game disc image into a directory.

The image is XDVDFS, not ISO 9660, so the usual tools do not read it: 7-Zip
sees a stray UDF header and stops. Rather than depend on extract-xiso being
installed, this reads the format directly - a volume descriptor naming the root
directory, and directory tables that are binary trees of entries.
"""

import argparse
import hashlib
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# A game partition can start at the front of the image or after a video
# partition, depending on the disc generation.
PARTITION_OFFSETS = (0x00000000, 0x0000FD90, 0x00002080, 0x0FD90000, 0x18300000, 0x1FB20000)
ATTRIBUTE_DIRECTORY = 0x10


class Image:
    def __init__(self, path):
        self.file = open(path, "rb")
        self.base = self._find_partition()

    def _find_partition(self):
        for base in PARTITION_OFFSETS:
            self.file.seek(base + 32 * SECTOR)
            if self.file.read(len(MAGIC)) == MAGIC:
                return base
        raise SystemExit("not an Xbox game disc image: no volume found")

    def sector(self, number, count=1):
        self.file.seek(self.base + number * SECTOR)
        return self.file.read(count * SECTOR)

    def root(self):
        descriptor = self.sector(32)
        start, size = struct.unpack_from("<II", descriptor, 0x14)
        return start, size


def walk(image, sector, size, offset=0):
    """Yield (name, start_sector, length, is_directory) from a directory table."""
    if size == 0:
        return
    table = image.sector(sector, (size + SECTOR - 1) // SECTOR)
    pending = [offset]
    seen = set()
    while pending:
        at = pending.pop() * 4
        if at in seen or at + 14 > len(table):
            continue
        seen.add(at)
        left, right, start, length, attributes, name_length = struct.unpack_from(
            "<HHIIBB", table, at)
        if left == 0xFFFF:
            continue
        name = table[at + 14:at + 14 + name_length].decode("latin-1")
        if not name:
            continue
        yield name, start, length, bool(attributes & ATTRIBUTE_DIRECTORY)
        if left:
            pending.append(left)
        if right:
            pending.append(right)


def copy_file(image, start, length, destination):
    with open(destination, "wb") as out:
        remaining = length
        image.file.seek(image.base + start * SECTOR)
        while remaining > 0:
            chunk = image.file.read(min(1 << 20, remaining))
            if not chunk:
                raise SystemExit("the image ends partway through %s" % destination)
            out.write(chunk)
            remaining -= len(chunk)


def extract(image, sector, size, destination, listing_only, report):
    os.makedirs(destination, exist_ok=True) if not listing_only else None
    for name, start, length, is_directory in walk(image, sector, size):
        path = os.path.join(destination, name)
        if is_directory:
            report(path + "/", 0)
            extract(image, start, length, path, listing_only, report)
        else:
            report(path, length)
            if not listing_only:
                copy_file(image, start, length, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", help="path to the disc image")
    parser.add_argument("destination", nargs="?", help="where to extract it")
    parser.add_argument("--list", action="store_true", help="list the contents and extract nothing")
    parser.add_argument("--sha256", help="expected SHA256 of the image")
    arguments = parser.parse_args()

    if not arguments.list and not arguments.destination:
        parser.error("a destination is needed (or --list)")

    if arguments.sha256:
        digest = hashlib.sha256()
        size = os.path.getsize(arguments.image)
        done = 0
        with open(arguments.image, "rb") as handle:
            while True:
                chunk = handle.read(1 << 22)
                if not chunk:
                    break
                digest.update(chunk)
                done += len(chunk)
                print("\rchecking the image: %3d%%" % (100 * done // size), end="", file=sys.stderr)
        print("\r", end="", file=sys.stderr)
        if digest.hexdigest() != arguments.sha256.lower():
            raise SystemExit("checksum mismatch:\n  got      %s\n  expected %s"
                             % (digest.hexdigest(), arguments.sha256.lower()))
        print("checksum matches", file=sys.stderr)

    image = Image(arguments.image)
    print("volume found at offset 0x%X" % image.base, file=sys.stderr)
    sector, size = image.root()

    files = [0]
    total = [0]

    def report(path, length):
        if not path.endswith("/"):
            files[0] += 1
            total[0] += length
        if arguments.list:
            print(path if path.endswith("/") else "%s\t%d" % (path, length))
        elif files[0] % 200 == 0:
            print("\rfiles written: %d" % files[0], end="", file=sys.stderr)

    extract(image, sector, size, arguments.destination or ".", arguments.list, report)
    if not arguments.list:
        print("\r", end="", file=sys.stderr)
    print("%d files, %.1f GiB in total" % (files[0], total[0] / (1 << 30)), file=sys.stderr)


if __name__ == "__main__":
    main()
