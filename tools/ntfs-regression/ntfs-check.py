#!/usr/bin/env python3
"""Offline consistency check for NTFS regression images.

Windows chkdsk cannot check the harness's images: they carry only the system
files the ReactOS driver needs, so Windows sees them as RAW. This script does
the structural cross-checks chkdsk would do on what is there:

- every file record parses (update sequence, attribute chain, run lists);
- the $MFT bitmap matches the records that are in use;
- no cluster is owned twice, and the $Bitmap matches the owned clusters;
- $MFTMirr matches the start of $MFT;
- every directory index is a well-formed, ordered B-tree whose entries point
  at live records with a matching name and parent, and every named record is
  indexed by its parent.

Findings are ERROR (corruption), WARN (what chkdsk would silently repair, such
as leaked clusters) or NOTE (known shortcuts of the harness-built format).
The exit status is 1 when there is any ERROR.
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path

ATTR_STANDARD_INFORMATION = 0x10
ATTR_FILE_NAME = 0x30
ATTR_DATA = 0x80
ATTR_INDEX_ROOT = 0x90
ATTR_INDEX_ALLOCATION = 0xA0
ATTR_BITMAP = 0xB0
ATTR_END = 0xFFFFFFFF

RECORD_IN_USE = 0x0001
RECORD_IS_DIRECTORY = 0x0002

INDEX_ENTRY_NODE = 0x01
INDEX_ENTRY_END = 0x02

FIRST_USER_RECORD = 16
# Windows also reserves 16-23 for $MFT extension records and marks them used.
RESERVED_RECORDS = 24
ROOT_RECORD = 5
NAMESPACE_DOS = 2


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


def apply_fixups(buf: bytes, sector_size: int) -> tuple[bytearray, str | None]:
    data = bytearray(buf)
    usa_offset, usa_count = struct.unpack_from("<HH", data, 4)
    if usa_count == 0 or usa_offset + 2 * usa_count > len(data):
        return data, f"bad update sequence array (offset {usa_offset}, count {usa_count})"
    if (usa_count - 1) * sector_size != len(data):
        return data, f"update sequence covers {usa_count - 1} sectors, block is {len(data)} bytes"
    usn = data[usa_offset:usa_offset + 2]
    for i in range(1, usa_count):
        end = i * sector_size - 2
        if data[end:end + 2] != usn:
            return data, f"torn write: sector {i} does not carry the update sequence number"
        data[end:end + 2] = data[usa_offset + 2 * i:usa_offset + 2 * i + 2]
    return data, None


def decode_runs(buf: bytes) -> list[tuple[int | None, int]] | None:
    runs: list[tuple[int | None, int]] = []
    lcn = 0
    i = 0
    while i < len(buf) and buf[i]:
        header = buf[i]
        length_size = header & 0x0F
        offset_size = header >> 4
        i += 1
        if length_size == 0 or i + length_size + offset_size > len(buf):
            return None
        length = int.from_bytes(buf[i:i + length_size], "little")
        i += length_size
        if offset_size:
            lcn += int.from_bytes(buf[i:i + offset_size], "little", signed=True)
            runs.append((lcn, length))
        else:
            runs.append((None, length))
        i += offset_size
    return runs if i < len(buf) else None


class Attribute:
    def __init__(self, raw: bytes):
        self.type, self.length = struct.unpack_from("<II", raw, 0)
        self.nonresident = raw[8] != 0
        name_length = raw[9]
        name_offset, = struct.unpack_from("<H", raw, 0x0A)
        self.name = raw[name_offset:name_offset + 2 * name_length].decode("utf-16le", "replace")
        self.raw = raw
        self.runs: list[tuple[int | None, int]] | None = None
        if self.nonresident:
            self.lowest_vcn, self.highest_vcn = struct.unpack_from("<QQ", raw, 0x10)
            runs_offset, = struct.unpack_from("<H", raw, 0x20)
            self.allocated, self.size, self.initialized = struct.unpack_from("<QQQ", raw, 0x28)
            self.runs = decode_runs(raw[runs_offset:])
            self.value = b""
        else:
            value_length, value_offset = struct.unpack_from("<IH", raw, 0x10)
            self.value = raw[value_offset:value_offset + value_length]
            self.size = value_length
            self.value_ok = value_offset + value_length <= len(raw)


class FileName:
    def __init__(self, value: bytes):
        self.parent_ref, = struct.unpack_from("<Q", value, 0)
        self.parent = self.parent_ref & 0xFFFFFFFFFFFF
        self.parent_seq = self.parent_ref >> 48
        self.real_size, = struct.unpack_from("<Q", value, 0x30)
        length = value[0x40]
        self.namespace = value[0x41]
        self.name = value[0x42:0x42 + 2 * length].decode("utf-16le", "replace")


class Record:
    def __init__(self, number: int, raw: bytes, sector_size: int, report: Report):
        self.number = number
        self.valid = raw[:4] == b"FILE"
        self.attrs: list[Attribute] = []
        self.flags = 0
        self.sequence = 0
        self.link_count = 0
        self.base_record = 0
        self.problem: str | None = None
        if not self.valid:
            return
        data, err = apply_fixups(raw, sector_size)
        self.sequence, self.link_count, attr_offset, self.flags, used, allocated = \
            struct.unpack_from("<HHHHII", data, 0x10)
        self.base_record = struct.unpack_from("<Q", data, 0x20)[0] & 0xFFFFFFFFFFFF
        if err:
            self.problem = err
            return
        if allocated != len(raw):
            self.problem = f"allocated size {allocated} != record size {len(raw)}"
            return
        if used > allocated or attr_offset % 8 or attr_offset >= used:
            self.problem = f"bad header (attribute offset {attr_offset}, bytes in use {used})"
            return
        offset = attr_offset
        while True:
            if offset + 4 > used:
                self.problem = "attribute list runs past bytes in use without an end marker"
                return
            attr_type, = struct.unpack_from("<I", data, offset)
            if attr_type == ATTR_END:
                break
            attr_length, = struct.unpack_from("<I", data, offset + 4)
            if attr_length < 0x18 or attr_length % 8 or offset + attr_length > used:
                self.problem = f"attribute {attr_type:#x} at {offset:#x} has bad length {attr_length}"
                return
            attr = Attribute(bytes(data[offset:offset + attr_length]))
            if attr.nonresident and attr.runs is None:
                self.problem = f"attribute {attr_type:#x} has an undecodable run list"
                return
            if not attr.nonresident and not attr.value_ok:
                self.problem = f"attribute {attr_type:#x} value runs past the attribute"
                return
            self.attrs.append(attr)
            offset += attr_length

    @property
    def in_use(self) -> bool:
        return bool(self.flags & RECORD_IN_USE)

    def find(self, attr_type: int, name: str = "") -> Attribute | None:
        for attr in self.attrs:
            if attr.type == attr_type and attr.name == name:
                return attr
        return None

    def file_names(self) -> list[FileName]:
        return [FileName(a.value) for a in self.attrs
                if a.type == ATTR_FILE_NAME and not a.nonresident]


class Volume:
    def __init__(self, image: bytes, partition_lba: int | None, report: Report):
        self.image = image
        self.report = report
        if partition_lba is None:
            partition_lba, self.partition_sectors = self._find_partition()
        else:
            self.partition_sectors = None
        self.base = partition_lba * 512
        boot = image[self.base:self.base + 512]
        if boot[3:11] != b"NTFS    " or boot[510:512] != b"\x55\xAA":
            raise SystemExit(f"No NTFS boot sector at LBA {partition_lba}")
        self.boot = boot
        self.sector_size, self.sectors_per_cluster = struct.unpack_from("<HB", boot, 0x0B)
        self.cluster_size = self.sector_size * self.sectors_per_cluster
        self.hidden_sectors, = struct.unpack_from("<I", boot, 0x1C)
        self.total_sectors, self.mft_lcn, self.mftmirr_lcn = struct.unpack_from("<QQQ", boot, 0x28)
        self.total_clusters = self.total_sectors // self.sectors_per_cluster
        self.record_size = self._size_from_boot(boot[0x40])
        self.index_block_size = self._size_from_boot(boot[0x44])
        self.partition_lba = partition_lba

    def _find_partition(self) -> tuple[int, int]:
        mbr = self.image[:512]
        if mbr[510:512] == b"\x55\xAA":
            for i in range(4):
                entry = mbr[446 + 16 * i:462 + 16 * i]
                if entry[4] == 0x07:
                    lba, count = struct.unpack_from("<II", entry, 8)
                    return lba, count
        return 0, len(self.image) // 512

    def _size_from_boot(self, value: int) -> int:
        signed = value - 256 if value > 127 else value
        return 1 << -signed if signed < 0 else signed * self.cluster_size

    def read_clusters(self, lcn: int, count: int) -> bytes:
        start = self.base + lcn * self.cluster_size
        return self.image[start:start + count * self.cluster_size]

    def read_nonresident(self, attr: Attribute, length: int | None = None) -> bytes:
        out = bytearray()
        for lcn, count in attr.runs or []:
            if lcn is None:
                out += bytes(count * self.cluster_size)
            else:
                out += self.read_clusters(lcn, count)
        return bytes(out[:attr.size if length is None else length])

    def attr_value(self, attr: Attribute) -> bytes:
        return self.read_nonresident(attr) if attr.nonresident else attr.value


def check_boot(vol: Volume, report: Report) -> None:
    if vol.partition_sectors is not None:
        if vol.total_sectors >= vol.partition_sectors:
            report.note(f"boot sector: volume claims {vol.total_sectors} sectors, the whole "
                        f"{vol.partition_sectors}-sector partition, leaving no room for the "
                        f"backup boot sector (Windows expects partition size - 1)")
        else:
            backup_at = vol.base + vol.total_sectors * 512
            if vol.image[backup_at:backup_at + 512] != vol.boot:
                report.note("boot sector: backup boot sector at the end of the volume is missing or differs")
    if vol.hidden_sectors != vol.partition_lba:
        report.note(f"boot sector: hidden sectors {vol.hidden_sectors} != partition start {vol.partition_lba}")
    if vol.record_size % vol.sector_size or vol.index_block_size % vol.sector_size:
        report.error(f"boot sector: record size {vol.record_size} / index block size "
                     f"{vol.index_block_size} is not a multiple of the sector size")


def load_records(vol: Volume, report: Report) -> tuple[list[Record], Record]:
    first = vol.read_clusters(vol.mft_lcn, (vol.record_size + vol.cluster_size - 1) // vol.cluster_size)
    mft = Record(0, first[:vol.record_size], vol.sector_size, report)
    if not mft.valid or mft.problem:
        raise SystemExit(f"$MFT record 0 is unreadable: {mft.problem or 'bad magic'}")
    data = mft.find(ATTR_DATA)
    if data is None or not data.nonresident:
        raise SystemExit("$MFT has no non-resident $DATA attribute")
    table = vol.read_nonresident(data)
    records = []
    for number in range(len(table) // vol.record_size):
        raw = table[number * vol.record_size:(number + 1) * vol.record_size]
        records.append(Record(number, raw, vol.sector_size, report))
    return records, mft


def check_records(vol: Volume, records: list[Record], mft: Record, report: Report) -> None:
    bitmap_attr = mft.find(ATTR_BITMAP)
    if bitmap_attr is None:
        report.error("$MFT has no $BITMAP attribute")
        return
    bitmap = vol.attr_value(bitmap_attr)
    if len(bitmap) * 8 < len(records):
        report.error(f"$MFT bitmap covers {len(bitmap) * 8} records, the table has {len(records)}")

    reserved_empty = []
    for rec in records:
        bit = rec.number < len(bitmap) * 8 and bool(bitmap[rec.number // 8] & (1 << (rec.number % 8)))
        if not rec.valid:
            if bit and rec.number < RESERVED_RECORDS:
                reserved_empty.append(rec.number)
            elif bit:
                report.error(f"record {rec.number}: marked in use in the $MFT bitmap but has no FILE signature")
            continue
        if rec.problem:
            level = report.error if (rec.in_use or bit) else report.warn
            level(f"record {rec.number}: {rec.problem}")
            continue
        if rec.in_use and not bit:
            report.error(f"record {rec.number}: in use but clear in the $MFT bitmap (can be handed out again)")
        elif bit and not rec.in_use and rec.number >= RESERVED_RECORDS:
            report.warn(f"record {rec.number}: marked in the $MFT bitmap but not in use (leaked record)")
        if not rec.in_use:
            continue
        check_attribute_headers(rec, report)
        for attr in rec.attrs:
            if not attr.nonresident:
                continue
            clusters = sum(count for _, count in attr.runs or [])
            where = f"record {rec.number} attribute {attr.type:#x}{'/' + attr.name if attr.name else ''}"
            if attr.lowest_vcn != 0:
                report.note(f"{where}: extent starting at VCN {attr.lowest_vcn} (attribute lists are not checked)")
                continue
            # An empty attribute records its highest VCN as -1.
            if (attr.highest_vcn + 1) & 0xFFFFFFFFFFFFFFFF != clusters:
                report.error(f"{where}: highest VCN {attr.highest_vcn} but runs cover {clusters} clusters")
            if attr.allocated != clusters * vol.cluster_size:
                report.error(f"{where}: allocated size {attr.allocated} != {clusters} run clusters")
            if not attr.initialized <= attr.size <= attr.allocated:
                report.error(f"{where}: sizes out of order (initialized {attr.initialized}, "
                             f"data {attr.size}, allocated {attr.allocated})")
        if rec.number >= FIRST_USER_RECORD or rec.number == ROOT_RECORD:
            names = [n for n in rec.file_names() if n.namespace != NAMESPACE_DOS]
            if rec.number >= FIRST_USER_RECORD and not names and not rec.base_record:
                report.error(f"record {rec.number}: in use but has no $FILE_NAME")
            if names and rec.link_count != len(names):
                report.warn(f"record {rec.number}: link count {rec.link_count} but {len(names)} names")
        is_dir = rec.find(ATTR_INDEX_ROOT, "$I30") is not None
        if is_dir != bool(rec.flags & RECORD_IS_DIRECTORY):
            report.error(f"record {rec.number}: directory flag {'set' if rec.flags & RECORD_IS_DIRECTORY else 'clear'} "
                         f"but it {'has' if is_dir else 'has no'} a $I30 index")
    if reserved_empty:
        report.note(f"reserved records {reserved_empty} are marked in the $MFT bitmap "
                    f"but were never written")


def check_attribute_headers(rec: Record, report: Report) -> None:
    """Header layout rules chkdsk enforces ("attribute record is corrupt")."""
    instances: dict[int, int] = {}
    for attr in rec.attrs:
        raw = attr.raw
        name_length = raw[9]
        name_offset, = struct.unpack_from("<H", raw, 0x0A)
        instance, = struct.unpack_from("<H", raw, 0x0E)
        where = f"record {rec.number} attribute {attr.type:#x}{'/' + attr.name if attr.name else ''}"
        if instance in instances:
            report.error(f"{where}: instance {instance} is also used by attribute {instances[instance]:#x}")
        instances[instance] = attr.type
        if attr.nonresident:
            runs_offset, = struct.unpack_from("<H", raw, 0x20)
            # An unnamed attribute may carry 0 (Windows writes that), never an
            # offset into the header.
            if (name_length or name_offset) and name_offset < 0x40:
                report.error(f"{where}: non-resident name offset {name_offset:#x} lies inside the 0x40-byte header")
            if name_offset + 2 * name_length > runs_offset:
                report.error(f"{where}: name runs into the mapping pairs at {runs_offset:#x}")
            if runs_offset % 8:
                report.error(f"{where}: mapping pairs offset {runs_offset:#x} is not 8-byte aligned")
        else:
            value_offset, = struct.unpack_from("<H", raw, 0x14)
            if (name_length or name_offset) and name_offset < 0x18:
                report.error(f"{where}: resident name offset {name_offset:#x} lies inside the 0x18-byte header")
            if name_length and name_offset + 2 * name_length > value_offset:
                report.error(f"{where}: name runs into the value at {value_offset:#x}")
            if value_offset % 8:
                report.error(f"{where}: value offset {value_offset:#x} is not 8-byte aligned")


def check_clusters(vol: Volume, records: list[Record], report: Report) -> None:
    owner: dict[int, str] = {}
    first_system = min(vol.mft_lcn, vol.mftmirr_lcn)
    boot_record = records[7] if len(records) > 7 else None
    if boot_record is None or not boot_record.valid or not boot_record.in_use:
        # No $Boot record: the harness reserves everything in front of the MFT.
        for lcn in range(first_system):
            owner[lcn] = "boot area"

    for rec in records:
        if not rec.valid or rec.problem or not rec.in_use:
            continue
        for attr in rec.attrs:
            if not attr.nonresident:
                continue
            label = f"record {rec.number} attribute {attr.type:#x}{'/' + attr.name if attr.name else ''}"
            for lcn, count in attr.runs or []:
                if lcn is None:
                    continue
                if lcn < 0 or lcn + count > vol.total_clusters:
                    report.error(f"{label}: run {lcn}+{count} lies outside the volume "
                                 f"({vol.total_clusters} clusters)")
                    continue
                for cluster in range(lcn, lcn + count):
                    if cluster in owner:
                        report.error(f"cluster {cluster} is owned by both {owner[cluster]} and {label}")
                    else:
                        owner[cluster] = label

    bitmap_record = records[6] if len(records) > 6 else None
    data = bitmap_record.find(ATTR_DATA) if bitmap_record and bitmap_record.in_use else None
    if data is None:
        report.error("$Bitmap (record 6) has no $DATA attribute")
        return
    bitmap = vol.attr_value(data)
    if len(bitmap) * 8 < vol.total_clusters:
        report.error(f"$Bitmap covers {len(bitmap) * 8} clusters, the volume has {vol.total_clusters}")
        return

    unmarked = [c for c in owner if not bitmap[c // 8] & (1 << (c % 8))]
    leaked = [c for c in range(vol.total_clusters)
              if bitmap[c // 8] & (1 << (c % 8)) and c not in owner]
    for cluster in sorted(unmarked)[:20]:
        report.error(f"cluster {cluster} is used by {owner[cluster]} but free in $Bitmap (can be double-allocated)")
    if len(unmarked) > 20:
        report.error(f"... and {len(unmarked) - 20} more used clusters free in $Bitmap")
    if leaked:
        report.warn(f"{len(leaked)} cluster(s) marked in $Bitmap but owned by nothing (leaked): "
                    f"{compress_ranges(leaked)}")
    tail_bits = len(bitmap) * 8 - vol.total_clusters
    tail_clear = [c for c in range(vol.total_clusters, len(bitmap) * 8)
                  if not bitmap[c // 8] & (1 << (c % 8))]
    if tail_bits and tail_clear:
        report.warn(f"{len(tail_clear)} $Bitmap bit(s) past the end of the volume are clear")


def compress_ranges(values: list[int]) -> str:
    parts = []
    start = prev = values[0]
    for value in values[1:] + [None]:
        if value is not None and value == prev + 1:
            prev = value
            continue
        parts.append(str(start) if start == prev else f"{start}-{prev}")
        if value is not None:
            start = prev = value
    return ", ".join(parts)


def check_mirror(vol: Volume, records: list[Record], report: Report) -> None:
    mirror_record = records[1] if len(records) > 1 else None
    data = mirror_record.find(ATTR_DATA) if mirror_record and mirror_record.in_use else None
    if data is None or not data.nonresident:
        report.error("$MFTMirr (record 1) has no non-resident $DATA attribute")
        return
    if data.runs and data.runs[0][0] != vol.mftmirr_lcn:
        report.error(f"$MFTMirr data starts at cluster {data.runs[0][0]}, boot sector says {vol.mftmirr_lcn}")
    mirror = vol.read_nonresident(data)
    first = vol.read_clusters(vol.mft_lcn, (len(mirror) + vol.cluster_size - 1) // vol.cluster_size)
    # Compare contents, not raw sectors: every rewrite of a record bumps its
    # update sequence number, which is not a difference chkdsk cares about.
    for number in range(len(mirror) // vol.record_size):
        a = mirror[number * vol.record_size:(number + 1) * vol.record_size]
        b = first[number * vol.record_size:(number + 1) * vol.record_size]
        if a == b:
            continue
        a_data, a_err = apply_fixups(a, vol.sector_size)
        b_data, b_err = apply_fixups(b, vol.sector_size)
        if a[:4] == b"FILE" and b[:4] == b"FILE" and not a_err and not b_err:
            usa_offset, usa_count = struct.unpack_from("<HH", b_data, 4)
            for data in (a_data, b_data):
                data[usa_offset:usa_offset + 2 * usa_count] = bytes(2 * usa_count)
            if a_data == b_data:
                continue
        report.error(f"$MFTMirr copy of record {number} differs from $MFT")


UPCASE: list[int] | None = None


def load_upcase(vol: Volume, records: list[Record]) -> None:
    """Use the volume's $UpCase table for name collation when it has one."""
    global UPCASE
    record = records[10] if len(records) > 10 else None
    data = record.find(ATTR_DATA) if record and record.valid and record.in_use else None
    if data is not None:
        table = vol.attr_value(data)
        if len(table) >= 0x20000:
            UPCASE = list(struct.unpack_from("<65536H", table))


