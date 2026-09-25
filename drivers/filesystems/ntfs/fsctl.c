/*
 *  ReactOS kernel
 *  Copyright (C) 2002 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystems/ntfs/fsctl.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMER:       Eric Kohl
 *                   Valentin Verkhovsky
 *                   Pierre Schweitzer
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#include <ntdddisk.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Tests if the device contains a filesystem that can be mounted
 *           by this fsd.
 */
static
NTSTATUS
NtfsHasFileSystem(PDEVICE_OBJECT DeviceToMount)
{
    PARTITION_INFORMATION PartitionInfo;
    DISK_GEOMETRY DiskGeometry;
    ULONG ClusterSize, Size, k;
    PBOOT_SECTOR BootSector;
    NTSTATUS Status;

    DPRINT("NtfsHasFileSystem() called\n");

    Size = sizeof(DISK_GEOMETRY);
    Status = NtfsDeviceIoControl(DeviceToMount,
                                 IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                 NULL,
                                 0,
                                 &DiskGeometry,
                                 &Size,
                                 TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtfsDeviceIoControl() failed (Status %lx)\n", Status);
        return Status;
    }

    if (DiskGeometry.MediaType == FixedMedia)
    {
        /* We have found a hard disk */
        Size = sizeof(PARTITION_INFORMATION);
        Status = NtfsDeviceIoControl(DeviceToMount,
                                     IOCTL_DISK_GET_PARTITION_INFO,
                                     NULL,
                                     0,
                                     &PartitionInfo,
                                     &Size,
                                     TRUE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("NtfsDeviceIoControl() failed (Status %lx)\n", Status);
            return Status;
        }

        if (PartitionInfo.PartitionType != PARTITION_IFS)
        {
            DPRINT1("Invalid partition type\n");
            return STATUS_UNRECOGNIZED_VOLUME;
        }
    }

    DPRINT1("BytesPerSector: %lu\n", DiskGeometry.BytesPerSector);
    BootSector = ExAllocatePoolWithTag(NonPagedPool,
                                       DiskGeometry.BytesPerSector,
                                       TAG_NTFS);
    if (BootSector == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsReadSectors(DeviceToMount,
                             0,
                             1,
                             DiskGeometry.BytesPerSector,
                             (PVOID)BootSector,
                             TRUE);
    if (!NT_SUCCESS(Status))
    {
        goto ByeBye;
    }

    /*
     * Check values of different fields. If those fields have not expected
     * values, we fail, to avoid mounting partitions that Windows won't mount.
     */

    /* OEMID: this field must be NTFS */
    if (RtlCompareMemory(BootSector->OEMID, "NTFS    ", 8) != 8)
    {
        DPRINT1("Failed with NTFS-identifier: [%.8s]\n", BootSector->OEMID);
        Status = STATUS_UNRECOGNIZED_VOLUME;
        goto ByeBye;
    }

    /* Unused0: this field must be COMPLETELY null */
    for (k = 0; k < 7; k++)
    {
        if (BootSector->BPB.Unused0[k] != 0)
        {
            DPRINT1("Failed in field Unused0: [%.7s]\n", BootSector->BPB.Unused0);
            Status = STATUS_UNRECOGNIZED_VOLUME;
            goto ByeBye;
        }
    }

    /* Unused3: this field must be COMPLETELY null */
    for (k = 0; k < 4; k++)
    {
        if (BootSector->BPB.Unused3[k] != 0)
        {
            DPRINT1("Failed in field Unused3: [%.4s]\n", BootSector->BPB.Unused3);
            Status = STATUS_UNRECOGNIZED_VOLUME;
            goto ByeBye;
        }
    }

    /* Check cluster size */
    ClusterSize = BootSector->BPB.BytesPerSector * BootSector->BPB.SectorsPerCluster;
    if (ClusterSize != 512 && ClusterSize != 1024 &&
        ClusterSize != 2048 && ClusterSize != 4096 &&
        ClusterSize != 8192 && ClusterSize != 16384 &&
        ClusterSize != 32768 && ClusterSize != 65536)
    {
        DPRINT1("Cluster size failed: %hu, %hu, %hu\n",
                BootSector->BPB.BytesPerSector,
                BootSector->BPB.SectorsPerCluster,
                ClusterSize);
        Status = STATUS_UNRECOGNIZED_VOLUME;
        goto ByeBye;
    }

ByeBye:
    ExFreePool(BootSector);

    return Status;
}


static
ULONG
NtfsQueryMftZoneReservation(VOID)
{
    ULONG ZoneReservation = 1;
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];

    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    QueryTable[0].Name = L"NtfsMftZoneReservation";
    QueryTable[0].EntryContext = &ZoneReservation;

    RtlQueryRegistryValues(RTL_REGISTRY_CONTROL,
                           L"FileSystem",
                           QueryTable,
                           NULL,
                           NULL);

    return ZoneReservation;
}


