/*
 * PROJECT:     ReactOS File System Recognizer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     exFAT Recognizer
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

/* INCLUDES *****************************************************************/

#include "fs_rec.h"

#define NDEBUG
#include <debug.h>

/* TYPES ****************************************************************/

#include <pshpack1.h>
typedef struct _EXFAT_BOOT_SECTOR
{
    UCHAR JumpBoot[3];                 // 0x00
    UCHAR FileSystemName[8];           // 0x03: "EXFAT   "
    UCHAR MustBeZero[53];              // 0x0B
    ULONGLONG PartitionOffset;         // 0x40
    ULONGLONG VolumeLength;            // 0x48
    ULONG FatOffset;                   // 0x50
    ULONG FatLength;                   // 0x54
    ULONG ClusterHeapOffset;           // 0x58
    ULONG ClusterCount;                // 0x5C
    ULONG FirstClusterOfRootDirectory; // 0x60
    ULONG VolumeSerialNumber;          // 0x64
    USHORT FileSystemRevision;         // 0x68
    USHORT VolumeFlags;                // 0x6A
    UCHAR BytesPerSectorShift;         // 0x6C (9..12)
    UCHAR SectorsPerClusterShift;      // 0x6D
    UCHAR NumberOfFats;                // 0x6E (1 or 2)
    UCHAR DriveSelect;                 // 0x6F
    UCHAR PercentInUse;                // 0x70
    UCHAR Reserved[7];                 // 0x71
    UCHAR BootCode[390];               // 0x78
    USHORT BootSignature;              // 0x1FE (0xAA55)
} EXFAT_BOOT_SECTOR, *PEXFAT_BOOT_SECTOR;
#include <poppack.h>

/* FUNCTIONS ****************************************************************/

BOOLEAN
NTAPI
FsRecIsExFatVolume(IN PEXFAT_BOOT_SECTOR BootSector)
{
    PAGED_CODE();

    if (BootSector->FileSystemName[0] != 'E' ||
        BootSector->FileSystemName[1] != 'X' ||
        BootSector->FileSystemName[2] != 'F' ||
        BootSector->FileSystemName[3] != 'A' ||
        BootSector->FileSystemName[4] != 'T' ||
        BootSector->FileSystemName[5] != ' ' ||
        BootSector->FileSystemName[6] != ' ' ||
        BootSector->FileSystemName[7] != ' ')
    {
        return FALSE;
    }

    if (BootSector->BootSignature != 0xAA55)
    {
        return FALSE;
    }

    if (BootSector->BytesPerSectorShift < 9 || BootSector->BytesPerSectorShift > 12)
    {
        return FALSE;
    }

    if (BootSector->SectorsPerClusterShift > (25 - BootSector->BytesPerSectorShift))
    {
        return FALSE;
    }

    if (BootSector->NumberOfFats != 1 && BootSector->NumberOfFats != 2)
    {
        return FALSE;
    }

    return TRUE;
}

NTSTATUS
NTAPI
FsRecExFatFsControl(IN PDEVICE_OBJECT DeviceObject,
                    IN PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;
    PDEVICE_OBJECT MountDevice;
    PEXFAT_BOOT_SECTOR BootSector = NULL;
    ULONG SectorSize;
    LARGE_INTEGER Offset = {{0, 0}};
    BOOLEAN DeviceError = FALSE;
    PAGED_CODE();

    /* Get the I/O Stack and check the function type */
    Stack = IoGetCurrentIrpStackLocation(Irp);
    switch (Stack->MinorFunction)
    {
        case IRP_MN_MOUNT_VOLUME:

            /* Assume failure */
            Status = STATUS_UNRECOGNIZED_VOLUME;

            /* Get the device object and request the sector size */
            MountDevice = Stack->Parameters.MountVolume.DeviceObject;
            if (FsRecGetDeviceSectorSize(MountDevice, &SectorSize))
            {
                /* Try to read sector 0 */
                if (FsRecReadBlock(MountDevice,
                                   &Offset,
                                   512,
                                   SectorSize,
                                   (PVOID)&BootSector,
                                   &DeviceError))
                {
                    /* Check if it's an exFAT volume */
                    if (FsRecIsExFatVolume(BootSector))
                    {
                        Status = STATUS_FS_DRIVER_REQUIRED;
                    }
                }

                /* Free the boot sector if we have one */
                if (BootSector)
                {
                    ExFreePool(BootSector);
                }
            }

            break;

        case IRP_MN_LOAD_FILE_SYSTEM:

            /* Load the exFAT file system driver */
            Status = FsRecLoadFileSystem(DeviceObject,
                                         L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\exfat");
            break;

        default:

            Status = STATUS_INVALID_DEVICE_REQUEST;
    }

    return Status;
}

/* EOF */
