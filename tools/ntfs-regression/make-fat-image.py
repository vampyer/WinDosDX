#!/usr/bin/env python3
"""Create a FAT16 test disk image (raw), without Windows or admin.

The image has an MBR with one FAT16 partition (type 0x06) starting at
sector 2048 and filling the disk, formatted like Windows' FORMAT /FS:FAT
would: 512-byte sectors, two FATs, a 512-entry root directory, and the
volume label FATREG, which the regression payload looks for.

Folders can be copied onto it with 8.3 names (upper case, long names cut
down, clashes numbered NAME~1), for example DOS programs to run in the
guest:

    python make-fat-image.py out.raw [--size-mb 64]
        [--add "D:\\games\\Jazz jack Rabbit=GAMES\\JAZZ" ...]
"""

import argparse
import os
import struct
import sys
import time
from pathlib import Path

SECTOR = 512
PART_START = 2048
LABEL = b"FATREG     "
VALID = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")


def cluster_size(sectors: int) -> int:
    """Sectors per cluster, as FORMAT picks them for FAT16."""
    mb = sectors * SECTOR // (1024 * 1024)
    for limit, spc in ((16, 2), (127, 4), (255, 8), (511, 16), (1023, 32), (2047, 64)):
        if mb <= limit:
            return spc
    raise SystemExit("FAT16 volumes are at most 2 GB")