static
NTSTATUS
NtfsGetVolumeData(PDEVICE_OBJECT DeviceObject,
                  PDEVICE_EXTENSION DeviceExt)
{
    DISK_GEOMETRY DiskGeometry;
    PFILE_RECORD_HEADER VolumeRecord;
    PVOLINFO_ATTRIBUTE VolumeInfo;
    PBOOT_SECTOR BootSector;
    ULONG Size;
    PNTFS_INFO NtfsInfo = &DeviceExt->NtfsInfo;
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtxt;
    PNTFS_ATTR_RECORD Attribute;
    PNTFS_FCB VolumeFcb;
    PWSTR VolumeNameU;

    DPRINT("NtfsGetVolumeData() called\n");

    Size = sizeof(DISK_GEOMETRY);
    Status = NtfsDeviceIoControl(DeviceObject,
                                 IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                 NULL,
                                 0,
                                 &DiskGeometry,
                                 &Size,
                                 TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("NtfsDeviceIoControl() failed (Status %lx)\n", Status);
        return Status;
    }

    DPRINT("BytesPerSector: %lu\n", DiskGeometry.BytesPerSector);
    BootSector = ExAllocatePoolWithTag(NonPagedPool,
                                       DiskGeometry.BytesPerSector,
                                       TAG_NTFS);
    if (BootSector == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsReadSectors(DeviceObject,
                             0, /* Partition boot sector */
                             1,
                             DiskGeometry.BytesPerSector,
                             (PVOID)BootSector,
                             TRUE);
    if (!NT_SUCCESS(Status))
    {
        ExFreePool(BootSector);
        return Status;
    }

    /* Read data from the bootsector */
    NtfsInfo->BytesPerSector = BootSector->BPB.BytesPerSector;
    NtfsInfo->SectorsPerCluster = BootSector->BPB.SectorsPerCluster;
    NtfsInfo->BytesPerCluster = BootSector->BPB.BytesPerSector * BootSector->BPB.SectorsPerCluster;
    NtfsInfo->SectorCount = BootSector->EBPB.SectorCount;
    NtfsInfo->ClusterCount = DeviceExt->NtfsInfo.SectorCount / (ULONGLONG)DeviceExt->NtfsInfo.SectorsPerCluster;

    NtfsInfo->MftStart.QuadPart = BootSector->EBPB.MftLocation;
    NtfsInfo->MftMirrStart.QuadPart = BootSector->EBPB.MftMirrLocation;
    NtfsInfo->SerialNumber = BootSector->EBPB.SerialNumber;
    if (BootSector->EBPB.ClustersPerMftRecord > 0)
        NtfsInfo->BytesPerFileRecord = BootSector->EBPB.ClustersPerMftRecord * NtfsInfo->BytesPerCluster;
    else
        NtfsInfo->BytesPerFileRecord = 1 << (-BootSector->EBPB.ClustersPerMftRecord);
    if (BootSector->EBPB.ClustersPerIndexRecord > 0)
        NtfsInfo->BytesPerIndexRecord = BootSector->EBPB.ClustersPerIndexRecord * NtfsInfo->BytesPerCluster;
    else
        NtfsInfo->BytesPerIndexRecord = 1 << (-BootSector->EBPB.ClustersPerIndexRecord);

    DPRINT("Boot sector information:\n");
    DPRINT("  BytesPerSector:         %hu\n", BootSector->BPB.BytesPerSector);
    DPRINT("  SectorsPerCluster:      %hu\n", BootSector->BPB.SectorsPerCluster);
    DPRINT("  SectorCount:            %I64u\n", BootSector->EBPB.SectorCount);
    DPRINT("  MftStart:               %I64u\n", BootSector->EBPB.MftLocation);
    DPRINT("  MftMirrStart:           %I64u\n", BootSector->EBPB.MftMirrLocation);
    DPRINT("  ClustersPerMftRecord:   %lx\n", BootSector->EBPB.ClustersPerMftRecord);
    DPRINT("  ClustersPerIndexRecord: %lx\n", BootSector->EBPB.ClustersPerIndexRecord);
    DPRINT("  SerialNumber:           %I64x\n", BootSector->EBPB.SerialNumber);

    ExFreePool(BootSector);

    ExInitializeNPagedLookasideList(&DeviceExt->FileRecLookasideList,
                                    NULL, NULL, 0, NtfsInfo->BytesPerFileRecord, TAG_FILE_REC, 0);

    DeviceExt->MasterFileTable = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (DeviceExt->MasterFileTable == NULL)
    {
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsReadSectors(DeviceObject,
                             NtfsInfo->MftStart.u.LowPart * NtfsInfo->SectorsPerCluster,
                             NtfsInfo->BytesPerFileRecord / NtfsInfo->BytesPerSector,
                             NtfsInfo->BytesPerSector,
                             (PVOID)DeviceExt->MasterFileTable,
                             TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed reading MFT.\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return Status;
    }

    Status = FindAttribute(DeviceExt,
                           DeviceExt->MasterFileTable,
                           AttributeData,
                           L"",
                           0,
                           &DeviceExt->MFTContext,
                           &DeviceExt->MftDataOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Can't find data attribute for Master File Table.\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return Status;
    }

    VolumeRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (VolumeRecord == NULL)
    {
        DPRINT1("Allocation failed for volume record\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Read Volume File (MFT index 3) */
    DeviceExt->StorageDevice = DeviceObject;
    Status = ReadFileRecord(DeviceExt,
                            NTFS_FILE_VOLUME,
                            VolumeRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed reading volume file\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, VolumeRecord);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return Status;
    }

    /* Enumerate attributes */
    NtfsDumpFileAttributes(DeviceExt, DeviceExt->MasterFileTable);

    /* Enumerate attributes */
    NtfsDumpFileAttributes(DeviceExt, VolumeRecord);

    /* Get volume name */
    Status = FindAttribute(DeviceExt, VolumeRecord, AttributeVolumeName, L"", 0, &AttrCtxt, NULL);

    if (NT_SUCCESS(Status) && AttrCtxt->pRecord->Resident.ValueLength != 0)
    {
        Attribute = AttrCtxt->pRecord;
        DPRINT("Data length %lu\n", AttributeDataLength(Attribute));
        NtfsInfo->VolumeLabelLength =
           min (Attribute->Resident.ValueLength, MAXIMUM_VOLUME_LABEL_LENGTH);
        RtlCopyMemory(NtfsInfo->VolumeLabel,
                      (PVOID)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset),
                      NtfsInfo->VolumeLabelLength);
        VolumeNameU = NtfsInfo->VolumeLabel;
    }
    else
    {
        NtfsInfo->VolumeLabelLength = 0;
        VolumeNameU = L"\0";
    }

    if (NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(AttrCtxt);
    }

    VolumeFcb = NtfsCreateFCB(VolumeNameU, NULL, DeviceExt);
    if (VolumeFcb == NULL)
    {
        DPRINT1("Failed allocating volume FCB\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, VolumeRecord);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    VolumeFcb->Flags = FCB_IS_VOLUME;
    VolumeFcb->RFCB.FileSize.QuadPart = DeviceExt->NtfsInfo.SectorCount * DeviceExt->NtfsInfo.BytesPerSector;
    VolumeFcb->RFCB.ValidDataLength = VolumeFcb->RFCB.FileSize;
    VolumeFcb->RFCB.AllocationSize = VolumeFcb->RFCB.FileSize;
    VolumeFcb->MFTIndex = 0;
    DeviceExt->VolumeFcb = VolumeFcb;

    /* Get volume information */
    Status = FindAttribute(DeviceExt, VolumeRecord, AttributeVolumeInformation, L"", 0, &AttrCtxt, NULL);

    if (NT_SUCCESS(Status) && AttrCtxt->pRecord->Resident.ValueLength != 0)
    {
        Attribute = AttrCtxt->pRecord;
        DPRINT("Data length %lu\n", AttributeDataLength (Attribute));
        VolumeInfo = (PVOID)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);

        NtfsInfo->MajorVersion = VolumeInfo->MajorVersion;
        NtfsInfo->MinorVersion = VolumeInfo->MinorVersion;
        NtfsInfo->Flags = VolumeInfo->Flags;
    }

    if (NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(AttrCtxt);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, VolumeRecord);

    NtfsInfo->MftZoneReservation = NtfsQueryMftZoneReservation();

    return Status;
}


static
BOOLEAN
NtfsValidateRestartPage(PUCHAR Page,
                        ULONG PageSize,
                        ULONG BytesPerSector,
                        BOOLEAN FirstPage,
                        PULONG SystemPageSize);

static
BOOLEAN
NtfsCheckSecondRestartPage(PDEVICE_EXTENSION Vcb);

static
BOOLEAN
NtfsCheckLogFileClean(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER LogFileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    PUCHAR RestartPage;
    ULONGLONG DataLength;
    ULONG BytesRead;
    ULONG Magic;
    ULONG SystemPageSize;
    ULONG LogPageSize;
    ULONG UsaOffset;
    ULONG UsaCount;
    ULONG RestartAreaOffset;
    USHORT RestartFlags;
    BOOLEAN IsRestartPage;

    /* Temporary autorun diagnostic: preserve the first failed check in the
       otherwise-unused high VCB flag byte so the test payload can report it. */
    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000100;

    LogFileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!LogFileRecord)
    {
        return FALSE;
    }

    if (!NT_SUCCESS(ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, LogFileRecord)))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
        return FALSE;
    }

    if (!NT_SUCCESS(FindAttribute(Vcb, LogFileRecord, AttributeData, L"", 0,
                                   &DataContext, NULL)))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
        return FALSE;
    }

    DataLength = AttributeDataLength(DataContext->pRecord);
    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000300;
    if (DataContext->pRecord->IsNonResident || DataLength < Vcb->NtfsInfo.BytesPerSector)
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
        return FALSE;
    }

    RestartPage = ExAllocatePoolWithTag(NonPagedPool,
                                        Vcb->NtfsInfo.BytesPerSector,
                                        TAG_NTFS);
    if (!RestartPage)
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
        return FALSE;
    }

    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000400;
    BytesRead = ReadAttribute(Vcb, DataContext, 0, RestartPage,
                              Vcb->NtfsInfo.BytesPerSector);
    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);

    if (BytesRead != Vcb->NtfsInfo.BytesPerSector)
    {
        ExFreePoolWithTag(RestartPage, TAG_NTFS);
        return FALSE;
    }

    Magic = *(PULONG)RestartPage;
    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000500;
    IsRestartPage = (Magic == 0x52545352 || Magic == 0x444b4843); /* RSTR or CHKD */
    if (!IsRestartPage)
    {
        ExFreePoolWithTag(RestartPage, TAG_NTFS);
        return FALSE;
    }

    SystemPageSize = *(PULONG)(RestartPage + 0x10);
    LogPageSize = *(PULONG)(RestartPage + 0x14);
    UsaOffset = *(PUSHORT)(RestartPage + 0x04);
    UsaCount = *(PUSHORT)(RestartPage + 0x06);
    RestartAreaOffset = *(PUSHORT)(RestartPage + 0x18);

    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000600;
    if (SystemPageSize < Vcb->NtfsInfo.BytesPerSector ||
        SystemPageSize > 0x10000 ||
        (SystemPageSize & (SystemPageSize - 1)) != 0 ||
        LogPageSize < Vcb->NtfsInfo.BytesPerSector ||
        LogPageSize > SystemPageSize ||
        (LogPageSize & (LogPageSize - 1)) != 0 ||
        ((PUSHORT)(RestartPage + 0x1c))[1] != 1 ||
        ((PUSHORT)(RestartPage + 0x1c))[0] > 1 ||
        RestartAreaOffset < sizeof(ULONG) * 8 ||
        (RestartAreaOffset & 7) != 0 ||
        RestartAreaOffset + 16 > Vcb->NtfsInfo.BytesPerSector - 2 ||
        RestartAreaOffset + 16 > SystemPageSize)
    {
        ExFreePoolWithTag(RestartPage, TAG_NTFS);
        return FALSE;
    }

    if (Magic == 0x52545352 &&
        (UsaOffset < 0x1e ||
         UsaCount != SystemPageSize / Vcb->NtfsInfo.BytesPerSector + 1 ||
         UsaOffset + UsaCount * sizeof(USHORT) > RestartAreaOffset))
    {
        ExFreePoolWithTag(RestartPage, TAG_NTFS);
        return FALSE;
    }

    RestartFlags = *(PUSHORT)(RestartPage + RestartAreaOffset + 14);
    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000800;
    ExFreePoolWithTag(RestartPage, TAG_NTFS);

    /* This driver does not create native log-client records.  Require the
       explicit clean bit so clearing it is never mistaken for an idle log. */
    if (!(RestartFlags & 0x0002))
    {
        return FALSE;
    }

    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000900;
    IsRestartPage = NtfsCheckSecondRestartPage(Vcb);
    if (IsRestartPage)
        Vcb->Flags &= ~0x0000FF00;
    return IsRestartPage;
}


