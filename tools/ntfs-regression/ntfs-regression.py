#!/usr/bin/env python3
"""Disposable NTFS regression harness for the ReactOS/WinDosDX tree.

The harness never opens a host physical drive. It creates a private raw disk
image with an MBR partition, boots the current bootcd, and checks machine-
readable COM2 markers emitted by ntfs-regression.exe. Interactive mode uses
the QEMU monitor to enter commands in the guest Run dialog.

The NTFS write registry value is enabled only inside the disposable guest:
with --enable-write in interactive mode, or in the dedicated autorun image.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = ROOT / "output-VS-i386"
DEFAULT_QEMU = ROOT / "output-tools" / "qemu"
DEFAULT_TIMEOUT = 150.0

KEYS = {
    " ": "spc",
    "\\": "backslash",
    "/": "slash",
    ":": "shift-semicolon",
    "-": "minus",
    ".": "dot",
    ",": "comma",
    ";": "semicolon",
    "_": "shift-minus",
    "+": "shift-equal",
    "&": "shift-7",
    "(": "shift-9",
    ")": "shift-0",
    "|": "shift-backslash",
    "\"": "apostrophe",
    "'": "apostrophe",
}


def fail(message: str) -> "NoReturn":
    raise RuntimeError(message)


def find_tool(directory: Path, name: str) -> Path:
    path = directory / name
    if not path.is_file():
        fail(f"Required tool is missing: {path}")
    return path


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


class Monitor:
    def __init__(self, port: int):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.socket.settimeout(0.05)

    def close(self) -> None:
        try:
            self.socket.close()
        except OSError:
            pass

    def drain(self) -> None:
        while True:
            try:
                data = self.socket.recv(4096)
            except socket.timeout:
                return
            except OSError:
                return
            if not data:
                return

    def command(self, command: str) -> None:
        self.drain()
        self.socket.sendall((command + "\r\n").encode("ascii"))
        time.sleep(0.002)
        self.drain()

    def key(self, key: str) -> None:
        self.command(f"sendkey {key}")

    def text(self, text: str) -> None:
        for character in text:
            if character in KEYS:
                key = KEYS[character]
            elif character.isalpha():
                key = character.lower() if character.islower() else f"shift-{character.lower()}"
            else:
                key = character
            self.key(key)
            time.sleep(0.004)

    def run_dialog(self, command: str) -> None:
        # Explorer maps Alt+Enter to the Start menu. The English test menu
        # exposes Run as an R accelerator, which is more reliable across QEMU
        # keyboard backends than the Windows-key shortcut.
        self.key("alt-enter")
        time.sleep(0.8)
        self.key("r")
        time.sleep(0.8)
        self.text(command)
        self.key("ret")


def _iso_directory(iso: Path, lba: int, size: int):
    with iso.open("rb") as stream:
        stream.seek(lba * 2048)
        data = stream.read(size)
    offset = 0
    while offset < len(data):
        length = data[offset]
        if length == 0:
            offset = (offset // 2048 + 1) * 2048
            continue
        record = data[offset:offset + length]
        if len(record) < 34:
            break
        name_length = record[32]
        raw_name = record[33:33 + name_length]
        if name_length == 1 and raw_name in (b"\x00", b"\x01"):
            name = ""
        else:
            name = raw_name.decode("ascii", "replace").split(";", 1)[0].lower()
        yield name, int.from_bytes(record[2:6], "little"), int.from_bytes(record[10:14], "little"), bool(record[25] & 2)
        offset += length


def _find_iso_file(iso: Path, wanted: str) -> tuple[int, int]:
    with iso.open("rb") as stream:
        stream.seek(16 * 2048 + 156)
        root = stream.read(34)
    root_lba = int.from_bytes(root[2:6], "little")
    root_size = int.from_bytes(root[10:14], "little")
    current = [(name, lba, size, is_dir) for name, lba, size, is_dir in _iso_directory(iso, root_lba, root_size) if name]
    for component in wanted.lower().split("/"):
        match = next(((lba, size) for name, lba, size, is_dir in current
                      if name == component and not is_dir), None)
        if match is None and component != wanted.lower().split("/")[-1]:
            match = next(((lba, size) for name, lba, size, is_dir in current
                          if name == component and is_dir), None)
        if match is None:
            raise RuntimeError(f"ISO boot image is missing: {wanted}")
        lba, size = match
        if component != wanted.lower().split("/")[-1]:
            current = [(name, child_lba, child_size, is_dir)
                       for name, child_lba, child_size, is_dir in _iso_directory(iso, lba, size)
                       if name]
    return lba, size


def _el_torito_catalog(boot_lba: int) -> bytes:
    catalog = bytearray(2048)
    catalog[0] = 1
    catalog[4:28] = b"REACTOS".ljust(24)
    catalog[30:32] = b"\x55\xaa"
    catalog[28:30] = b"\x00\x00"
    words = struct.unpack_from("<16H", catalog, 0)
    catalog[28:30] = struct.pack("<H", (-sum(words)) & 0xFFFF)
    catalog[32] = 0x88
    catalog[38:40] = struct.pack("<H", 4)
    catalog[40:44] = struct.pack("<I", boot_lba)
    return bytes(catalog)


def _catalog_is_valid(bootcd: Path) -> bool:
    """Check that the El Torito boot record points at a usable boot catalog."""
    with bootcd.open("rb") as stream:
        stream.seek(17 * 2048)
        record = stream.read(2048)
        if record[0] != 0 or record[1:6] != b"CD001":
            return False
        catalog_lba = int.from_bytes(record[0x47:0x4B], "little")
        if not catalog_lba:
            return False
        stream.seek(catalog_lba * 2048)
        catalog = stream.read(64)
    if len(catalog) < 64 or catalog[0] != 1 or catalog[30:32] != b"\x55\xaa":
        return False
    words = struct.unpack_from("<16H", catalog, 0)
    if sum(words) & 0xFFFF:
        return False
    entry = catalog[32:64]
    return entry[0] == 0x88 and int.from_bytes(entry[8:12], "little") > 0


def prepare_bootcd(bootcd: Path, work: Path) -> Path:
    """Make the generated ISO acceptable to QEMU's BIOS boot path.

    Older builds of the bundled Windows mkisofs/isohybrid pair emitted a boot
    record without a usable El Torito catalog pointer. BIOS firmware then kept
    scanning the CD and never entered the boot sector. Keep the build artifact
    untouched and repair a disposable per-workdir copy instead.
    """
    if _catalog_is_valid(bootcd):
        return bootcd

    boot_record = 17 * 2048
    signature_offset = boot_record + 0x7FE
    catalog_pointer_offset = boot_record + 0x47

    patched = work / "bootcd-qemu.iso"
    if not patched.is_file():
        shutil.copyfile(bootcd, patched)
    with patched.open("r+b") as stream:
        # Sector 19 follows the PVD, boot record, and terminator and is
        # padding in the generated image. Keep the catalog inside the ISO
        # volume; some BIOSes ignore appended media sectors.
        catalog_lba = 19
        stream.seek(catalog_lba * 2048)
        existing = stream.read(4)
        if existing not in (b"\x00\x00\x00\x00", b"\xff\xff\xff\xff"):
            stream.seek(0, os.SEEK_END)
            catalog_lba = stream.tell() // 2048
        stream.seek(catalog_lba * 2048)
        stream.write(_el_torito_catalog(_find_iso_file(patched, "loader/isobootntfs.bin")[0]))
        stream.seek(16 * 2048 + 80)
        volume_sectors = max(int.from_bytes(stream.read(4), "little"), catalog_lba + 1)
        stream.seek(16 * 2048 + 80)
        stream.write(struct.pack("<I", volume_sectors))
        stream.write(struct.pack(">I", volume_sectors))
        stream.seek(catalog_pointer_offset)
        stream.write(struct.pack("<I", catalog_lba))
        stream.seek(signature_offset)
        stream.write(b"\x55\xaa")
    return patched


class Boot:
    def __init__(self, qemu: Path, image: Path, bootcd: Path, serial: Path,
                 machine_serial: Path, stderr: Path, port: int,
                 memory: int = 1024, allow_reboot: bool = False):
        self.qemu = qemu
        self.image = image
        self.bootcd = bootcd
        self.serial = serial
        self.machine_serial = machine_serial
        self.stderr = stderr
        self.port = port
        self.memory = memory
        self.allow_reboot = allow_reboot
        self.process: subprocess.Popen[bytes] | None = None
        self.monitor: Monitor | None = None
        self.serial.write_bytes(b"")
        self.machine_serial.write_bytes(b"")

    def start(self) -> None:
        bootcd = prepare_bootcd(self.bootcd, self.serial.parent)
        command = [
            str(self.qemu),
            "-M", "pc",
            "-m", str(self.memory),
            "-drive", f"file={self.image},format=raw,if=ide,index=0",
            "-cdrom", str(bootcd),
            "-boot", "d",
            "-serial", f"file:{self.serial}",
            "-serial", f"file:{self.machine_serial}",
            "-display", "none",
            "-monitor", f"tcp:127.0.0.1:{self.port},server,nowait",
        ]
        if not self.allow_reboot:
            command.append("-no-reboot")
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.DEVNULL,
            stderr=self.stderr.open("wb"),
            creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
        )
        try:
            self.monitor = Monitor(self.port)
        except OSError as exc:
            self.stop()
            fail(f"Could not connect to the QEMU monitor: {exc}")

    def serial_text(self) -> str:
        return read_text(self.machine_serial)

    def diagnostic_text(self) -> str:
        return f"PASS serial (COM2):\n{self.serial_text()[-4000:]}\n" \
               f"DEBUG serial (COM1):\n{read_text(self.serial)[-2000:]}"

    def wait_for(self, marker: str, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker in self.serial_text():
                return True
            if self.process is not None and self.process.poll() is not None:
                return marker in self.serial_text()
            time.sleep(0.2)
        return False

    def stop(self, force: bool = False) -> None:
        if self.process is None:
            return
        if not force and self.monitor is not None:
            try:
                self.monitor.command("quit")
                self.process.wait(timeout=15)
            except (OSError, subprocess.TimeoutExpired):
                pass
        if self.process.poll() is None:
            if force:
                self.process.kill()
            else:
                self.process.terminate()
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=15)
        if self.monitor is not None:
            self.monitor.close()
            self.monitor = None
        self.process = None


def run_guest_command(boot: Boot, command: str, marker: str | None,
                      timeout: float, settle: float = 2.0) -> str:
    assert boot.monitor is not None
    boot.monitor.run_dialog(command)
    time.sleep(settle)
    if marker is not None and not boot.wait_for(marker, timeout):
        output = boot.serial_text()
        boot.stop()
        fail(f"Timed out waiting for {marker!r}.\n{boot.diagnostic_text()}")
    return boot.serial_text()


def create_test_partition(image: Path) -> None:
    """Write an MBR with one disposable NTFS partition.

    ReactOS does not assign a drive letter to a completely blank RAW disk.
    A 64 MiB partition is sufficient for the regression payload and keeps the
    generated test image compact. The boot flag is irrelevant because QEMU is
    explicitly booted from the CD, but a conventional active partition entry
    makes FreeLoader and partmgr agree on the layout.
    """
    start_lba = 2048
    partition_sectors = 130560
    end_lba = start_lba + partition_sectors - 1

    def chs(lba: int) -> bytes:
        cylinders = lba // (16 * 63)
        heads = (lba // 63) % 16
        sectors = (lba % 63) + 1
        if cylinders > 1023:
            cylinders = 1023
        return bytes((heads & 0x0F,
                      ((cylinders >> 2) & 0xC0) | (sectors & 0x3F),
                      cylinders & 0xFF))

    entry = b"\x80" + chs(start_lba) + b"\x07" + chs(end_lba) + \
        struct.pack("<II", start_lba, partition_sectors)
    mbr = bytearray(512)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    with image.open("r+b") as stream:
        stream.seek(0)
        stream.write(mbr)
        stream.seek(start_lba * 512)
        stream.write(minimal_ntfs_volume(partition_sectors))


def check_image(qemu_img: Path, image: Path) -> None:
    # qemu-img check supports qcow2, not raw images. The harness uses raw
    # storage so it can place an MBR directly at guest offset zero.
    operation = "check" if image.suffix.lower() == ".qcow2" else "info"
    result = subprocess.run(
        [str(qemu_img), operation, str(image)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        fail(f"qemu-img {operation} failed for {image}:\n{result.stdout}")


def fixup_array(sector_size: int) -> bytes:
    """Build the update-sequence array protecting the last two bytes of
    every 512-byte sector.

    Layout: [USANumber:u16][USACount:u16][protected words...]. When the
    record is written to disk the first 'count' protected words are copied
    into the trailing word of each sector and replaced here by USANumber.
    On read FixupUpdateSequenceArray() verifies each trailing word equals
    USANumber and swaps the originals back in.
    """
    count = sector_size // 512
    return struct.pack("<HH", 1, count + 1) + struct.pack("<H", 0) * count


def file_record_fixup_array(record_size: int, sector_size: int) -> bytes:
    """Update-sequence array covering every sector of a multi-sector
    MFT file record (one protected word per 512-byte sector)."""
    count = record_size // sector_size
    return struct.pack("<HH", 1, count + 1) + struct.pack("<H", 0) * count


def seal_fixups(record: bytearray, usa_offset: int, sector_size: int) -> None:
    """Apply fixup-array sealing for a record about to be written to disk:
    each 512-byte sector's last word is replaced by the USA number and its
    original value is stored in the corresponding array slot."""
    number = struct.unpack_from("<H", record, usa_offset)[0]
    count = struct.unpack_from("<H", record, usa_offset + 2)[0] - 1
    for i in range(count):
        end = (i + 1) * sector_size - 2
        original = record[end:end + 2]
        record[usa_offset + 4 + i * 2:usa_offset + 6 + i * 2] = original
        record[end:end + 2] = struct.pack("<H", number)


def apply_fixups(record: bytearray, offset: int, sector_size: int, number: int) -> None:
    """Replace each sector's trailing word with the fixup number and stash
    the original values in the array at 'offset' (NTFS USA semantics)."""
    count = sector_size // 512
    struct.pack_into("<I", record, offset, number)
    struct.pack_into("<H", record, offset + 4, count)
    for i in range(count):
        end = offset - 2 + (i + 1) * 512
        record[end:end + 2] = struct.pack("<H", number)


def file_record(index: int, attributes: bytes | None = None,
                record_size: int = 4096, sector_size: int = 512,
                flags: int = 1) -> bytes:
    """Create an MFT file record from raw attribute bytes (flags bit 1 = in-use)."""
    usa_offset = 0x30
    fixups = file_record_fixup_array(record_size, sector_size)
    record = bytearray(record_size)

    attr_off = usa_offset + len(fixups)
    attr_off = (attr_off + 7) & ~7

    if attributes is None:
        # $STANDARD_INFORMATION (resident, 0x68 bytes)
        si = bytearray(0x68)
        struct.pack_into("<I", si, 0x00, 0x10)
        struct.pack_into("<I", si, 0x04, 0x68)
        struct.pack_into("<H", si, 0x0A, 0x48)
        struct.pack_into("<I", si, 0x10, 0x48)
        struct.pack_into("<I", si, 0x14, 0x20)
        record[attr_off:attr_off + len(si)] = si
        attr_off += len(si)
        end_marker = attr_off
    else:
        record[attr_off:attr_off + len(attributes)] = attributes
        end_marker = attr_off + len(attributes)

    struct.pack_into("<I", record, end_marker, 0xFFFFFFFF)

    # File record header: NTFS_RECORD_HEADER + FILE_RECORD_HEADER fields
    record[0x00:0x04] = b"FILE"
    struct.pack_into("<H", record, 0x04, usa_offset)
    # The header UsaCount is "number + protected words"; the array stores the
    # same value in its second word, so derive it from there to keep both in
    # sync (FixupUpdateSequenceArray trusts the header).
    struct.pack_into("<H", record, 0x06, struct.unpack_from("<H", fixups, 2)[0])
    record[usa_offset:usa_offset + len(fixups)] = fixups
    struct.pack_into("<H", record, 0x10, 0x38)   # SequenceNumber
    struct.pack_into("<H", record, 0x12, 1)      # LinkCount
    struct.pack_into("<H", record, 0x14, attr_off)  # AttributeOffset
    struct.pack_into("<H", record, 0x16, flags)  # Flags: in-use bit
    struct.pack_into("<I", record, 0x18, end_marker + 4)  # BytesInUse
    struct.pack_into("<I", record, 0x1C, record_size)     # BytesAllocated
    struct.pack_into("<Q", record, 0x20, 0)      # BaseFileRecord = NULL
    struct.pack_into("<H", record, 0x28, index + 1)  # NextAttributeNumber
    struct.pack_into("<I", record, 0x2C, index)  # MFTRecordNumber
    seal_fixups(record, usa_offset, sector_size)
    return bytes(record)


def resident_attribute(attr_type: int, name: str, value: bytes,
                       instance: int = 0) -> bytes:
    """Build a resident attribute record (8-byte aligned)."""
    name_bytes = name.encode("utf-16-le") if name else b""
    value_offset = 0x18 + len(name_bytes)
    attr_len = value_offset + len(value)
    attr_len = (attr_len + 7) & ~7
    attr = bytearray(attr_len)
    struct.pack_into("<I", attr, 0x00, attr_type)
    struct.pack_into("<I", attr, 0x04, attr_len)
    attr[0x08] = 0                       # resident
    attr[0x09] = len(name_bytes) // 2    # NameLength
    struct.pack_into("<H", attr, 0x0A, 0x18)  # NameOffset
    struct.pack_into("<H", attr, 0x0C, 0)     # Flags
    struct.pack_into("<H", attr, 0x0E, instance)  # Instance
    struct.pack_into("<I", attr, 0x10, len(value))    # ValueLength (0x10)
    struct.pack_into("<H", attr, 0x14, value_offset)  # ValueOffset (0x14)
    attr[0x18:0x18 + len(name_bytes)] = name_bytes
    attr[value_offset:value_offset + len(value)] = value
    return bytes(attr)


def nonresident_data_attribute(runs: bytes, data_size: int,
                               allocated_size: int, instance: int = 0) -> bytes:
    """Build a non-resident unnamed $DATA attribute with the given runs."""
    attr_len = 0x40 + len(runs)
    attr_len = (attr_len + 7) & ~7
    attr = bytearray(attr_len)
    struct.pack_into("<I", attr, 0x00, 0x80)   # $DATA
    struct.pack_into("<I", attr, 0x04, attr_len)
    attr[0x08] = 1                             # non-resident
    struct.pack_into("<H", attr, 0x0C, 0)     # Flags
    struct.pack_into("<H", attr, 0x0E, instance)  # Instance
    struct.pack_into("<Q", attr, 0x10, 0)     # LowestVCN
    struct.pack_into("<Q", attr, 0x18, 0)     # HighestVCN (set by caller below)
    struct.pack_into("<H", attr, 0x20, 0x40)  # MappingPairsOffset
    struct.pack_into("<H", attr, 0x22, 0)     # CompressionUnit
    struct.pack_into("<Q", attr, 0x28, allocated_size)  # AllocatedSize
    struct.pack_into("<Q", attr, 0x30, data_size)       # DataSize
    struct.pack_into("<Q", attr, 0x38, data_size)       # InitializedSize
    attr[0x40:0x40 + len(runs)] = runs
    return bytearray(attr)


def encode_run(offset_clusters: int, length_clusters: int) -> bytes:
    """Encode one NTFS data run (length then offset, sparse-capable)."""
    length_bytes = bytearray()
    remaining = length_clusters
    while remaining:
        length_bytes.append(remaining & 0xFF)
        remaining >>= 8
    if offset_clusters == 0:
        offset_bytes = b""
    else:
        value = offset_clusters
        size = 1
        while value >= (1 << (8 * size - 1)):
            size += 1
        value_bytes = value.to_bytes(size, "little", signed=False)
        # Sign-extend the last byte convention: keep two's complement via signed int
        offset_bytes = (offset_clusters & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
    # Header byte: high nibble = offset size, low nibble = length size
    # (DecodeRun reads *DataRun >> 4 for the offset size).  bytes(x) with an
    # int would allocate a zero-filled buffer, so build a 1-byte buffer.
    return bytes(((len(offset_bytes) << 4) | len(length_bytes),)) + \
        length_bytes + offset_bytes


def minimal_ntfs_volume(partition_sectors: int) -> bytes:
    """Build the smallest NTFS image the driver's mount path accepts.

    The ReactOS NTFS driver requires at mount time:
    - $MFT (record 0) with a non-resident $DATA attribute whose data runs
      cover the whole table, plus a resident $Bitmap of >= 3 bytes;
    - $LogFile (record 2) with a resident $DATA of at least 0x400 bytes
      holding two valid restart pages marked RESTART_VOLUME_IS_CLEAN;
    - $Volume (record 3) with a resident $VOLUME_INFORMATION attribute;
    - a root directory (record 5) carrying $FILE_NAME and an empty
      $INDEX_ROOT $I30 so file creation can add entries to it.
    """
    sector_size = 512
    sectors_per_cluster = 8
    cluster_size = sector_size * sectors_per_cluster
    mft_record_count = 64
    mft_records_bytes = mft_record_count * 4096
    mft_clusters = mft_records_bytes // cluster_size

    mft_cluster = 4
    mftmirr_cluster = mft_cluster + mft_clusters
    mft_lcn = mft_cluster * cluster_size

    image = bytearray(partition_sectors * sector_size)

    # Boot sector
    bs = bytearray(sector_size)
    bs[0:3] = b"\xeb\x52\x90"
    bs[3:11] = b"NTFS    "
    struct.pack_into("<H", bs, 0x0B, sector_size)
    bs[0x0D] = sectors_per_cluster
    bs[0x15] = 0xF8  # hard disk media
    struct.pack_into("<H", bs, 0x18, 63)
    struct.pack_into("<H", bs, 0x1A, 16)
    struct.pack_into("<H", bs, 0x24, 0x80)
    struct.pack_into("<H", bs, 0x26, 0x80)
    struct.pack_into("<Q", bs, 0x28, partition_sectors)
    struct.pack_into("<Q", bs, 0x30, mft_cluster)
    struct.pack_into("<Q", bs, 0x38, mftmirr_cluster)
    # ClustersPerMftRecord/IndexRecord are SIGNED chars; the driver computes
    # the record size as 1 << (-value) bytes for negative values.  -12 gives
    # 4 KiB MFT records, -9 gives 512-byte index blocks (a positive value
    # would instead mean value * BytesPerCluster).
    bs[0x40] = 0xF4  # MFT record = 2^12 = 4096 B
    bs[0x44] = 0xF7  # index record = 2^9 = 512 B
    struct.pack_into("<Q", bs, 0x48, 0x1A2B3C4D5E6F7081)
    bs[0x1FE:0x200] = b"\x55\xAA"
    image[0:sector_size] = bs

    # --- Attributes ------------------------------------------------------------
    # $MFT's non-resident $DATA: one run covering the whole table.
    highest_vcn = mft_records_bytes // cluster_size - 1
    mft_run = encode_run(mft_cluster, mft_clusters) + b"\x00"
    mft_data_attr = nonresident_data_attribute(mft_run, mft_records_bytes,
                                               mft_records_bytes)
    struct.pack_into("<Q", mft_data_attr, 0x18, highest_vcn)  # HighestVCN

    # $MFT's resident $Bitmap: enough bits for the table, first four set.
    bitmap_bytes = (mft_record_count + 7) // 8
    bitmap_value = bytearray(bitmap_bytes)
    # Mark the NTFS system files (0-15) as allocated; record 5 is the root.
    bitmap_value[0:2] = b"\xff\xff"
    mft_bitmap_attr = resident_attribute(0xB0, "", bytes(bitmap_value), 1)  # $BITMAP

    # $LogFile resident value: two 0x200-byte restart pages, both clean.
    # RSTR page contract (NtfsValidateRestartPage / NtfsCheckLogFileClean):
    #   UsaOffset(0x04)=0x30, UsaCount(0x06)=SystemPage/512+1=2,
    #   UsaOffset + UsaCount*2 (0x34) must fit before the restart area,
    #   so the area starts at 0x38 (8-aligned, >=32, +16 <= page-2).
    #   Header 0x1C/0x1E hold major/minor version (1,1).
    log_value = bytearray(0x400)
    for i, magic in enumerate((b"RSTR", b"CHKD")):
        page = bytearray(0x200)
        page[0:4] = magic
        struct.pack_into("<H", page, 0x04, 0x30)   # UsaOffset
        struct.pack_into("<H", page, 0x06, 2)      # UsaCount
        struct.pack_into("<I", page, 0x10, 0x200)  # SystemPageSize
        struct.pack_into("<I", page, 0x14, 0x200)  # LogPageSize
        struct.pack_into("<H", page, 0x18, 0x38)   # RestartAreaOffset
        struct.pack_into("<H", page, 0x1C, 1)      # major version
        struct.pack_into("<H", page, 0x1E, 1)      # minor version
        struct.pack_into("<I", page, 0x30, 1)      # USA number word
        # Restart area: Lsn(8) | client_in_use(2)=0xFFFF | client_free(2)=0xFFFF
        # | client_array_offset(4)=0x10 | flags(2)=RESTART_VOLUME_IS_CLEAN
        struct.pack_into("<H", page, 0x38 + 8, 0xFFFF)
        struct.pack_into("<H", page, 0x38 + 10, 0xFFFF)
        struct.pack_into("<I", page, 0x38 + 12, 0x10)
        struct.pack_into("<H", page, 0x38 + 14, 0x0002)  # clean flag
        page[0x1FE:0x200] = struct.pack("<H", 1)   # fixup word (USA number)
        log_value[i * 0x200:(i + 1) * 0x200] = page
    log_data_attr = resident_attribute(0x80, "", bytes(log_value))

    # $Volume's resident $VOLUME_INFORMATION (8-byte value, version 3.1, clean)
    vol_info = struct.pack("<QBBH", 0, 3, 1, 0)
    vol_data_attr = resident_attribute(0x70, "", vol_info)

    # Root directory (record 5): $FILE_NAME self-reference + empty $INDEX_ROOT
    # $I30, mirroring what NtfsCreateFileRecord builds for a new directory.
    # FILENAME_ATTRIBUTE layout: parent@0, times@8..0x38, FileAttributes@0x38,
    # EaInfo/ReparseTag@0x3C, NameLength@0x40, NameType@0x41, Name@0x42.
    name_value = bytearray(0x42 + 2 * 4)
    struct.pack_into("<Q", name_value, 0x00, 5 << 48)  # parent = root itself
    struct.pack_into("<I", name_value, 0x38, 0x10000000)  # FileAttributes: dir
    name_value[0x40] = 4       # NameLength
    name_value[0x41] = 1       # NameType: WIN32
    name_value[0x42:0x4A] = ".".encode("utf-16-le")
    root_name_attr = resident_attribute(0x30, "", bytes(name_value))

    # Empty $INDEX_ROOT, byte-identical to what CreateIndexRootFromBTree
    # produces for a fresh directory: a 0x30-byte value = 0x10 fixed header
    # (SizeOfEntry is a ULONG = BytesPerIndexRecord, ClustersPerIndexRecord
    # at 0x0C) + 0x10 INDEX_HEADER + one terminating dummy entry with
    # NTFS_INDEX_ENTRY_END, Length 0x10 and no child VCN.  FirstEntryOffset
    # is relative to the INDEX_HEADER; TotalSizeOfEntries is the offset
    # (from value start) of the entry past the last real one.
    ir = bytearray(0x30)
    struct.pack_into("<I", ir, 0x00, 0x30)     # AttributeType: $FILE_NAME
    struct.pack_into("<I", ir, 0x04, 1)        # CollationRule: file name
    struct.pack_into("<I", ir, 0x08, 0x200)    # SizeOfEntry = index block size
    ir[0x0C] = 1                               # ClustersPerIndexRecord
    struct.pack_into("<I", ir, 0x10, 0x10)     # FirstEntryOffset
    struct.pack_into("<I", ir, 0x14, 0x20)     # TotalSizeOfEntries
    struct.pack_into("<I", ir, 0x18, 0x20)     # AllocatedSize
    struct.pack_into("<I", ir, 0x1C, 0)        # Flags: INDEX_ROOT_SMALL
    # Terminating dummy entry (Length 0x10, no key, no child VCN).
    struct.pack_into("<H", ir, 0x28, 0x10)     # entry Length @0x20+8
    struct.pack_into("<H", ir, 0x2C, 2)        # Flags @0x20+0xC: ENTRY_END
    root_index_attr = resident_attribute(0x90, "$I30", bytes(ir))

    # $MFTMirr's non-resident $DATA covering the first 2 MFT records.
    mirr_run = encode_run(mftmirr_cluster, 1) + b"\x00"
    mirr_data_attr = nonresident_data_attribute(mirr_run, 2 * 4096, 2 * 4096)
    struct.pack_into("<Q", mirr_data_attr, 0x18, 0)  # HighestVCN = 0

    # --- Build records ---------------------------------------------------------
    # $MFT: SI + non-resident DATA + BITMAP
    mft_attrs = bytearray()
    mft_attrs += resident_attribute(0x10, "", b"\x00" * 0x20)
    mft_attrs += mft_data_attr
    mft_attrs += mft_bitmap_attr
    # $MFTMirr: SI + non-resident DATA (first 2 records, mirrored at boot LCN)
    mirr_attrs = bytearray()
    mirr_attrs += resident_attribute(0x10, "", b"\x00" * 0x20)
    mirr_attrs += mirr_data_attr
    # $LogFile: SI + resident DATA (two restart pages)
    log_attrs = bytearray()
    log_attrs += resident_attribute(0x10, "", b"\x00" * 0x20)
    log_attrs += log_data_attr
    # $Volume: SI + VOLUME_INFORMATION
    vol_attrs = bytearray()
    vol_attrs += resident_attribute(0x10, "", b"\x00" * 0x20)
    vol_attrs += vol_data_attr
    # $Root: SI + FILE_NAME + INDEX_ROOT $I30 (directory flag set)
    root_attrs = bytearray()
    root_attrs += resident_attribute(0x10, "", b"\x00" * 0x20)
    root_attrs += root_name_attr
    root_attrs += root_index_attr

    records = [
        file_record(0, bytes(mft_attrs)),
        file_record(1, bytes(mirr_attrs)),
        file_record(2, bytes(log_attrs)),
        file_record(3, bytes(vol_attrs)),
        file_record(5, bytes(root_attrs), flags=0x1001),
    ]
    mft_data = bytearray(mft_record_count * 4096)
    for index, record in zip((0, 1, 2, 3, 5), records):
        offset = index * 4096
        mft_data[offset:offset + 4096] = record
    # Remaining MFT slots (4, 6..63) stay zeroed: magic-zero records the
    # allocator treats as free (bit clear in $Bitmap).
    image[mft_lcn:mft_lcn + len(mft_data)] = mft_data

    # $MFTMirr: mirror the first two records (one cluster is enough).
    mirror_lba = mftmirr_cluster * cluster_size
    image[mirror_lba:mirror_lba + 2 * 4096] = bytes(mft_data[:2 * 4096])

    return bytes(image)


def assert_allocation_publication_order() -> None:
    """Guard the MFT allocator's crash-order invariant without a host disk.

    A new MFT record must be inactive before its bitmap bit is published, and
    the in-use record must be published only after that bitmap write succeeds.
    """
    source = read_text(ROOT / "drivers" / "filesystems" / "ntfs" / "mft.c")
    start = source.find("AddNewMftEntry(PFILE_RECORD_HEADER")
    end = source.find("NTSTATUS\nRemoveNewMftEntry", start)
    if start < 0 or end < 0:
        fail("Could not locate AddNewMftEntry() for allocation-order validation")

    body = source[start:end]
    inactive = body.find("FileRecord->Flags &= ~FRH_IN_USE;")
    initial_publish = body.find("Status = UpdateFileRecord(DeviceExt, MftIndex, FileRecord);", inactive)
    bitmap_publish = body.find("Status = WriteAttribute(DeviceExt, BitmapContext, 0, BitmapData,", initial_publish)
    final_publish = body.find("Status = UpdateFileRecord(DeviceExt, MftIndex, FileRecord);", bitmap_publish)
    if min(inactive, initial_publish, bitmap_publish, final_publish) < 0:
        fail("MFT allocation publication steps are missing or out of order")
    if not inactive < initial_publish < bitmap_publish < final_publish:
        fail("MFT allocation publication order is unsafe")


def assert_journal_readback_order() -> None:
    """Require durable dirty-page readback before journal state is released."""
    source = read_text(ROOT / "drivers" / "filesystems" / "ntfs" / "fsctl.c")
    start = source.find("NtfsSetJournalState(PDEVICE_EXTENSION Vcb,")
    end = source.find("\nNTSTATUS\nNtfsPrepareForMetadataUpdate", start)
    if start < 0 or end < 0:
        fail("Could not locate NtfsSetJournalState() for readback validation")

    body = source[start:end]
    flush = body.find("Status = NtfsFlushJournalPages(Vcb);")
    verify = body.find("Status = NtfsVerifyJournalState(Vcb, DataContext,")
    release = body.find("ReleaseAttributeContext(DataContext);", verify)
    if min(flush, verify, release) < 0:
        fail("Journal durability readback steps are missing")
    if not flush < verify < release:
        fail("Journal dirty-page readback is not before state release")


def assert_crash_injection_points() -> None:
    """Keep the opt-in crash points present at the publication boundaries."""
    mft = read_text(ROOT / "drivers" / "filesystems" / "ntfs" / "mft.c")
    fsctl = read_text(ROOT / "drivers" / "filesystems" / "ntfs" / "fsctl.c")
    ntfs = read_text(ROOT / "drivers" / "filesystems" / "ntfs" / "ntfs.c")
    required_mft = (
        "NtfsCrashInjectPoint(NTFS_CRASH_BEFORE_MFT_BITMAP);",
        "NtfsCrashInjectPoint(NTFS_CRASH_AFTER_MFT_BITMAP);",
    )
    required_fsctl = (
        "NtfsCrashInjectPoint(NTFS_CRASH_BEFORE_RESTART_PAGE);",
        "NtfsCrashInjectPoint(NTFS_CRASH_AFTER_RESTART_PAGE);",
    )
    if any(point not in mft for point in required_mft):
        fail("MFT bitmap crash-injection points are incomplete")
    if any(point not in fsctl for point in required_fsctl):
        fail("Restart-page crash-injection points are incomplete")

    bitmap_write = mft.find("Status = WriteAttribute(DeviceExt, BitmapContext, 0, BitmapData,")
    bitmap_before = mft.rfind(required_mft[0], 0, bitmap_write)
    bitmap_after = mft.find(required_mft[1], bitmap_write)
    restart_write = fsctl.find("Status = WriteAttribute(Vcb, DataContext, Offset, Page,")
    restart_before = fsctl.rfind(required_fsctl[0], 0, restart_write)
    restart_after = fsctl.find(required_fsctl[1], restart_write)
    if min(bitmap_write, bitmap_before, bitmap_after) < 0 or not bitmap_before < bitmap_write < bitmap_after:
        fail("MFT crash-injection points are not around bitmap publication")
    if min(restart_write, restart_before, restart_after) < 0 or not restart_before < restart_write < restart_after:
        fail("Restart crash-injection points are not around restart-page publication")
    if "CrashInjectionMask" not in ntfs or "MANUALLY_INITIATED_CRASH" not in ntfs:
        fail("Crash injection is not registry-gated and fail-stop")


def run_case(qemu: Path, qemu_img: Path, image: Path, bootcd: Path,
             work: Path, port: int, command: str, marker: str | None,
             timeout: float = DEFAULT_TIMEOUT, settle: float = 2.0) -> str:
    tag = marker.replace(" ", "-") if marker else hashlib.sha1(command.encode()).hexdigest()[:12]
    serial = work / f"{tag}.serial.log"
    machine_serial = work / f"{tag}.com2.log"
    stderr = work / f"{tag}.qemu.err.log"
    boot = Boot(qemu, image, bootcd, serial, machine_serial, stderr, port)
    boot.start()
    time.sleep(35)
    try:
        output = run_guest_command(boot, command, marker, timeout, settle)
    finally:
        boot.stop()
    check_image(qemu_img, image)
    return output


def run_crash_case(qemu: Path, qemu_img: Path, image: Path, bootcd: Path,
                   work: Path, port: int, command: str) -> str:
    serial = work / "crash.serial.log"
    machine_serial = work / "crash.com2.log"
    stderr = work / "crash.qemu.err.log"
    boot = Boot(qemu, image, bootcd, serial, machine_serial, stderr, port)
    boot.start()
    time.sleep(35)
    assert boot.monitor is not None
    try:
        boot.monitor.run_dialog(command)
        time.sleep(2)
        if not boot.wait_for("NTFSREG CRASH-BEGIN", DEFAULT_TIMEOUT):
            output = boot.serial_text()
            fail(f"Crash phase did not start.\n{boot.diagnostic_text()}")
        # Deliberately terminate QEMU instead of using quit or a guest shutdown.
        # This models an interrupted write at the virtual-disk boundary.
        boot.stop(force=True)
    finally:
        boot.stop(force=True)
    check_image(qemu_img, image)
    return read_text(machine_serial)


def run_autorun(qemu: Path, qemu_img: Path, image: Path, bootcd: Path,
                work: Path, port: int, timeout: float) -> str:
    crash_serial = work / "autorun-crash.serial.log"
    crash_machine_serial = work / "autorun-crash.com2.log"
    crash_stderr = work / "autorun-crash.qemu.err.log"
    boot = Boot(qemu, image, bootcd, crash_serial, crash_machine_serial,
                crash_stderr, port, allow_reboot=True)
    boot.start()
    try:
        if not boot.wait_for("NTFSREG CRASH-BEGIN", timeout):
            output = boot.serial_text()
            fail(f"Autorun did not reach the interrupted-write phase.\n"
                 f"{boot.diagnostic_text()}")
        boot.stop(force=True)
    finally:
        boot.stop(force=True)
    check_image(qemu_img, image)

    verify_serial = work / "autorun-verify.serial.log"
    verify_machine_serial = work / "autorun-verify.com2.log"
    verify_stderr = work / "autorun-verify.qemu.err.log"
    boot = Boot(qemu, image, bootcd, verify_serial, verify_machine_serial,
                verify_stderr, port + 1)
    boot.start()
    try:
        if not boot.wait_for("NTFSREG END PASS", timeout):
            output = boot.serial_text()
            fail(f"Autorun recovery did not pass.\n{boot.diagnostic_text()}")
        output = boot.serial_text()
    finally:
        boot.stop()
    check_image(qemu_img, image)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--qemu-dir", type=Path, default=DEFAULT_QEMU)
    parser.add_argument("--workdir", type=Path,
                        help="directory for the disposable image and logs")
    parser.add_argument("--enable-write", action="store_true",
                        help="set the experimental NTFS write value in the disposable guest")
    parser.add_argument("--autorun", action="store_true",
                        help="use a bootcd configured with NTFS_REGRESSION_AUTORUN=ON")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = parser.parse_args()

    assert_allocation_publication_order()
    assert_journal_readback_order()
    assert_crash_injection_points()

    build = args.build.resolve()
    qemu_dir = args.qemu_dir.resolve()
    qemu = find_tool(qemu_dir, "qemu-system-x86_64.exe")
    qemu_img = find_tool(qemu_dir, "qemu-img.exe")
    bootcd = build / "bootcd.iso"
    if not bootcd.is_file():
        fail(f"Boot CD is missing: {bootcd}")

    work = args.workdir.resolve() if args.workdir else Path(
        tempfile.mkdtemp(prefix="ntfs-regression-", dir=str(build)))
    work.mkdir(parents=True, exist_ok=True)
    image = work / "ntfs-regression.raw"

    if image.exists():
        fail(f"Refusing to overwrite existing image: {image}")

    subprocess.run([str(qemu_img), "create", "-f", "raw", str(image), "256M"],
                   check=True)
    create_test_partition(image)
    check_image(qemu_img, image)

    port = 45440
    try:
        if args.autorun:
            output = run_autorun(qemu, qemu_img, image, bootcd, work, port,
                                 max(args.timeout, 300.0))
            if "NTFSREG FAIL" in output:
                fail("The autorun payload emitted a failure marker.")
            print(f"NTFS regression autorun passed. Image: {image}")
            print(f"Logs: {work}")
            return 0

        format_command = r"D:\reactos\system32\format.com C: /FS:NTFS /Q /V:NTFSREG"
        serial = work / "format.serial.log"
        machine_serial = work / "format.com2.log"
        stderr = work / "format.qemu.err.log"
        boot = Boot(qemu, image, bootcd, serial, machine_serial, stderr, port)
        boot.start()
        time.sleep(35)
        assert boot.monitor is not None
        boot.monitor.run_dialog(format_command)
        time.sleep(2)
        boot.monitor.key("y")
        boot.monitor.key("ret")
        time.sleep(35)
        boot.stop()
        check_image(qemu_img, image)
        port += 1

        if not args.enable_write:
            fail("The image is prepared, but --enable-write is required for write tests. "
                 "The harness never enables NTFS writes by default.")

        enable = (r"D:\reactos\system32\reg.exe add "
                  r"HKLM\System\CurrentControlSet\Services\Ntfs "
                  r"/v MyDataDoesNotMatterSoEnableExperimentalWriteSupportForEveryNTFSVolume "
                  r"/t REG_DWORD /d 1 /f")
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 enable, None, settle=5.0, timeout=20.0)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg full",
                 "NTFSREG END PASS", timeout=args.timeout)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg flush",
                 "NTFSREG END PASS", timeout=args.timeout)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg locks",
                 "NTFSREG END PASS", timeout=args.timeout)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg allocation",
                 "NTFSREG END PASS", timeout=args.timeout)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\reg.exe add "
                 r"HKLM\System\CurrentControlSet\Services\Ntfs "
                 r"/v MyDataDoesNotMatterSoEnableExperimentalWriteSupportForEveryNTFSVolume "
                 r"/t REG_DWORD /d 1 /f",
                 None, settle=5.0, timeout=20.0)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg remount",
                 "NTFSREG END PASS", timeout=args.timeout)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\reg.exe add "
                 r"HKLM\System\CurrentControlSet\Services\Ntfs "
                 r"/v MyDataDoesNotMatterSoEnableExperimentalWriteSupportForEveryNTFSVolume "
                 r"/t REG_DWORD /d 1 /f",
                 None, settle=5.0, timeout=20.0)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg verify-remount",
                 "NTFSREG END PASS", timeout=args.timeout)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\reg.exe add "
                 r"HKLM\System\CurrentControlSet\Services\Ntfs "
                 r"/v MyDataDoesNotMatterSoEnableExperimentalWriteSupportForEveryNTFSVolume "
                 r"/t REG_DWORD /d 1 /f",
                 None, settle=5.0, timeout=20.0)
        port += 1
        run_crash_case(qemu, qemu_img, image, bootcd, work, port,
                       r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg crash")
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\reg.exe add "
                 r"HKLM\System\CurrentControlSet\Services\Ntfs "
                 r"/v MyDataDoesNotMatterSoEnableExperimentalWriteSupportForEveryNTFSVolume "
                 r"/t REG_DWORD /d 1 /f",
                 None, settle=5.0, timeout=20.0)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg verify-crash",
                 "NTFSREG END PASS", timeout=args.timeout)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\reg.exe add "
                 r"HKLM\System\CurrentControlSet\Services\Ntfs "
                 r"/v MyDataDoesNotMatterSoEnableExperimentalWriteSupportForEveryNTFSVolume "
                 r"/t REG_DWORD /d 1 /f",
                 None, settle=5.0, timeout=20.0)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg dirty",
                 "NTFSREG END PASS", timeout=args.timeout)
        port += 1
        run_case(qemu, qemu_img, image, bootcd, work, port,
                 r"D:\reactos\system32\ntfs-regression.exe C:\ntfs-reg verify-dirty",
                 "NTFSREG END PASS", timeout=args.timeout)
    except Exception as exc:
        print(f"NTFS regression harness failed: {exc}", file=sys.stderr)
        print(f"Disposable image and logs: {work}", file=sys.stderr)
        return 1

    print(f"NTFS regression harness passed. Image: {image}")
    print(f"Logs: {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
