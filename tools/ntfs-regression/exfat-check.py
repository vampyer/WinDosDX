#!/usr/bin/env python3
"""Offline consistency check for exFAT volumes (images or VHDs).

Checks what chkdsk would, following the exFAT specification:

- the boot region: signature, checksum, backup copy, volume flags;
- the FAT's reserved entries and every cluster chain it holds;
- the up-case table's checksum;
- every directory entry set: checksum, structure, name length, name hash,
  duplicate names, flags, and sizes against the clusters allocated;
- that no cluster belongs to two files, and that the allocation bitmap marks
  exactly the clusters in use.

Findings are ERROR (corruption), WARN (what chkdsk would silently repair,
such as leaked clusters) or NOTE. The exit status is 1 when there is any ERROR.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

FILE = 0x85
STREAM = 0xC0
NAME = 0xC1
BITMAP = 0x81
UPCASE = 0x82
LABEL = 0x83
EOF = 0xFFFFFFFF
BAD = 0xFFFFFFF7
ATTR_DIRECTORY = 0x10


class Report:
    def __init__(self) -> None:
        self.items: list[tuple[str, str]] = []

    def error(self, text: str) -> None:
        self.items.append(("ERROR", text))

    def warn(self, text: str) -> None:
        self.items.append(("WARN", text))

    def note(self, text: str) -> None:
        self.items.append(("NOTE", text))

    def count(self, level: str) -> int:
        return sum(1 for item_level, _ in self.items if item_level == level)


def checksum16(data: bytes, skip: tuple[int, ...] = ()) -> int:
    value = 0
    for i, byte in enumerate(data):
        if i in skip:
            continue
        value = ((0x8000 if value & 1 else 0) + (value >> 1) + byte) & 0xFFFF
    return value


def checksum32(data: bytes, skip: tuple[int, ...] = ()) -> int:
    value = 0
    for i, byte in enumerate(data):
        if i in skip:
            continue
        value = ((0x80000000 if value & 1 else 0) + (value >> 1) + byte) & 0xFFFFFFFF
    return value


class Volume:
    def __init__(self, image: bytes, report: Report):
        self.image = image
        self.report = report
        self.base = self.find_partition()
        boot = image[self.base:self.base + 512]
        if boot[3:11] != b"EXFAT   " or boot[510:512] != b"\x55\xAA":
            raise SystemExit("No exFAT boot sector found")
        (self.partition_offset, self.volume_length, self.fat_offset, self.fat_length,
         self.heap_offset, self.cluster_count, self.root_cluster, self.serial,
         self.revision, self.flags) = struct.unpack_from("<QQIIIIIIHH", boot, 0x40)
        self.sector_size = 1 << boot[0x6C]
        self.cluster_size = self.sector_size << boot[0x6D]
        self.fat_count = boot[0x6E]
        self.percent_in_use = boot[0x70]
        active = 1 if (self.fat_count == 2 and self.flags & 1) else 0
        self.fat_base = self.base + (self.fat_offset + active * self.fat_length) * self.sector_size

    def find_partition(self) -> int:
        mbr = self.image[:512]
        if mbr[3:11] == b"EXFAT   ":
            return 0
        if mbr[510:512] == b"\x55\xAA":
            for i in range(4):
                entry = mbr[446 + 16 * i:462 + 16 * i]
                if entry[4] == 0x07:
                    return struct.unpack_from("<I", entry, 8)[0] * 512
        raise SystemExit("No exFAT partition found")

    def valid(self, cluster: int) -> bool:
        return 2 <= cluster < self.cluster_count + 2

    def fat(self, cluster: int) -> int:
        return struct.unpack_from("<I", self.image, self.fat_base + 4 * cluster)[0]

    def cluster_data(self, cluster: int) -> bytes:
        start = self.base + self.heap_offset * self.sector_size + (cluster - 2) * self.cluster_size
        return self.image[start:start + self.cluster_size]

    def chain(self, first: int, no_fat_chain: bool, clusters: int | None, what: str) -> list[int] | None:
        """The clusters of an allocation; clusters=None follows the FAT to its end."""
        if first == 0:
            return []
        if not self.valid(first):
            self.report.error(f"{what}: first cluster {first} is outside the cluster heap")
            return None
        if no_fat_chain:
            result = [first + i for i in range(clusters or 0)]
            if result and not self.valid(result[-1]):
                self.report.error(f"{what}: contiguous run {first}+{clusters} runs past the heap")
                return None
            return result
        result = [first]
        seen = {first}
        while True:
            if clusters is not None and len(result) == clusters:
                if self.fat(result[-1]) != EOF:
                    self.report.error(f"{what}: FAT chain continues past its {clusters} clusters "
                                      f"(entry {self.fat(result[-1]):#x} at cluster {result[-1]})")
                return result
            nxt = self.fat(result[-1])
            if nxt == EOF:
                if clusters is not None:
                    self.report.error(f"{what}: FAT chain ends after {len(result)} of {clusters} clusters")
                return result
            if nxt == BAD or not self.valid(nxt):
                self.report.error(f"{what}: FAT entry {nxt:#x} at cluster {result[-1]} is not a valid link")
                return None
            if nxt in seen:
                self.report.error(f"{what}: FAT chain loops at cluster {nxt}")
                return None
            seen.add(nxt)
            result.append(nxt)

    def read(self, clusters: list[int], length: int | None = None) -> bytes:
        data = b"".join(self.cluster_data(c) for c in clusters)
        return data if length is None else data[:length]


def check_boot(vol: Volume, report: Report) -> None:
    region = vol.image[vol.base:vol.base + 12 * vol.sector_size]
    expected = checksum32(region[:11 * vol.sector_size], skip=(106, 107, 112))
    stored = region[11 * vol.sector_size:12 * vol.sector_size]
    if any(struct.unpack_from("<I", stored, i)[0] != expected for i in range(0, vol.sector_size, 4)):
        report.error("boot region checksum does not match")
    backup = vol.image[vol.base + 12 * vol.sector_size:vol.base + 24 * vol.sector_size]
    main = bytearray(region)
    backup = bytearray(backup)
    for buf in (main, backup):
        buf[106:108] = b"\0\0"
        buf[112] = 0
    if main[:11 * vol.sector_size] != backup[:11 * vol.sector_size]:
        report.warn("backup boot region differs from the main one")
    if vol.flags & 2:
        report.warn("volume is marked dirty (not cleanly dismounted)")
    if vol.flags & 4:
        report.warn("volume reports media failure")
    if (vol.revision >> 8) != 1:
        report.error(f"unsupported file system revision {vol.revision:#x}")
    if vol.fat(0) != 0xFFFFFFF8 or vol.fat(1) != 0xFFFFFFFF:
        report.error(f"FAT reserved entries are {vol.fat(0):#x}, {vol.fat(1):#x}")


def load_upcase(vol: Volume, entry: bytes, owner: dict[int, str], report: Report) -> list[int] | None:
    table_checksum, = struct.unpack_from("<I", entry, 4)
    first, length = struct.unpack_from("<IQ", entry, 20)
    clusters = vol.chain(first, False, -(-length // vol.cluster_size), "$UpCase")
    if clusters is None:
        return None
    claim(owner, clusters, "$UpCase", report)
    raw = vol.read(clusters, length)
    if checksum32(raw) != table_checksum:
        report.error("up-case table checksum does not match")
    table = list(range(0x10000))
    words = struct.unpack(f"<{len(raw) // 2}H", raw[:len(raw) // 2 * 2])
    i = index = 0
    while i < len(words) and index < 0x10000:
        if words[i] == 0xFFFF and i + 1 < len(words):
            index += words[i + 1]
            i += 2
        else:
            table[index] = words[i]
            index += 1
            i += 1
    return table


def claim(owner: dict[int, str], clusters: list[int], what: str, report: Report) -> None:
    for cluster in clusters:
        if cluster in owner:
            report.error(f"cluster {cluster} belongs to both {owner[cluster]} and {what}")
        else:
            owner[cluster] = what


def name_hash(name: str, upcase: list[int]) -> int:
    value = 0
    for unit in struct.unpack(f"<{len(name.encode('utf-16le')) // 2}H", name.encode("utf-16le")):
        up = upcase[unit]
        for byte in (up & 0xFF, up >> 8):
            value = ((0x8000 if value & 1 else 0) + (value >> 1) + byte) & 0xFFFF
    return value


class Checker:
    def __init__(self, vol: Volume, report: Report):
        self.vol = vol
        self.report = report
        self.owner: dict[int, str] = {}
        self.upcase: list[int] = list(range(0x10000))
        self.files = 0
        self.directories = 0

    def check_directory(self, data: bytes, path: str, is_root: bool) -> None:
        seen_names: dict[tuple[int, ...], str] = {}
        i = 0
        ended = False
        while i < len(data):
            entry_type = data[i]
            if ended:
                if entry_type != 0:
                    self.report.warn(f"{path}: entry {i // 32} of type {entry_type:#x} follows the end marker")
                i += 32
                continue
            if entry_type == 0:
                ended = True
                i += 32
                continue
            if entry_type == FILE:
                i = self.check_set(data, i, path, seen_names)
                continue
            if entry_type in (BITMAP, UPCASE, LABEL) and not is_root:
                self.report.error(f"{path}: system entry {entry_type:#x} outside the root directory")
            elif entry_type & 0x80 and entry_type & 0x40:
                self.report.warn(f"{path}: stray secondary entry {entry_type:#x} at index {i // 32}")
            i += 32

    def check_set(self, data: bytes, i: int, path: str, seen_names: dict) -> int:
        secondary = data[i + 1]
        where = f"{path}: entry set at index {i // 32}"
        if secondary < 2 or i + 32 * (secondary + 1) > len(data):
            self.report.error(f"{where}: secondary count {secondary} is invalid")
            return i + 32
        entry_set = data[i:i + 32 * (secondary + 1)]
        stored, = struct.unpack_from("<H", entry_set, 2)
        if checksum16(entry_set, skip=(2, 3)) != stored:
            self.report.error(f"{where}: set checksum {stored:#06x} does not match")
        stream = entry_set[32:64]
        if stream[0] != STREAM:
            self.report.error(f"{where}: second entry is {stream[0]:#x}, not a stream extension")
            return i + 32
        name_length = stream[3]
        name_entries = -(-name_length // 15)
        if name_length == 0 or name_entries > secondary - 1:
            self.report.error(f"{where}: name length {name_length} does not fit its {secondary - 1} entries")
            return i + 32 * (secondary + 1)
        raw_name = b""
        for k in range(name_entries):
            name_entry = entry_set[64 + 32 * k:96 + 32 * k]
            if name_entry[0] != NAME:
                self.report.error(f"{where}: entry {2 + k} is {name_entry[0]:#x}, not a file name")
                return i + 32 * (secondary + 1)
            raw_name += name_entry[2:32]
        name = raw_name.decode("utf-16le", "replace")[:name_length]
        full = f"{path}\\{name}" if path != "\\" else f"\\{name}"
        attributes, = struct.unpack_from("<H", entry_set, 4)
        flags = stream[1]
        stored_hash, = struct.unpack_from("<H", stream, 4)
        valid_length, = struct.unpack_from("<Q", stream, 8)
        first, length = struct.unpack_from("<IQ", stream, 20)

        if name_hash(name, self.upcase) != stored_hash:
            self.report.error(f"{full}: name hash {stored_hash:#06x} does not match the name")
        key = tuple(self.upcase[u] for u in struct.unpack(f"<{len(name.encode('utf-16le')) // 2}H",
                                                            name.encode("utf-16le")))
        if key in seen_names:
            self.report.error(f"{full}: same name (ignoring case) as {seen_names[key]!r}")
        seen_names[key] = name
        for ch in name:
            if ord(ch) < 0x20 or ch in '"*/:<>?\\|':
                self.report.error(f"{full}: name has the illegal character {ch!r}")
                break
        if not flags & 1:
            if first or length:
                self.report.error(f"{full}: allocation-possible flag is clear but it has clusters")
        if valid_length > length:
            self.report.error(f"{full}: valid data length {valid_length} exceeds data length {length}")

        clusters_needed = -(-length // self.vol.cluster_size)
        if first == 0 and length != 0:
            self.report.error(f"{full}: {length} bytes of data but no first cluster")
            return i + 32 * (secondary + 1)
        clusters = self.vol.chain(first, bool(flags & 2), clusters_needed, full)
        if clusters is not None:
            claim(self.owner, clusters, full, self.report)

        if attributes & ATTR_DIRECTORY:
            self.directories += 1
            if length % self.vol.cluster_size:
                self.report.error(f"{full}: directory size {length} is not a whole number of clusters")
            if length > 256 * 1024 * 1024:
                self.report.error(f"{full}: directory is larger than 256 MB")
            if valid_length != length:
                self.report.error(f"{full}: directory valid length {valid_length} differs from its size {length}")
            if clusters:
                self.check_directory(self.vol.read(clusters, length), full, False)
        else:
            self.files += 1
        return i + 32 * (secondary + 1)

    def run(self) -> None:
        vol = self.vol
        root = vol.chain(vol.root_cluster, False, None, "root directory")
        if root is None:
            return
        claim(self.owner, root, "root directory", self.report)
        root_data = vol.read(root)

        bitmap = None
        bitmaps = 0
        for i in range(0, len(root_data), 32):
            entry = root_data[i:i + 32]
            if entry[0] == 0:
                break
            if entry[0] == BITMAP:
                bitmaps += 1
                first, length = struct.unpack_from("<IQ", entry, 20)
                clusters = vol.chain(first, False, -(-length // vol.cluster_size), "$Bitmap")
                if clusters is not None:
                    claim(self.owner, clusters, "$Bitmap", self.report)
                    if entry[1] & 1 == 0:
                        bitmap = vol.read(clusters, length)
                if length < -(-vol.cluster_count // 8):
                    self.report.error(f"$Bitmap is {length} bytes; {vol.cluster_count} clusters need more")
            elif entry[0] == UPCASE:
                table = load_upcase(vol, entry, self.owner, self.report)
                if table:
                    self.upcase = table
        if bitmaps != vol.fat_count:
            self.report.error(f"{bitmaps} allocation bitmap entries for {vol.fat_count} FATs")

        self.check_directory(root_data, "\\", True)

        if bitmap is None:
            self.report.error("no allocation bitmap to compare")
            return
        used_free = []
        leaked = []
        for n in range(vol.cluster_count):
            cluster = n + 2
            marked = bool(bitmap[n // 8] & (1 << (n % 8)))
            if cluster in self.owner and not marked:
                used_free.append(cluster)
            elif marked and cluster not in self.owner:
                leaked.append(cluster)
        for cluster in used_free[:20]:
            self.report.error(f"cluster {cluster} is used by {self.owner[cluster]} but free in the bitmap")
        if len(used_free) > 20:
            self.report.error(f"... and {len(used_free) - 20} more used clusters free in the bitmap")
        if leaked:
            self.report.warn(f"{len(leaked)} cluster(s) marked in the bitmap but owned by nothing")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=Path, help="raw disk image, partition image or fixed VHD")
    args = parser.parse_args()
    # File names can hold any Unicode; never let the console code page abort a check.
    sys.stdout.reconfigure(errors="backslashreplace")

    report = Report()
    vol = Volume(args.image.read_bytes(), report)
    check_boot(vol, report)
    checker = Checker(vol, report)
    checker.run()

    used = len(checker.owner)
    print(f"{args.image}: exFAT, {vol.cluster_count} clusters of {vol.cluster_size} bytes, "
          f"{checker.files} files, {checker.directories} directories, {used} clusters in use")
    for level, text in report.items:
        print(f"{level:5} {text}")
    print(f"{report.count('ERROR')} error(s), {report.count('WARN')} warning(s), "
          f"{report.count('NOTE')} note(s)")
    return 1 if report.count("ERROR") else 0


if __name__ == "__main__":
    sys.exit(main())