static
BOOLEAN
NtfsValidateRestartPage(PUCHAR Page,
                        ULONG PageSize,
                        ULONG BytesPerSector,
                        BOOLEAN FirstPage,
                        PULONG SystemPageSize)
{
    ULONG Magic;
    ULONG SystemSize;
    ULONG LogPageSize;
    ULONG UsaOffset;
    ULONG UsaCount;
    ULONG RestartAreaOffset;

    if (PageSize < 0x1e)
    {
        return FALSE;
    }

    Magic = *(PULONG)Page;
    if (Magic != 0x52545352 && Magic != 0x444b4843) /* RSTR or CHKD */
    {
        return FALSE;
    }

    SystemSize = *(PULONG)(Page + 0x10);
    LogPageSize = *(PULONG)(Page + 0x14);
    UsaOffset = *(PUSHORT)(Page + 0x04);
    UsaCount = *(PUSHORT)(Page + 0x06);
    RestartAreaOffset = *(PUSHORT)(Page + 0x18);

    if (SystemSize < BytesPerSector || SystemSize > 0x10000 ||
        (SystemSize & (SystemSize - 1)) != 0 ||
        LogPageSize < BytesPerSector || LogPageSize > SystemSize ||
        (LogPageSize & (LogPageSize - 1)) != 0 ||
        ((PUSHORT)(Page + 0x1c))[1] != 1 ||
        ((PUSHORT)(Page + 0x1c))[0] > 1 ||
        RestartAreaOffset < sizeof(ULONG) * 8 ||
        (RestartAreaOffset & 7) != 0 ||
        RestartAreaOffset + 16 > PageSize ||
        RestartAreaOffset + 16 > 510)
    {
        return FALSE;
    }

    if (Magic == 0x52545352 &&
        (UsaOffset < 0x1e ||
         UsaCount != SystemSize / BytesPerSector + 1 ||
         UsaOffset + UsaCount * sizeof(USHORT) > RestartAreaOffset))
    {
        return FALSE;
    }

    if (FirstPage)
    {
        *SystemPageSize = SystemSize;
    }
    else if (*SystemPageSize != SystemSize)
    {
        return FALSE;
    }

    return TRUE;
}