def dos_time(stamp: float) -> tuple:
    t = time.localtime(max(stamp, 315532800))  # not before 1980
    return (((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)),
            (((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday))


def short_name(name: str, taken: set) -> bytes:
    """An 8.3 name for name that is not in taken (11 bytes, space padded)."""
    upper = name.upper()
    base, dot, ext = upper.rpartition(".")
    if not dot:
        base, ext = upper, ""
    clean = lambda s: bytes(c for c in s.encode("ascii", "replace") if c in VALID)
    base, ext = clean(base) or b"_", clean(ext)[:3]
    candidate = base[:8].ljust(8) + ext.ljust(3)
    n = 1
    while candidate in taken or (len(base) > 8 and n == 1):
        tail = b"~%d" % n
        candidate = (base[:8 - len(tail)] + tail).ljust(8) + ext.ljust(3)
        n += 1
        if candidate not in taken:
            break
    taken.add(candidate)
    return candidate


class Fat16:
    def __init__(self, size_mb: int):
        self.total = size_mb * 1024 * 1024 // SECTOR
        part_sectors = self.total - PART_START
        self.spc = cluster_size(part_sectors)
        self.reserved = 1
        self.root_entries = 512
        root_sectors = self.root_entries * 32 // SECTOR
        fat_sectors = 1
        while True:
            data = part_sectors - self.reserved - 2 * fat_sectors - root_sectors
            clusters = data // self.spc
            if (clusters + 2) * 2 <= fat_sectors * SECTOR:
                break
            fat_sectors += 1
        if not 4085 <= clusters < 65525:
            raise SystemExit(f"{clusters} clusters is not a FAT16 volume")
        self.part_sectors, self.fat_sectors, self.clusters = part_sectors, fat_sectors, clusters
        self.image = bytearray(self.total * SECTOR)
        self.base = PART_START * SECTOR
        self.fat = [0] * (clusters + 2)
        self.fat[0], self.fat[1] = 0xFFF8, 0xFFFF
        self.next_free = 2
        self.root_offset = self.base + (self.reserved + 2 * fat_sectors) * SECTOR
        self.data_offset = self.root_offset + root_sectors * SECTOR
        self.cluster_bytes = self.spc * SECTOR
        self.root = []          # 32-byte entries of the root directory
        self.root_taken = set()

    def alloc(self, count: int) -> list:
        if self.next_free + count > self.clusters + 2:
            raise SystemExit("the image is too small for the files added")
        chain = list(range(self.next_free, self.next_free + count))
        self.next_free += count
        for a, b in zip(chain, chain[1:] + [0xFFFF]):
            self.fat[a] = b
        return chain

    def write_chain(self, data: bytes) -> int:
        """Stores data in new clusters; returns the first cluster (0 if empty)."""
        if not data:
            return 0
        count = (len(data) + self.cluster_bytes - 1) // self.cluster_bytes
        chain = self.alloc(count)
        for i, c in enumerate(chain):
            off = self.data_offset + (c - 2) * self.cluster_bytes
            piece = data[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
            self.image[off:off + len(piece)] = piece
        return chain[0]

    @staticmethod
    def entry(name11: bytes, attr: int, cluster: int, size: int, stamp: float) -> bytes:
        t, d = dos_time(stamp)
        return struct.pack("<11sBBBHHHHHHHI", name11, attr, 0, 0, t, d, d, 0, t, d, cluster, size)

    def add_tree(self, host: Path, parent_cluster: int, taken: set, entries: list) -> None:
        """Adds host's contents to a directory (its entries list)."""
        for child in sorted(host.iterdir(), key=lambda p: p.name.lower()):
            name11 = short_name(child.name, taken)
            stamp = child.stat().st_mtime
            if child.is_dir():
                # The directory's first cluster is taken now, so its children
                # can name it as "..", and written once they are all known.
                first = self.alloc(1)[0]
                sub, sub_taken = [], {b".          ", b"..         "}
                self.add_tree(child, first, sub_taken, sub)
                self.write_dir(sub, parent_cluster, stamp, first)
                entries.append(self.entry(name11, 0x10, first, 0, stamp))
            else:
                data = child.read_bytes()
                entries.append(self.entry(name11, 0x20, self.write_chain(data), len(data), stamp))

    def write_dir(self, sub: list, parent_cluster: int, stamp: float, first: int) -> int:
        """Writes a subdirectory whose first cluster is first."""
        dot = self.entry(b".          ", 0x10, first, 0, stamp)
        dotdot = self.entry(b"..         ", 0x10, parent_cluster, 0, stamp)
        data = dot + dotdot + b"".join(sub)
        count = max(1, (len(data) + self.cluster_bytes - 1) // self.cluster_bytes)
        chain = [first] + (self.alloc(count - 1) if count > 1 else [])
        for a, b in zip(chain, chain[1:] + [0xFFFF]):
            self.fat[a] = b
        for i, c in enumerate(chain):
            off = self.data_offset + (c - 2) * self.cluster_bytes
            piece = data[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
            self.image[off:off + len(piece)] = piece
        return first

    def add(self, host: Path, dos_path: str) -> None:
        """Queues host (a folder) to be copied to dos_path (e.g. GAMES\\JAZZ);
        build() writes the directories."""
        parts = [p for p in dos_path.replace("/", "\\").split("\\") if p]
        node = self.tree_root
        for i, part in enumerate(parts):
            node = node.setdefault(part.upper(), {"__host__": None, "__children__": {}})
            if i == len(parts) - 1:
                node["__host__"] = host
            node = node["__children__"]

    def build(self) -> None:
        def emit(tree: dict, parent_cluster: int, entries: list, taken: set) -> None:
            for name, node in sorted(tree.items()):
                name11 = short_name(name, taken)
                first = self.alloc(1)[0]
                sub, sub_taken = [], {b".          ", b"..         "}
                emit(node["__children__"], first, sub, sub_taken)
                if node["__host__"] is not None:
                    self.add_tree(node["__host__"], first, sub_taken, sub)
                self.write_dir(sub, parent_cluster, time.time(), first)
                entries.append(self.entry(name11, 0x10, first, 0, time.time()))

        emit(self.tree_root, 0, self.root, self.root_taken)

    def finish(self, path: Path) -> None:
        image = self.image
        mbr = bytearray(SECTOR)
        struct.pack_into("<I", mbr, 440, int(time.time()) & 0xFFFFFFFF)
        mbr[446:462] = struct.pack("<B3sB3sII", 0x80, b"\xFE\xFF\xFF", 0x06, b"\xFE\xFF\xFF",
                                   PART_START, self.part_sectors)
        mbr[510:512] = b"\x55\xAA"
        image[0:SECTOR] = mbr

        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = b"MSDOS5.0"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, self.spc, self.reserved, 2,
                         self.root_entries,
                         self.part_sectors if self.part_sectors < 65536 else 0,
                         0xF8, self.fat_sectors, 63, 255, PART_START,
                         self.part_sectors if self.part_sectors >= 65536 else 0)
        struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29,
                         int(time.time() * 7) & 0xFFFFFFFF, LABEL, b"FAT16   ")
        bs[510:512] = b"\x55\xAA"
        image[self.base:self.base + SECTOR] = bs

        fat_bytes = struct.pack("<%dH" % len(self.fat), *self.fat)
        for n in range(2):
            off = self.base + (self.reserved + n * self.fat_sectors) * SECTOR
            image[off:off + len(fat_bytes)] = fat_bytes

        label = bytearray(32)
        label[0:11] = LABEL
        label[11] = 0x08
        root = bytes(label) + b"".join(self.root)
        if len(root) > self.root_entries * 32:
            raise SystemExit("too many entries in the root directory")
        image[self.root_offset:self.root_offset + len(root)] = root
        path.write_bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=Path)
    parser.add_argument("--size-mb", type=int, default=64)
    parser.add_argument("--add", action="append", default=[], metavar="FOLDER=DOSPATH",
                        help="copy FOLDER to DOSPATH on the image (repeatable)")
    args = parser.parse_args()
    if args.image.exists():
        raise SystemExit(f"{args.image} already exists")

    fs = Fat16(args.size_mb)
    fs.tree_root = {}
    for spec in args.add:
        host, _, dos = spec.rpartition("=")
        if not host or not Path(host).is_dir():
            raise SystemExit(f"--add needs an existing FOLDER=DOSPATH: {spec}")
        fs.add(Path(host), dos)
    fs.build()
    fs.finish(args.image)
    used = (fs.next_free - 2) * fs.cluster_bytes // (1024 * 1024)
    print(f"{args.image}: {args.size_mb} MB, FAT16, {fs.clusters} clusters of "
          f"{fs.cluster_bytes} bytes, label FATREG, {used} MB used")
    return 0


if __name__ == "__main__":
    sys.exit(main())
