#!/usr/bin/env python3
"""Create a FAT16 test disk image (raw), without Windows or admin.

The image has an MBR with one FAT16 partition (type 0x06) starting at
sector 2048 and filling the disk, formatted like Windows' FORMAT /FS:FAT
would: 512-byte sectors, two FATs, a 512-entry root directory, and the
volume label FATREG, which the regression payload looks for.

Folders can be copied onto it, for example DOS programs or Windows programs
to run in the guest. Every file gets an 8.3 name (upper case, long names
cut down, clashes numbered NAME~1) and, when its real name differs, a long
file name (VFAT) entry, so Windows programs find their DLLs by name:

    python make-fat-image.py out.raw [--size-mb 64]
        [--add "D:\\games\\Jazz jack Rabbit=GAMES\\JAZZ" ...]

It also builds the writable live image: --superfloppy writes a bare volume
(no partition table, as a RAM disk holds it) and --list takes a ReactOS CD
file list (lines "path/on/volume=source"), so

    python make-fat-image.py liveimg.img --superfloppy --label WINDOSDX
        --list boot/livecd.Debug.lst --free-mb 128
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


def short_display(name11: bytes) -> str:
    base, ext = name11[:8].rstrip().decode("ascii"), name11[8:].rstrip().decode("ascii")
    return base + ("." + ext if ext else "")


def lfn_checksum(name11: bytes) -> int:
    total = 0
    for b in name11:
        total = (((total & 1) << 7) + (total >> 1) + b) & 0xFF
    return total


def long_name_entries(name: str, name11: bytes) -> list:
    """The VFAT entries that go before the short entry (none if the 8.3 name
    is the real name)."""
    if name == short_display(name11):
        return []
    units = name.encode("utf-16-le")
    chars = [units[i:i + 2] for i in range(0, len(units), 2)]
    if len(chars) > 255:
        raise SystemExit(f"name too long for FAT: {name}")
    chars.append(b"\0\0")
    while len(chars) % 13:
        chars.append(b"\xff\xff")
    check = lfn_checksum(name11)
    pieces = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    entries = []
    for seq, piece in enumerate(pieces, 1):
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == len(pieces) else 0)
        e[1:11] = b"".join(piece[0:5])
        e[11] = 0x0F
        e[13] = check
        e[14:26] = b"".join(piece[5:11])
        e[28:32] = b"".join(piece[11:13])
        entries.append(bytes(e))
    return list(reversed(entries))


class Fat16:
    def __init__(self, size_mb: int, superfloppy: bool = False, label: bytes = LABEL):
        self.superfloppy = superfloppy
        self.part_start = 0 if superfloppy else PART_START
        self.label = label
        self.total = size_mb * 1024 * 1024 // SECTOR
        part_sectors = self.total - self.part_start
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
        self.base = self.part_start * SECTOR
        self.fat = [0] * (clusters + 2)
        self.fat[0], self.fat[1] = 0xFFF8, 0xFFFF
        self.next_free = 2
        self.root_offset = self.base + (self.reserved + 2 * fat_sectors) * SECTOR
        self.data_offset = self.root_offset + root_sectors * SECTOR
        self.cluster_bytes = self.spc * SECTOR
        self.root = []          # 32-byte entries of the root directory
        self.root_taken = set()
        self.tree_root = {}

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
            entries.extend(long_name_entries(child.name, name11))
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

    def _dir_node(self, parts: list) -> dict:
        """The tree node of a directory, made on the way (names keep their case;
        lookups ignore it, as FAT does)."""
        node = {"__children__": self.tree_root}
        for part in parts:
            children = node["__children__"]
            node = children.setdefault(part.lower(), {"__name__": part, "__host__": None,
                                                      "__children__": {}, "__files__": {}})
        return node

    @staticmethod
    def _split(path: str) -> list:
        return [p for p in path.replace("/", "\\").split("\\") if p]

    def add(self, host: Path, dos_path: str) -> None:
        """Queues host (a folder) to be copied to dos_path (e.g. GAMES\\JAZZ);
        build() writes the directories."""
        self._dir_node(self._split(dos_path))["__host__"] = host

    def add_file(self, source: Path, dos_path: str) -> None:
        """Queues one file to be stored as dos_path (folders made as needed)."""
        parts = self._split(dos_path)
        self._dir_node(parts[:-1])["__files__"][parts[-1].lower()] = (parts[-1], source)

    def add_list(self, listing: Path) -> None:
        """Queues the entries of a ReactOS CD file list ("path=source" lines);
        a source that is a folder makes (and fills) the folder."""
        for line in listing.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line or "=" not in line:
                continue
            dest, source = line.split("=", 1)
            source = Path(source)
            if source.is_dir():
                node = self._dir_node(self._split(dest))
                if any(source.iterdir()):
                    node["__host__"] = source
            elif source.is_file():
                self.add_file(source, dest)
            else:
                raise SystemExit(f"{listing}: missing source {source}")

    def build(self) -> None:
        def emit(tree: dict, parent_cluster: int, entries: list, taken: set) -> None:
            for key, node in sorted(tree.items()):
                name = node["__name__"]
                name11 = short_name(name, taken)
                first = self.alloc(1)[0]
                sub, sub_taken = [], {b".          ", b"..         "}
                emit(node["__children__"], first, sub, sub_taken)
                for _, (file_name, source) in sorted(node["__files__"].items()):
                    file11 = short_name(file_name, sub_taken)
                    data = source.read_bytes()
                    sub.extend(long_name_entries(file_name, file11))
                    sub.append(self.entry(file11, 0x20, self.write_chain(data), len(data),
                                          source.stat().st_mtime))
                if node["__host__"] is not None:
                    self.add_tree(node["__host__"], first, sub_taken, sub)
                self.write_dir(sub, parent_cluster, time.time(), first)
                entries.extend(long_name_entries(name, name11))
                entries.append(self.entry(name11, 0x10, first, 0, time.time()))

        emit(self.tree_root, 0, self.root, self.root_taken)

    def finish(self, path: Path) -> None:
        image = self.image
        if not self.superfloppy:
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
                         0xF8, self.fat_sectors, 63, 255, self.part_start,
                         self.part_sectors if self.part_sectors >= 65536 else 0)
        struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29,
                         int(time.time() * 7) & 0xFFFFFFFF, self.label, b"FAT16   ")
        bs[510:512] = b"\x55\xAA"
        image[self.base:self.base + SECTOR] = bs

        fat_bytes = struct.pack("<%dH" % len(self.fat), *self.fat)
        for n in range(2):
            off = self.base + (self.reserved + n * self.fat_sectors) * SECTOR
            image[off:off + len(fat_bytes)] = fat_bytes

        label = bytearray(32)
        label[0:11] = self.label
        label[11] = 0x08
        root = bytes(label) + b"".join(self.root)
        if len(root) > self.root_entries * 32:
            raise SystemExit("too many entries in the root directory")
        image[self.root_offset:self.root_offset + len(root)] = root
        path.write_bytes(image)


def content_mb(listings: list, adds: list) -> int:
    """Room the files need, with slack for cluster rounding and directories."""
    total = count = 0
    def walk(folder: Path):
        nonlocal total, count
        for f in folder.rglob("*"):
            if f.is_file():
                total += f.stat().st_size
                count += 1
    for listing in listings:
        for line in listing.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                source = Path(line.strip().split("=", 1)[1])
                if source.is_file():
                    total += source.stat().st_size
                    count += 1
                elif source.is_dir():
                    walk(source)
    for spec in adds:
        walk(Path(spec.rpartition("=")[0]))
    # Up to 32 KB lost per file to cluster rounding on the largest volumes.
    return (total + count * 32 * 1024) // (1024 * 1024) + 16


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=Path)
    parser.add_argument("--size-mb", type=int, default=64)
    parser.add_argument("--add", action="append", default=[], metavar="FOLDER=DOSPATH",
                        help="copy FOLDER to DOSPATH on the image (repeatable)")
    parser.add_argument("--list", type=Path, action="append", default=[],
                        help="add the files of a ReactOS CD file list (path=source lines)")
    parser.add_argument("--superfloppy", action="store_true",
                        help="a bare volume without a partition table (for a RAM disk)")
    parser.add_argument("--label", default="FATREG", help="volume label (11 characters at most)")
    parser.add_argument("--free-mb", type=int,
                        help="size the image to its contents plus this much free space "
                             "(instead of --size-mb)")
    parser.add_argument("--force", action="store_true", help="replace an existing image")
    args = parser.parse_args()
    if args.image.exists() and not args.force:
        raise SystemExit(f"{args.image} already exists")

    label = args.label.upper().encode("ascii")[:11].ljust(11)
    size_mb = args.size_mb
    if args.free_mb is not None:
        size_mb = content_mb(args.list, args.add) + args.free_mb
    fs = Fat16(size_mb, args.superfloppy, label)
    for listing in args.list:
        fs.add_list(listing)
    for spec in args.add:
        host, _, dos = spec.rpartition("=")
        if not host or not Path(host).is_dir():
            raise SystemExit(f"--add needs an existing FOLDER=DOSPATH: {spec}")
        fs.add(Path(host), dos)
    fs.build()
    fs.finish(args.image)
    used = (fs.next_free - 2) * fs.cluster_bytes // (1024 * 1024)
    print(f"{args.image}: {size_mb} MB, FAT16{' superfloppy' if args.superfloppy else ''}, "
          f"{fs.clusters} clusters of {fs.cluster_bytes} bytes, label {label.decode().strip()}, "
          f"{used} MB used")
    return 0


if __name__ == "__main__":
    sys.exit(main())