static
BOOLEAN
NtfsCheckSecondRestartPage(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER LogFileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    PUCHAR Header;
    PUCHAR SecondPage;
    ULONGLONG DataLength;
    ULONG SystemPageSize = 0;
    ULONG BytesRead;
    ULONG RestartAreaOffset;
    USHORT RestartFlags;
    BOOLEAN Result = FALSE;

    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000A00;
    LogFileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!LogFileRecord)
    {
        return FALSE;
    }
    if (!NT_SUCCESS(ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, LogFileRecord)))
    {
        goto Cleanup;
    }
    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000A10;
    if (!NT_SUCCESS(FindAttribute(Vcb, LogFileRecord, AttributeData, L"", 0,
                                   &DataContext, NULL)))
    {
        goto Cleanup;
    }

    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000A20;
    DataLength = AttributeDataLength(DataContext->pRecord);
    if (DataContext->pRecord->IsNonResident || DataLength < 0x400)
    {
        ReleaseAttributeContext(DataContext);
        goto Cleanup;
    }

    Header = ExAllocatePoolWithTag(NonPagedPool, 0x200, TAG_NTFS);
    if (!Header)
    {
        ReleaseAttributeContext(DataContext);
        goto Cleanup;
    }
    BytesRead = ReadAttribute(Vcb, DataContext, 0, Header, 0x200);
    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000A30;
    if (BytesRead != 0x200 ||
        !NtfsValidateRestartPage(Header, 0x200,
                                 Vcb->NtfsInfo.BytesPerSector, TRUE,
                                 &SystemPageSize) ||
        DataLength / SystemPageSize < 2)
    {
        ExFreePoolWithTag(Header, TAG_NTFS);
        ReleaseAttributeContext(DataContext);
        goto Cleanup;
    }
    ExFreePoolWithTag(Header, TAG_NTFS);

    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000A40;
    SecondPage = ExAllocatePoolWithTag(NonPagedPool, 0x200, TAG_NTFS);
    if (!SecondPage)
    {
        ReleaseAttributeContext(DataContext);
        goto Cleanup;
    }
    BytesRead = ReadAttribute(Vcb, DataContext, SystemPageSize,
                              SecondPage, 0x200);
    Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000A50;
    if (BytesRead == 0x200 &&
        NtfsValidateRestartPage(SecondPage, 0x200,
                                Vcb->NtfsInfo.BytesPerSector, FALSE,
                                &SystemPageSize))
    {
        Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000A60;
        RestartAreaOffset = *(PUSHORT)(SecondPage + 0x18);
        RestartFlags = *(PUSHORT)(SecondPage + RestartAreaOffset + 14);
        Result = (RestartFlags & 0x0002) != 0;
        if (Result)
            Vcb->Flags &= ~0x0000FF00;
    }
    ExFreePoolWithTag(SecondPage, TAG_NTFS);
    ReleaseAttributeContext(DataContext);

Cleanup:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
    return Result;
}


static
NTSTATUS
NtfsFlushJournalPages(PDEVICE_EXTENSION Vcb)
{
    PIRP Irp;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_FLUSH_BUFFERS,
                                       Vcb->StorageDevice,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &IoStatus);
    if (!Irp)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(Vcb->StorageDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    return Status;
}


static
NTSTATUS
NtfsWriteRestartPage(PDEVICE_EXTENSION Vcb,
                     PFILE_RECORD_HEADER LogFileRecord,
                     PNTFS_ATTR_CONTEXT DataContext,
                     ULONGLONG Offset,
                     ULONG SystemPageSize,
                     BOOLEAN Clean)
{
    PUCHAR Page;
    ULONG SystemSize;
    ULONG RestartAreaOffset;
    ULONG LengthWritten;
    ULONG BytesRead;
    NTSTATUS Status;

    Page = ExAllocatePoolWithTag(NonPagedPool, SystemPageSize, TAG_NTFS);
    if (!Page)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    SystemSize = SystemPageSize;
    BytesRead = ReadAttribute(Vcb, DataContext, Offset, Page, SystemPageSize);
    if (BytesRead != SystemPageSize ||
        !NtfsValidateRestartPage(Page, SystemPageSize,
                                 Vcb->NtfsInfo.BytesPerSector, FALSE,
                                 &SystemSize))
    {
        ExFreePoolWithTag(Page, TAG_NTFS);
        return STATUS_DATA_ERROR;
    }

    if (*(PULONG)Page != 0x444b4843 || *(PUSHORT)(Page + 0x06) != 0)
    {
        Status = FixupUpdateSequenceArray(Vcb, &((PINDEX_BUFFER)Page)->Ntfs);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Page, TAG_NTFS);
            return Status;
        }
    }

    RestartAreaOffset = *(PUSHORT)(Page + 0x18);
    if (Clean)
    {
        *(PUSHORT)(Page + RestartAreaOffset + 14) |= 0x0002;
    }
    else
    {
        *(PUSHORT)(Page + RestartAreaOffset + 14) &= (USHORT)~0x0002;
    }

    Status = AddFixupArray(Vcb, &((PINDEX_BUFFER)Page)->Ntfs);
    if (NT_SUCCESS(Status))
    {
        NtfsCrashInjectPoint(NTFS_CRASH_BEFORE_RESTART_PAGE);
        Status = WriteAttribute(Vcb, DataContext, Offset, Page,
                                SystemPageSize, &LengthWritten,
                                LogFileRecord);
        if (NT_SUCCESS(Status))
        {
            NtfsCrashInjectPoint(NTFS_CRASH_AFTER_RESTART_PAGE);
        }
        if (NT_SUCCESS(Status) && LengthWritten != SystemPageSize)
        {
            Status = STATUS_END_OF_FILE;
        }
    }

    ExFreePoolWithTag(Page, TAG_NTFS);
    return Status;
}


static
NTSTATUS
NtfsVerifyJournalState(PDEVICE_EXTENSION Vcb,
                        PNTFS_ATTR_CONTEXT DataContext,
                        ULONG SystemPageSize,
                        BOOLEAN Clean)
{
    PUCHAR Page;
    ULONG Offset;
    ULONG BytesRead;
    ULONG RestartAreaOffset;
    ULONG SystemSize = SystemPageSize;
    USHORT RestartFlags;
    NTSTATUS Status = STATUS_SUCCESS;

    Page = ExAllocatePoolWithTag(NonPagedPool, SystemPageSize, TAG_NTFS);
    if (!Page)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Re-read both complete pages from the storage path.  This is deliberately
       after the device flush and before the caller is allowed to mutate MFT,
       index, or file metadata. */
    for (Offset = 0; Offset <= SystemPageSize; Offset += SystemPageSize)
    {
        BytesRead = ReadAttribute(Vcb, DataContext, Offset, Page, SystemPageSize);
        if (BytesRead != SystemPageSize ||
            !NtfsValidateRestartPage(Page, SystemPageSize,
                                     Vcb->NtfsInfo.BytesPerSector,
                                     Offset == 0, &SystemSize))
        {
            Status = STATUS_DATA_ERROR;
            break;
        }

        RestartAreaOffset = *(PUSHORT)(Page + 0x18);
        RestartFlags = *(PUSHORT)(Page + RestartAreaOffset + 14);
        if ((Clean && !(RestartFlags & 0x0002)) ||
            (!Clean && (RestartFlags & 0x0002)))
        {
            Status = STATUS_DATA_ERROR;
            break;
        }
    }

    ExFreePoolWithTag(Page, TAG_NTFS);
    return Status;
}