def upcase_key(name: str) -> tuple[int, ...]:
    # NTFS collates file names by comparing UTF-16 code units after mapping
    # each through $UpCase. Harness volumes have no $UpCase; simple Unicode
    # upper-casing matches the driver's ordering for the names tests create.
    units = struct.unpack(f"<{len(name.encode('utf-16le')) // 2}H", name.encode("utf-16le"))
    if UPCASE is not None:
        return tuple(UPCASE[u] for u in units)
    return tuple(ord(c) for c in name.upper())


class IndexWalker:
    def __init__(self, vol: Volume, records: list[Record], directory: Record, report: Report):
        self.vol = vol
        self.records = records
        self.dir = directory
        self.report = report
        self.where = f"directory record {directory.number} $I30"
        self.entries: list[tuple[str, int, int]] = []  # (name, record, sequence)
        self.visited: set[int] = set()
        alloc = directory.find(ATTR_INDEX_ALLOCATION, "$I30")
        bitmap = directory.find(ATTR_BITMAP, "$I30")
        self.alloc = vol.read_nonresident(alloc) if alloc else b""
        self.bitmap = vol.attr_value(bitmap) if bitmap else b""
        if alloc and not bitmap:
            report.error(f"{self.where}: has $INDEX_ALLOCATION but no $BITMAP")
        self.vcn_unit = vol.sector_size if vol.index_block_size < vol.cluster_size else vol.cluster_size

    def run(self) -> None:
        root = self.dir.find(ATTR_INDEX_ROOT, "$I30")
        value = root.value
        attr_type, collation, block_size = struct.unpack_from("<III", value, 0)
        if attr_type != ATTR_FILE_NAME or collation != 1:
            self.report.error(f"{self.where}: root indexes type {attr_type:#x} collation {collation}, "
                              f"expected $FILE_NAME / filename collation")
        if block_size != self.vol.index_block_size:
            self.report.error(f"{self.where}: block size {block_size} != boot sector's {self.vol.index_block_size}")
        self.walk_node(value, 0x10, "root", None, None)
        blocks = len(self.alloc) // self.vol.index_block_size
        for block in range(blocks):
            used = block < len(self.bitmap) * 8 and self.bitmap[block // 8] & (1 << (block % 8))
            if used and block not in self.visited:
                self.report.warn(f"{self.where}: index block {block} is allocated but unreachable from the root")
        for i in range(1, len(self.entries)):
            if upcase_key(self.entries[i - 1][0]) >= upcase_key(self.entries[i][0]):
                self.report.error(f"{self.where}: entries out of order or duplicated: "
                                  f"{self.entries[i - 1][0]!r} then {self.entries[i][0]!r}")

    def walk_node(self, node: bytes, header_offset: int, label: str,
                  low: str | None, high: str | None) -> None:
        first, total, allocated, flags = struct.unpack_from("<IIIB", node, header_offset)
        offset = header_offset + first
        end = header_offset + total
        if total > allocated or end > len(node):
            self.report.error(f"{self.where} {label}: node header sizes are inconsistent")
            return
        while True:
            if offset + 0x10 > end:
                self.report.error(f"{self.where} {label}: entries run past the node without an end entry")
                return
            file_ref, entry_length, key_length, entry_flags = struct.unpack_from("<QHHI", node, offset)
            if entry_length < 0x10 or entry_length % 8 or offset + entry_length > end:
                self.report.error(f"{self.where} {label}: entry at {offset:#x} has bad length {entry_length}")
                return
            name = None
            if not entry_flags & INDEX_ENTRY_END:
                key = FileName(node[offset + 0x10:offset + 0x10 + key_length])
                name = key.name
                if (low is not None and upcase_key(name) <= upcase_key(low)) or \
                   (high is not None and upcase_key(name) >= upcase_key(high)):
                    self.report.error(f"{self.where} {label}: {name!r} is outside its subtree's key range")
                self.check_entry(file_ref, key, label)
            if entry_flags & INDEX_ENTRY_NODE:
                if not flags & 0x01:
                    self.report.error(f"{self.where} {label}: entry has a child but the node is marked as a leaf")
                vcn, = struct.unpack_from("<Q", node, offset + entry_length - 8)
                self.walk_block(vcn, low, name if name is not None else high)
            if name is not None:
                self.entries.append((name, file_ref & 0xFFFFFFFFFFFF, file_ref >> 48))
                low = name
            if entry_flags & INDEX_ENTRY_END:
                return
            offset += entry_length

    def walk_block(self, vcn: int, low: str | None, high: str | None) -> None:
        block = vcn * self.vcn_unit // self.vol.index_block_size
        label = f"block {block} (VCN {vcn})"
        if vcn * self.vcn_unit % self.vol.index_block_size:
            self.report.error(f"{self.where}: child VCN {vcn} is not on an index block boundary")
            return
        if block in self.visited:
            self.report.error(f"{self.where}: {label} is referenced more than once (cycle or shared child)")
            return
        self.visited.add(block)
        start = block * self.vol.index_block_size
        raw = self.alloc[start:start + self.vol.index_block_size]
        if len(raw) < self.vol.index_block_size:
            self.report.error(f"{self.where}: {label} lies past the end of $INDEX_ALLOCATION")
            return
        if not (block < len(self.bitmap) * 8 and self.bitmap[block // 8] & (1 << (block % 8))):
            self.report.error(f"{self.where}: {label} is in use but free in the $I30 bitmap")
        if raw[:4] != b"INDX":
            self.report.error(f"{self.where}: {label} has no INDX signature")
            return
        data, err = apply_fixups(raw, self.vol.sector_size)
        if err:
            self.report.error(f"{self.where}: {label}: {err}")
            return
        own_vcn, = struct.unpack_from("<Q", data, 0x10)
        if own_vcn != vcn:
            self.report.error(f"{self.where}: {label} records its own VCN as {own_vcn}")
        self.walk_node(bytes(data), 0x18, label, low, high)

    def check_entry(self, file_ref: int, key: FileName, label: str) -> None:
        number = file_ref & 0xFFFFFFFFFFFF
        sequence = file_ref >> 48
        text = f"{self.where} {label}: entry {key.name!r} -> record {number}"
        if key.parent != self.dir.number:
            self.report.error(f"{text}: key names parent {key.parent}, not this directory")
        if number >= len(self.records):
            self.report.error(f"{text}: record is past the end of $MFT")
            return
        target = self.records[number]
        if not target.valid or target.problem or not target.in_use:
            self.report.error(f"{text}: record is not in use (dangling entry)")
            return
        if sequence and sequence != target.sequence:
            self.report.error(f"{text}: sequence {sequence} but record has {target.sequence} (stale entry)")
        matches = [n for n in target.file_names()
                   if n.name == key.name and n.parent == self.dir.number]
        if not matches:
            self.report.error(f"{text}: record has no $FILE_NAME {key.name!r} in this directory")


def check_directories(vol: Volume, records: list[Record], report: Report) -> None:
    indexed: dict[tuple[int, str], int] = defaultdict(int)
    for rec in records:
        if not rec.valid or rec.problem or not rec.in_use:
            continue
        if rec.find(ATTR_INDEX_ROOT, "$I30") is None:
            continue
        walker = IndexWalker(vol, records, rec, report)
        walker.run()
        for name, number, _ in walker.entries:
            indexed[(rec.number, name)] += 1
            if indexed[(rec.number, name)] == 2:
                report.error(f"directory record {rec.number}: {name!r} is indexed more than once")

    for rec in records:
        if (not rec.valid or rec.problem or not rec.in_use or rec.number < FIRST_USER_RECORD
                or rec.base_record):
            continue
        for name in rec.file_names():
            if name.namespace == NAMESPACE_DOS:
                continue
            parent = records[name.parent] if name.parent < len(records) else None
            where = f"record {rec.number} {name.name!r}"
            if parent is None or not parent.valid or not parent.in_use:
                report.error(f"{where}: parent record {name.parent} is not in use (orphaned file)")
                continue
            if name.parent_seq and name.parent_seq != parent.sequence:
                report.error(f"{where}: parent reference sequence {name.parent_seq} "
                             f"!= parent's {parent.sequence}")
            if parent.find(ATTR_INDEX_ROOT, "$I30") is None:
                report.error(f"{where}: parent record {name.parent} is not a directory")
            elif indexed[(name.parent, name.name)] == 0:
                report.error(f"{where}: not listed in parent directory {name.parent} (lost file)")


def check_harness_format(records: list[Record], report: Report) -> None:
    missing = [n for n, label in ((4, "$AttrDef"), (7, "$Boot"), (8, "$BadClus"), (9, "$Secure"),
                                  (10, "$UpCase"), (11, "$Extend"))
               if n >= len(records) or not records[n].valid or not records[n].in_use]
    if missing:
        report.note(f"system records {missing} are absent (harness builds only what the driver needs)")
    log = records[2] if len(records) > 2 else None
    data = log.find(ATTR_DATA) if log and log.valid else None
    if data is not None and not data.nonresident:
        report.note(f"$LogFile is a resident {data.size}-byte stub, not a real journal")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=Path, help="raw disk or partition image")
    parser.add_argument("--partition-lba", type=int,
                        help="start of the NTFS volume (default: first MBR partition of type 07)")
    parser.add_argument("--quiet-notes", action="store_true", help="hide NOTE findings")
    args = parser.parse_args()

    report = Report()
    vol = Volume(args.image.read_bytes(), args.partition_lba, report)
    check_boot(vol, report)
    records, mft = load_records(vol, report)
    load_upcase(vol, records)
    check_harness_format(records, report)
    check_records(vol, records, mft, report)
    check_clusters(vol, records, report)
    check_mirror(vol, records, report)
    check_directories(vol, records, report)

    in_use = sum(1 for r in records if r.valid and not r.problem and r.in_use)
    print(f"{args.image}: NTFS at LBA {vol.partition_lba}, {vol.total_clusters} clusters of "
          f"{vol.cluster_size} bytes, {len(records)} MFT records ({in_use} in use)")
    for level, text in report.items:
        if level == "NOTE" and args.quiet_notes:
            continue
        print(f"{level:5} {text}")
    errors, warnings = report.count("ERROR"), report.count("WARN")
    print(f"{errors} error(s), {warnings} warning(s), {report.count('NOTE')} note(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
