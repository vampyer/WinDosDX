#!/usr/bin/env python3
"""Create an empty FAT16 test disk image (raw), without Windows or admin.

The image has an MBR with one FAT16 partition (type 0x06) starting at
sector 2048 and filling the disk, formatted like Windows' FORMAT /FS:FAT
would: 512-byte sectors, two FATs, a 512-entry root directory, and the
volume label FATREG, which the regression payload looks for.

    python make-fat-image.py out.raw [--size-mb 64]
"""

import argparse
import struct
import sys
import time
from pathlib import Path

SECTOR = 512
PART_START = 2048
LABEL = b"FATREG     "


def cluster_size(sectors: int) -> int:
    """Sectors per cluster, as FORMAT picks them for FAT16."""
    mb = sectors * SECTOR // (1024 * 1024)
    for limit, spc in ((16, 2), (128, 4), (256, 8), (512, 16), (1024, 32), (2048, 64)):
        if mb <= limit:
            return spc
    raise SystemExit("FAT16 volumes are at most 2 GB")


def make(path: Path, size_mb: int) -> None:
    total = size_mb * 1024 * 1024 // SECTOR
    part_sectors = total - PART_START
    spc = cluster_size(part_sectors)
    reserved = 1
    root_entries = 512
    root_sectors = root_entries * 32 // SECTOR
    # FAT size: enough 16-bit entries for every cluster plus the two reserved.
    fat_sectors = 1
    while True:
        data = part_sectors - reserved - 2 * fat_sectors - root_sectors
        clusters = data // spc
        if (clusters + 2) * 2 <= fat_sectors * SECTOR:
            break
        fat_sectors += 1
    if not 4085 <= clusters < 65525:
        raise SystemExit(f"{clusters} clusters is not a FAT16 volume")

    image = bytearray(total * SECTOR)

    # MBR: one active FAT16 partition (LBA addressing; CHS fields filled
    # with the usual "beyond 8 GB" values).
    mbr = bytearray(SECTOR)
    struct.pack_into("<I", mbr, 440, int(time.time()) & 0xFFFFFFFF)  # disk signature
    entry = struct.pack("<B3sB3sII", 0x80, b"\xFE\xFF\xFF", 0x06, b"\xFE\xFF\xFF",
                        PART_START, part_sectors)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    image[0:SECTOR] = mbr

    # FAT16 boot sector.
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"MSDOS5.0"
    struct.pack_into("<HBHBHHBHHHII", bs, 11,
                     SECTOR, spc, reserved, 2, root_entries,
                     part_sectors if part_sectors < 65536 else 0,
                     0xF8, fat_sectors, 63, 255, PART_START,
                     part_sectors if part_sectors >= 65536 else 0)
    struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29,
                     int(time.time() * 7) & 0xFFFFFFFF, LABEL, b"FAT16   ")
    bs[510:512] = b"\x55\xAA"
    base = PART_START * SECTOR
    image[base:base + SECTOR] = bs

    # Two FATs: media byte and end-of-chain in the reserved entries.
    for n in range(2):
        fat = base + (reserved + n * fat_sectors) * SECTOR
        image[fat:fat + 4] = b"\xF8\xFF\xFF\xFF"

    # Root directory: the volume label entry.
    root = base + (reserved + 2 * fat_sectors) * SECTOR
    label = bytearray(32)
    label[0:11] = LABEL
    label[11] = 0x08
    image[root:root + 32] = label

    path.write_bytes(image)
    print(f"{path}: {size_mb} MB, FAT16, {clusters} clusters of {spc * SECTOR} bytes, label FATREG")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=Path)
    parser.add_argument("--size-mb", type=int, default=64)
    args = parser.parse_args()
    if args.image.exists():
        raise SystemExit(f"{args.image} already exists")
    make(args.image, args.size_mb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