static
NTSTATUS
NtfsSetJournalState(PDEVICE_EXTENSION Vcb,
                     BOOLEAN Clean)
{
    PFILE_RECORD_HEADER LogFileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    PUCHAR Header;
    ULONGLONG DataLength;
    ULONG SystemPageSize = 0;
    ULONG LengthRead;
    NTSTATUS Status;

    if (Vcb->Flags & VCB_VOLUME_DIRTY)
    {
        return STATUS_VOLUME_DIRTY;
    }

    ExAcquireResourceExclusiveLite(&Vcb->JournalResource, TRUE);
    LogFileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!LogFileRecord)
    {
        ExReleaseResourceLite(&Vcb->JournalResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, LogFileRecord);
    if (!NT_SUCCESS(Status))
    {
        goto Cleanup;
    }

    Status = FindAttribute(Vcb, LogFileRecord, AttributeData, L"", 0,
                           &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        goto Cleanup;
    }

    DataLength = AttributeDataLength(DataContext->pRecord);
    if (DataContext->pRecord->IsNonResident || DataLength < 0x200)
    {
        ReleaseAttributeContext(DataContext);
        Status = STATUS_DATA_ERROR;
        goto Cleanup;
    }

    Header = ExAllocatePoolWithTag(NonPagedPool, 0x200, TAG_NTFS);
    if (!Header)
    {
        ReleaseAttributeContext(DataContext);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    LengthRead = ReadAttribute(Vcb, DataContext, 0, Header, 0x200);
    if (LengthRead != 0x200 ||
        !NtfsValidateRestartPage(Header, 0x200,
                                 Vcb->NtfsInfo.BytesPerSector, TRUE,
                                 &SystemPageSize) ||
        DataLength / SystemPageSize < 2)
    {
        ExFreePoolWithTag(Header, TAG_NTFS);
        ReleaseAttributeContext(DataContext);
        Status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    ExFreePoolWithTag(Header, TAG_NTFS);

    /* The first restart page is the authoritative publication point. */
    if (Clean)
    {
        Status = NtfsWriteRestartPage(Vcb, LogFileRecord, DataContext,
                                      SystemPageSize, SystemPageSize, TRUE);
        if (NT_SUCCESS(Status))
        {
            Status = NtfsWriteRestartPage(Vcb, LogFileRecord, DataContext,
                                          0, SystemPageSize, TRUE);
        }
    }
    else
    {
        Status = NtfsWriteRestartPage(Vcb, LogFileRecord, DataContext,
                                      0, SystemPageSize, FALSE);
        if (NT_SUCCESS(Status))
        {
            Status = NtfsWriteRestartPage(Vcb, LogFileRecord, DataContext,
                                          SystemPageSize, SystemPageSize, FALSE);
        }
    }

    if (NT_SUCCESS(Status))
    {
        Status = NtfsFlushJournalPages(Vcb);
    }
    if (NT_SUCCESS(Status))
    {
        Status = NtfsVerifyJournalState(Vcb, DataContext,
                                        SystemPageSize, Clean);
    }

    ReleaseAttributeContext(DataContext);

    if (!NT_SUCCESS(Status))
    {
        Vcb->Flags |= VCB_VOLUME_DIRTY | VCB_JOURNAL_DIRTY;
        Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x0000D200;
    }
    else if (Clean)
    {
        Vcb->Flags &= ~VCB_JOURNAL_DIRTY;
        Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x0000D400;
    }
    else
    {
        Vcb->Flags |= VCB_JOURNAL_DIRTY;
        Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x0000D300;
    }

Cleanup:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList,
                                LogFileRecord);
    ExReleaseResourceLite(&Vcb->JournalResource);
    return Status;
}


NTSTATUS
NtfsPrepareForMetadataUpdate(PDEVICE_EXTENSION Vcb)
{
    if (Vcb->Flags & VCB_VOLUME_DIRTY)
    {
        return STATUS_VOLUME_DIRTY;
    }
    if (Vcb->Flags & VCB_JOURNAL_DIRTY)
    {
        return STATUS_SUCCESS;
    }
    return NtfsSetJournalState(Vcb, FALSE);
}


NTSTATUS
NtfsCheckpointJournal(PDEVICE_EXTENSION Vcb)
{
    if (!(Vcb->Flags & VCB_JOURNAL_DIRTY))
    {
        return STATUS_SUCCESS;
    }
    return NtfsSetJournalState(Vcb, TRUE);
}


VOID
NtfsMarkJournalFailure(PDEVICE_EXTENSION Vcb,
                       ULONG CallSite)
{
    Vcb->Flags |= VCB_VOLUME_DIRTY | VCB_JOURNAL_DIRTY;
    Vcb->Flags = (Vcb->Flags & ~0x00FFFF00) |
                 ((CallSite & 0x0000FFFF) << 8);
}


static
NTSTATUS
NtfsMountVolume(PDEVICE_OBJECT DeviceObject,
                PIRP Irp)
{
    PDEVICE_OBJECT NewDeviceObject = NULL;
    PDEVICE_OBJECT DeviceToMount;
    PIO_STACK_LOCATION Stack;
    PNTFS_FCB Fcb = NULL;
    PNTFS_CCB Ccb = NULL;
    PNTFS_VCB Vcb = NULL;
    NTSTATUS Status;
    BOOLEAN Lookaside = FALSE;

    DPRINT("NtfsMountVolume() called\n");

    if (DeviceObject != NtfsGlobalData->DeviceObject)
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto ByeBye;
    }

    Stack = IoGetCurrentIrpStackLocation(Irp);
    DeviceToMount = Stack->Parameters.MountVolume.DeviceObject;

    Status = NtfsHasFileSystem(DeviceToMount);
    if (!NT_SUCCESS(Status))
    {
        goto ByeBye;
    }

    Status = IoCreateDevice(NtfsGlobalData->DriverObject,
                            sizeof(DEVICE_EXTENSION),
                            NULL,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &NewDeviceObject);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    NewDeviceObject->Flags |= DO_DIRECT_IO;
    Vcb = (PVOID)NewDeviceObject->DeviceExtension;
    RtlZeroMemory(Vcb, sizeof(NTFS_VCB));

    Vcb->Identifier.Type = NTFS_TYPE_VCB;
    Vcb->Identifier.Size = sizeof(NTFS_TYPE_VCB);

    Status = NtfsGetVolumeData(DeviceToMount,
                               Vcb);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    if (!NtfsCheckLogFileClean(Vcb))
    {
        WARN_(NTFS, "NTFS volume is dirty or has an invalid $LogFile; writes will be denied.\n");
        Vcb->Flags |= VCB_VOLUME_DIRTY | VCB_JOURNAL_DIRTY;
    }
    else
    {
        Vcb->Flags &= ~(VCB_VOLUME_DIRTY | VCB_JOURNAL_DIRTY);
        Vcb->Flags = (Vcb->Flags & ~0x0000FF00) | 0x00000F00;
    }

    Lookaside = TRUE;

    NewDeviceObject->Vpb = DeviceToMount->Vpb;

    Vcb->StorageDevice = DeviceToMount;
    Vcb->StorageDevice->Vpb->DeviceObject = NewDeviceObject;
    Vcb->StorageDevice->Vpb->RealDevice = Vcb->StorageDevice;
    Vcb->StorageDevice->Vpb->Flags |= VPB_MOUNTED;
    NewDeviceObject->StackSize = Vcb->StorageDevice->StackSize + 1;
    NewDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    Vcb->StreamFileObject = IoCreateStreamFileObject(NULL,
                                                     Vcb->StorageDevice);

    InitializeListHead(&Vcb->FcbListHead);

    Fcb = NtfsCreateFCB(NULL, NULL, Vcb);
    if (Fcb == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ByeBye;
    }

    Ccb = ExAllocatePoolWithTag(NonPagedPool,
                                sizeof(NTFS_CCB),
                                TAG_CCB);
    if (Ccb == NULL)
    {
        Status =  STATUS_INSUFFICIENT_RESOURCES;
        goto ByeBye;
    }

    RtlZeroMemory(Ccb, sizeof(NTFS_CCB));

    Ccb->Identifier.Type = NTFS_TYPE_CCB;
    Ccb->Identifier.Size = sizeof(NTFS_TYPE_CCB);

    Vcb->StreamFileObject->FsContext = Fcb;
    Vcb->StreamFileObject->FsContext2 = Ccb;
    Vcb->StreamFileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    Vcb->StreamFileObject->PrivateCacheMap = NULL;
    Vcb->StreamFileObject->Vpb = Vcb->Vpb;
    Ccb->PtrFileObject = Vcb->StreamFileObject;
    Fcb->FileObject = Vcb->StreamFileObject;
    Fcb->Vcb = Vcb;

    Fcb->Flags = FCB_IS_VOLUME_STREAM;

    Fcb->RFCB.FileSize.QuadPart = Vcb->NtfsInfo.SectorCount * Vcb->NtfsInfo.BytesPerSector;
    Fcb->RFCB.ValidDataLength.QuadPart = Vcb->NtfsInfo.SectorCount * Vcb->NtfsInfo.BytesPerSector;
    Fcb->RFCB.AllocationSize.QuadPart = Vcb->NtfsInfo.SectorCount * Vcb->NtfsInfo.BytesPerSector; /* Correct? */

//    Fcb->Entry.ExtentLocationL = 0;
//    Fcb->Entry.DataLengthL = DeviceExt->CdInfo.VolumeSpaceSize * BLOCKSIZE;

    _SEH2_TRY
    {
        CcInitializeCacheMap(Vcb->StreamFileObject,
                             (PCC_FILE_SIZES)(&Fcb->RFCB.AllocationSize),
                             TRUE,
                             &(NtfsGlobalData->CacheMgrCallbacks),
                             Fcb);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        goto ByeBye;
    }
    _SEH2_END;

    ExInitializeResourceLite(&Vcb->DirResource);
    ExInitializeResourceLite(&Vcb->JournalResource);

    KeInitializeSpinLock(&Vcb->FcbListLock);

    /* Get serial number */
    NewDeviceObject->Vpb->SerialNumber = Vcb->NtfsInfo.SerialNumber;

    /* Get volume label */
    NewDeviceObject->Vpb->VolumeLabelLength = Vcb->NtfsInfo.VolumeLabelLength;
    RtlCopyMemory(NewDeviceObject->Vpb->VolumeLabel,
                  Vcb->NtfsInfo.VolumeLabel,
                  Vcb->NtfsInfo.VolumeLabelLength);

    FsRtlNotifyVolumeEvent(Vcb->StreamFileObject, FSRTL_VOLUME_MOUNT);

    Status = STATUS_SUCCESS;

ByeBye:
    if (!NT_SUCCESS(Status))
    {
        /* Cleanup */
        if (Vcb && Vcb->StreamFileObject)
            ObDereferenceObject(Vcb->StreamFileObject);

        if (Fcb)
            NtfsDestroyFCB(Fcb);

        if (Ccb)
            ExFreePool(Ccb);

        if (Lookaside)
            ExDeleteNPagedLookasideList(&Vcb->FileRecLookasideList);

        if (NewDeviceObject)
            IoDeleteDevice(NewDeviceObject);
    }

    DPRINT("NtfsMountVolume() done (Status: %lx)\n", Status);

    return Status;
}


