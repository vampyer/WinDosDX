# Disposable NTFS regression harness

This harness runs the current `bootcd.iso` against a newly created raw disk image containing a disposable MBR partition. It never opens a physical host drive. The NTFS write registry value is opt-in and is changed only inside the disposable guest.

## Coverage

The guest payload `ntfs-regression.exe` checks:

- overwrite of an existing file;
- extension of a file and size verification;
- rename and delete;
- explicit `FlushFileBuffers` calls;
- volume-handle flushes and `FSCTL_IS_VOLUME_DIRTY` checkpoint assertions;
- volume lock/unlock exclusivity and denial of competing volume opens;
- MFT allocation publication-order source invariant plus multi-file allocation/readback;
- explicit dirty marking, dirty remount, and write-denial assertions;
- close/reopen and a second QEMU boot for remount verification;
- an interrupted write loop followed by QEMU termination and a second boot;
- recovery verification of the first persisted data block.

The host harness creates a raw disposable image with an MBR/NTFS partition (a blank RAW disk is not assigned a drive letter by the current loader), validates it with `qemu-img info` after every phase, and records QEMU stderr plus COM1/COM2 serial logs in a unique directory.

## Usage

Build the normal i386 targets first, including `ntfs-regression` and `bootcd`, then run from the repository root:

```text
python tools/ntfs-regression/ntfs-regression.py --enable-write
```

For a non-interactive run, configure and build a test-only bootcd, then use the autorun mode. This bypasses the LiveCD language page and launches the payload as the shell:

```text
cmake -S . -B output-VS-i386 -DNTFS_REGRESSION_AUTORUN=ON
cmake --build output-VS-i386 --target bootcd -j 4
python tools/ntfs-regression/ntfs-regression.py --autorun --timeout 300
```

The autorun mode enables experimental NTFS writes only in that disposable guest image and never changes the host registry. It uses COM2 for machine-readable payload markers; COM1 remains available for guest debug output.

### Test-only crash injection

The NTFS driver also accepts a `CrashInjectionMask` DWORD under the same
`Services\\Ntfs` registry key. It is ignored unless experimental NTFS writes are
enabled, and it is not set by the harness by default:

- `0x1`: crash immediately before MFT bitmap publication;
- `0x2`: crash immediately after MFT bitmap publication;
- `0x4`: crash immediately before a restart-page write;
- `0x8`: crash immediately after a restart-page write.

These values intentionally bugcheck the disposable guest and must never be
used on a physical NTFS volume.

For a controlled location and log retention:

```text
python tools/ntfs-regression/ntfs-regression.py --enable-write --workdir output-VS-i386/ntfs-regression-run
```

The script refuses to overwrite an existing image. Without `--enable-write`, it prepares and validates the disposable image but refuses to run write tests.

The interactive mode uses the QEMU monitor to type commands into the guest Run dialog and reads payload markers from COM2. It is intended for a disposable image only; do not point it at a real drive or replace the image argument with a physical device.