static
NTSTATUS
NtfsVerifyVolume(PDEVICE_OBJECT DeviceObject,
                 PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    DPRINT1("NtfsVerifyVolume() called\n");
    return STATUS_WRONG_VOLUME;
}


static
NTSTATUS
GetNfsVolumeData(PDEVICE_EXTENSION DeviceExt,
                 PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    PNTFS_VOLUME_DATA_BUFFER DataBuffer;
    PNTFS_ATTR_RECORD Attribute;
    FIND_ATTR_CONTXT Context;
    NTSTATUS Status;

    DataBuffer = (PNTFS_VOLUME_DATA_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    Stack = IoGetCurrentIrpStackLocation(Irp);

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(NTFS_VOLUME_DATA_BUFFER) ||
        Irp->UserBuffer == NULL)
    {
        DPRINT1("Invalid output! %d %p\n", Stack->Parameters.FileSystemControl.OutputBufferLength, Irp->UserBuffer);
        return STATUS_INVALID_PARAMETER;
    }

    DataBuffer->VolumeSerialNumber.QuadPart = DeviceExt->NtfsInfo.SerialNumber;
    DataBuffer->NumberSectors.QuadPart = DeviceExt->NtfsInfo.SectorCount;
    DataBuffer->TotalClusters.QuadPart = DeviceExt->NtfsInfo.ClusterCount;
    DataBuffer->FreeClusters.QuadPart = NtfsGetFreeClusters(DeviceExt);
    DataBuffer->TotalReserved.QuadPart = 0LL; // FIXME
    DataBuffer->BytesPerSector = DeviceExt->NtfsInfo.BytesPerSector;
    DataBuffer->BytesPerCluster = DeviceExt->NtfsInfo.BytesPerCluster;
    DataBuffer->BytesPerFileRecordSegment = DeviceExt->NtfsInfo.BytesPerFileRecord;
    DataBuffer->ClustersPerFileRecordSegment = DeviceExt->NtfsInfo.BytesPerFileRecord / DeviceExt->NtfsInfo.BytesPerCluster;
    DataBuffer->MftStartLcn.QuadPart = DeviceExt->NtfsInfo.MftStart.QuadPart;
    DataBuffer->Mft2StartLcn.QuadPart = DeviceExt->NtfsInfo.MftMirrStart.QuadPart;
    DataBuffer->MftZoneStart.QuadPart = 0; // FIXME
    DataBuffer->MftZoneEnd.QuadPart = 0; // FIXME

    Status = FindFirstAttribute(&Context, DeviceExt, DeviceExt->MasterFileTable, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == AttributeData)
        {
            ASSERT(Attribute->IsNonResident);
            DataBuffer->MftValidDataLength.QuadPart = Attribute->NonResident.DataSize;

            break;
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }
    FindCloseAttribute(&Context);

    Irp->IoStatus.Information = sizeof(NTFS_VOLUME_DATA_BUFFER);

    if (Stack->Parameters.FileSystemControl.OutputBufferLength >= sizeof(NTFS_EXTENDED_VOLUME_DATA) + sizeof(NTFS_VOLUME_DATA_BUFFER))
    {
        PNTFS_EXTENDED_VOLUME_DATA ExtendedData = (PNTFS_EXTENDED_VOLUME_DATA)((ULONG_PTR)Irp->UserBuffer + sizeof(NTFS_VOLUME_DATA_BUFFER));

        ExtendedData->ByteCount = sizeof(NTFS_EXTENDED_VOLUME_DATA);
        ExtendedData->MajorVersion = DeviceExt->NtfsInfo.MajorVersion;
        ExtendedData->MinorVersion = DeviceExt->NtfsInfo.MinorVersion;
        Irp->IoStatus.Information += sizeof(NTFS_EXTENDED_VOLUME_DATA);
    }

    return STATUS_SUCCESS;
}


static
NTSTATUS
GetNtfsFileRecord(PDEVICE_EXTENSION DeviceExt,
                  PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PNTFS_FILE_RECORD_INPUT_BUFFER InputBuffer;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_FILE_RECORD_OUTPUT_BUFFER OutputBuffer;
    ULONGLONG MFTRecord;

    Stack = IoGetCurrentIrpStackLocation(Irp);

    if (Stack->Parameters.FileSystemControl.InputBufferLength < sizeof(NTFS_FILE_RECORD_INPUT_BUFFER) ||
        Irp->AssociatedIrp.SystemBuffer == NULL)
    {
        DPRINT1("Invalid input! %d %p\n", Stack->Parameters.FileSystemControl.InputBufferLength, Irp->AssociatedIrp.SystemBuffer);
        return STATUS_INVALID_PARAMETER;
    }

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < (FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + DeviceExt->NtfsInfo.BytesPerFileRecord) ||
        Irp->AssociatedIrp.SystemBuffer == NULL)
    {
        DPRINT1("Invalid output! %d %p\n", Stack->Parameters.FileSystemControl.OutputBufferLength, Irp->AssociatedIrp.SystemBuffer);
        return STATUS_BUFFER_TOO_SMALL;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    InputBuffer = (PNTFS_FILE_RECORD_INPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;

    MFTRecord = InputBuffer->FileReferenceNumber.QuadPart;
    DPRINT1("Requesting: %I64x\n", MFTRecord);

    do
    {
        Status = ReadFileRecord(DeviceExt, MFTRecord, FileRecord);
        if (NT_SUCCESS(Status))
        {
            if (FileRecord->Flags & FRH_IN_USE)
            {
                break;
            }
        }

        --MFTRecord;
    } while (TRUE);

    DPRINT1("Returning: %I64x\n", MFTRecord);
    OutputBuffer = (PNTFS_FILE_RECORD_OUTPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    OutputBuffer->FileReferenceNumber.QuadPart = MFTRecord;
    OutputBuffer->FileRecordLength = DeviceExt->NtfsInfo.BytesPerFileRecord;
    RtlCopyMemory(OutputBuffer->FileRecordBuffer, FileRecord, DeviceExt->NtfsInfo.BytesPerFileRecord);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    Irp->IoStatus.Information = FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + DeviceExt->NtfsInfo.BytesPerFileRecord;

    return STATUS_SUCCESS;
}


static
NTSTATUS
GetVolumeBitmap(PDEVICE_EXTENSION DeviceExt,
                PIRP Irp)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION Stack;
    PVOLUME_BITMAP_BUFFER BitmapBuffer;
    LONGLONG StartingLcn;
    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONGLONG TotalClusters;
    ULONGLONG ToCopy;
    BOOLEAN Overflow = FALSE;

    DPRINT("GetVolumeBitmap(%p, %p)\n", DeviceExt, Irp);

    Stack = IoGetCurrentIrpStackLocation(Irp);

    if (Stack->Parameters.FileSystemControl.InputBufferLength < sizeof(STARTING_LCN_INPUT_BUFFER))
    {
        DPRINT1("Invalid input! %d\n", Stack->Parameters.FileSystemControl.InputBufferLength);
        return STATUS_INVALID_PARAMETER;
    }

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(VOLUME_BITMAP_BUFFER))
    {
        DPRINT1("Invalid output! %d\n", Stack->Parameters.FileSystemControl.OutputBufferLength);
        return STATUS_BUFFER_TOO_SMALL;
    }

    BitmapBuffer = NtfsGetUserBuffer(Irp, FALSE);
    if (Irp->RequestorMode == UserMode)
    {
        _SEH2_TRY
        {
            ProbeForRead(Stack->Parameters.FileSystemControl.Type3InputBuffer,
                         Stack->Parameters.FileSystemControl.InputBufferLength,
                         sizeof(CHAR));
            ProbeForWrite(BitmapBuffer, Stack->Parameters.FileSystemControl.OutputBufferLength,
                          sizeof(CHAR));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }
    else
    {
        if (Stack->Parameters.FileSystemControl.Type3InputBuffer == NULL ||
            BitmapBuffer == NULL)
        {
            Status = STATUS_INVALID_PARAMETER;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Invalid buffer! %p %p\n", Stack->Parameters.FileSystemControl.Type3InputBuffer, BitmapBuffer);
        return Status;
    }

    StartingLcn = ((PSTARTING_LCN_INPUT_BUFFER)Stack->Parameters.FileSystemControl.Type3InputBuffer)->StartingLcn.QuadPart;
    if (StartingLcn > DeviceExt->NtfsInfo.ClusterCount)
    {
        DPRINT1("Requested bitmap start beyond partition end: %I64x %I64x\n", DeviceExt->NtfsInfo.ClusterCount, StartingLcn);
        return STATUS_INVALID_PARAMETER;
    }

    /* Round down to a multiple of 8 */
    StartingLcn = StartingLcn & ~7;
    TotalClusters = DeviceExt->NtfsInfo.ClusterCount - StartingLcn;
    ToCopy = TotalClusters / 8;
    if ((ToCopy + FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)) > Stack->Parameters.FileSystemControl.OutputBufferLength)
    {
        DPRINT1("Buffer too small: %x, needed: %x\n", Stack->Parameters.FileSystemControl.OutputBufferLength, (ToCopy + FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)));
        Overflow = TRUE;
        ToCopy = Stack->Parameters.FileSystemControl.OutputBufferLength - FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    }

    BitmapRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed reading volume bitmap: %lx\n", Status);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return Status;
    }

    Status = FindAttribute(DeviceExt, BitmapRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed find $DATA for bitmap: %lx\n", Status);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return Status;
    }

    BitmapBuffer->StartingLcn.QuadPart = StartingLcn;
    BitmapBuffer->BitmapSize.QuadPart = ToCopy * 8;

    Irp->IoStatus.Information = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    _SEH2_TRY
    {
        Irp->IoStatus.Information += ReadAttribute(DeviceExt, DataContext, StartingLcn / 8, (PCHAR)BitmapBuffer->Buffer, ToCopy);
        Status = (Overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);

    return Status;
}


static
NTSTATUS
LockOrUnlockVolume(PDEVICE_EXTENSION DeviceExt,
                   PIRP Irp,
                   BOOLEAN Lock)
{
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    DPRINT("LockOrUnlockVolume(%p, %p, %d)\n", DeviceExt, Irp, Lock);

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    if (!FileObject)
    {
        return STATUS_INVALID_HANDLE;
    }

    ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, TRUE);
    Fcb = FileObject->FsContext;
    if (!Fcb)
    {
        Status = STATUS_INVALID_HANDLE;
        goto Exit;
    }

    if (!(Fcb->Flags & FCB_IS_VOLUME))
    {
        Status = STATUS_ACCESS_DENIED;
        goto Exit;
    }

    if (((DeviceExt->Flags & VCB_VOLUME_LOCKED) && Lock) ||
        (!(DeviceExt->Flags & VCB_VOLUME_LOCKED) && !Lock))
    {
        Status = STATUS_ACCESS_DENIED;
        goto Exit;
    }

    if (Lock)
    {
        if (DeviceExt->VolumeLockOwner != NULL ||
            DeviceExt->OpenHandleCount != 1 ||
            DeviceExt->VolumeFcb->OpenHandleCount != 1)
        {
            Status = STATUS_ACCESS_DENIED;
            goto Exit;
        }

        DeviceExt->VolumeLockOwner = FileObject;
        DeviceExt->Flags |= VCB_VOLUME_LOCKED;
    }
    else
    {
        if (DeviceExt->VolumeLockOwner != FileObject)
        {
            Status = STATUS_ACCESS_DENIED;
            goto Exit;
        }

        DeviceExt->VolumeLockOwner = NULL;
        DeviceExt->Flags &= ~VCB_VOLUME_LOCKED;
    }

    Status = STATUS_SUCCESS;

Exit:
    ExReleaseResourceLite(&DeviceExt->DirResource);
    return Status;
}


static
NTSTATUS
NtfsUserFsRequest(PDEVICE_OBJECT DeviceObject,
                  PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PDEVICE_EXTENSION DeviceExt;

    DPRINT("NtfsUserFsRequest(%p, %p)\n", DeviceObject, Irp);

    Stack = IoGetCurrentIrpStackLocation(Irp);
    DeviceExt = DeviceObject->DeviceExtension;
    switch (Stack->Parameters.FileSystemControl.FsControlCode)
    {
        case FSCTL_CREATE_USN_JOURNAL:
        case FSCTL_DELETE_USN_JOURNAL:
        case FSCTL_ENUM_USN_DATA:
        case FSCTL_EXTEND_VOLUME:
        //case FSCTL_GET_RETRIEVAL_POINTER_BASE:
        case FSCTL_GET_RETRIEVAL_POINTERS:
        //case FSCTL_LOOKUP_STREAM_FROM_CLUSTER:
        case FSCTL_MARK_HANDLE:
        case FSCTL_MOVE_FILE:
        case FSCTL_QUERY_USN_JOURNAL:
        case FSCTL_READ_FILE_USN_DATA:
        case FSCTL_READ_USN_JOURNAL:
        //case FSCTL_SHRINK_VOLUME:
        case FSCTL_WRITE_USN_CLOSE_RECORD:
            UNIMPLEMENTED;
            DPRINT1("Unimplemented user request: %x\n", Stack->Parameters.FileSystemControl.FsControlCode);
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case FSCTL_LOCK_VOLUME:
            Status = LockOrUnlockVolume(DeviceExt, Irp, TRUE);
            break;

        case FSCTL_UNLOCK_VOLUME:
            Status = LockOrUnlockVolume(DeviceExt, Irp, FALSE);
            break;

        case FSCTL_IS_VOLUME_DIRTY:
            if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            ExAcquireResourceSharedLite(&DeviceExt->DirResource, TRUE);
            *(PULONG)Irp->AssociatedIrp.SystemBuffer =
                ((DeviceExt->Flags & (VCB_VOLUME_DIRTY | VCB_JOURNAL_DIRTY)) ?
                 VOLUME_IS_DIRTY : 0) |
                ((DeviceExt->Flags & 0x00FFFF00) << 8);
            ExReleaseResourceLite(&DeviceExt->DirResource);
            Irp->IoStatus.Information = sizeof(ULONG);
            Status = STATUS_SUCCESS;
            break;

        case FSCTL_MARK_VOLUME_DIRTY:
            if (!NtfsGlobalData->EnableWriteSupport)
            {
                Status = STATUS_ACCESS_DENIED;
            }
            else
            {
                ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, TRUE);
                Status = NtfsPrepareForMetadataUpdate(DeviceExt);
                if (NT_SUCCESS(Status))
                {
                    /* FSCTL_MARK_VOLUME_DIRTY is an explicit, durable
                       request to keep the volume read-only until reboot. */
                    DeviceExt->Flags |= VCB_VOLUME_DIRTY;
                    DeviceExt->Flags = (DeviceExt->Flags & ~0x0000FF00) | 0x0000D500;
                }
                ExReleaseResourceLite(&DeviceExt->DirResource);
            }
            break;

        case FSCTL_DISMOUNT_VOLUME:
            /* Teardown is not implemented; refusing is safer than reporting a false dismount. */
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case FSCTL_GET_NTFS_VOLUME_DATA:
            Status = GetNfsVolumeData(DeviceExt, Irp);
            break;

        case FSCTL_GET_NTFS_FILE_RECORD:
            Status = GetNtfsFileRecord(DeviceExt, Irp);
            break;

        case FSCTL_GET_VOLUME_BITMAP:
            Status = GetVolumeBitmap(DeviceExt, Irp);
            break;

        default:
            DPRINT("Invalid user request: %x\n", Stack->Parameters.FileSystemControl.FsControlCode);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return Status;
}


NTSTATUS
NtfsFileSystemControl(PNTFS_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;

    DPRINT("NtfsFileSystemControl() called\n");

    DeviceObject = IrpContext->DeviceObject;
    Irp = IrpContext->Irp;
    Irp->IoStatus.Information = 0;

    switch (IrpContext->MinorFunction)
    {
        case IRP_MN_KERNEL_CALL:
            DPRINT1("NTFS: IRP_MN_USER_FS_REQUEST\n");
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;

        case IRP_MN_USER_FS_REQUEST:
            Status = NtfsUserFsRequest(DeviceObject, Irp);
            break;

        case IRP_MN_MOUNT_VOLUME:
            DPRINT("NTFS: IRP_MN_MOUNT_VOLUME\n");
            Status = NtfsMountVolume(DeviceObject, Irp);
            break;

        case IRP_MN_VERIFY_VOLUME:
            DPRINT1("NTFS: IRP_MN_VERIFY_VOLUME\n");
            Status = NtfsVerifyVolume(DeviceObject, Irp);
            break;

        default:
            DPRINT1("NTFS FSC: MinorFunction %d\n", IrpContext->MinorFunction);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return Status;
}

/* EOF */
