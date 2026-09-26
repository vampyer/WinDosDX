/*
 * PROJECT:     exFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     FAT, allocation bitmap and cluster chains
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 *              Based on the vfatfs FAT routines by the ReactOS Team
 */

/* INCLUDES *****************************************************************/

#include "vfat.h"

#define NDEBUG
#include <debug.h>

#define CACHEPAGESIZE(pDeviceExt) ((pDeviceExt)->FatInfo.BytesPerCluster > PAGE_SIZE ? \
                                   (pDeviceExt)->FatInfo.BytesPerCluster : PAGE_SIZE)

/* FUNCTIONS ****************************************************************/

static
BOOLEAN
ExfatIsDataCluster(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster)
{
    return Cluster >= EXFAT_FIRST_DATA_CLUSTER &&
           Cluster - EXFAT_FIRST_DATA_CLUSTER < DeviceExt->FatInfo.NumberOfClusters;
}

static
VOID
ExfatReportCorruption(
    PCSTR What,
    ULONG Value)
{
    DPRINT1("exFAT: file system corruption detected (%s 0x%lx). You may need to run a disk repair utility.\n",
            What, Value);
}

/*
 * FUNCTION: Read one 32-bit FAT entry through the cached FAT stream
 */
NTSTATUS
ExfatReadFatEntry(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster,
    PULONG Value)
{
    PVOID BaseAddress;
    PVOID Context;
    LARGE_INTEGER Offset;
    ULONG ChunkSize = CACHEPAGESIZE(DeviceExt);
    ULONG FatOffset = Cluster * sizeof(ULONG);

    if (!ExfatIsDataCluster(DeviceExt, Cluster))
    {
        ExfatReportCorruption("FAT read of cluster", Cluster);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Offset.QuadPart = ROUND_DOWN(FatOffset, ChunkSize);
    _SEH2_TRY
    {
        if (!CcMapData(DeviceExt->FATFileObject, &Offset, ChunkSize, MAP_WAIT, &Context, &BaseAddress))
        {
            _SEH2_YIELD(return STATUS_UNSUCCESSFUL);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    *Value = *(PULONG)((PUCHAR)BaseAddress + (FatOffset % ChunkSize));
    CcUnpinData(Context);
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Write one 32-bit FAT entry through the cached FAT stream
 */
NTSTATUS
ExfatWriteFatEntry(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster,
    ULONG Value)
{
    PVOID BaseAddress;
    PVOID Context;
    LARGE_INTEGER Offset;
    ULONG ChunkSize = CACHEPAGESIZE(DeviceExt);
    ULONG FatOffset = Cluster * sizeof(ULONG);

    if (!ExfatIsDataCluster(DeviceExt, Cluster))
    {
        ExfatReportCorruption("FAT write of cluster", Cluster);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Offset.QuadPart = ROUND_DOWN(FatOffset, ChunkSize);
    _SEH2_TRY
    {
        CcPinRead(DeviceExt->FATFileObject, &Offset, ChunkSize, PIN_WAIT, &Context, &BaseAddress);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    *(PULONG)((PUCHAR)BaseAddress + (FatOffset % ChunkSize)) = Value;
    CcSetDirtyPinnedData(Context, NULL);
    CcUnpinData(Context);
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Load the allocation bitmap into memory. The caller has filled
 *           BitmapChain and BitmapBytes from the root directory's bitmap entry.
 */
NTSTATUS
ExfatLoadBitmap(
    PDEVICE_EXTENSION DeviceExt)
{
    ULONG BytesPerCluster = DeviceExt->FatInfo.BytesPerCluster;
    ULONG ClusterCount;
    ULONG BufferBytes;
    ULONG DirtyBits;
    ULONG Cluster;
    ULONG i;
    PUCHAR Buffer;
    PULONG DirtyBuffer;
    LARGE_INTEGER Offset;
    NTSTATUS Status;

    if (DeviceExt->BitmapBytes < (DeviceExt->FatInfo.NumberOfClusters + 7) / 8)
    {
        ExfatReportCorruption("allocation bitmap length", DeviceExt->BitmapBytes);
        return STATUS_DISK_CORRUPT_ERROR;
    }

    ClusterCount = (DeviceExt->BitmapBytes + BytesPerCluster - 1) / BytesPerCluster;
    BufferBytes = ClusterCount * BytesPerCluster;

    DeviceExt->BitmapClusters = ExAllocatePoolWithTag(PagedPool, ClusterCount * sizeof(ULONG), TAG_BUFFER);
    Buffer = ExAllocatePoolWithTag(PagedPool, BufferBytes, TAG_BUFFER);
    DirtyBits = BufferBytes / DeviceExt->FatInfo.BytesPerSector;
    DirtyBuffer = ExAllocatePoolWithTag(PagedPool, ROUND_UP(DirtyBits, 32) / 8, TAG_BUFFER);
    if (!DeviceExt->BitmapClusters || !Buffer || !DirtyBuffer)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Failed;
    }

    /* The bitmap's own clusters are FAT-chained. */
    DeviceExt->BitmapChain.Count = ClusterCount;
    Cluster = DeviceExt->BitmapChain.FirstCluster;
    for (i = 0; i < ClusterCount; i++)
    {
        if (!ExfatIsDataCluster(DeviceExt, Cluster))
        {
            ExfatReportCorruption("allocation bitmap cluster", Cluster);
            Status = STATUS_DISK_CORRUPT_ERROR;
            goto Failed;
        }
        DeviceExt->BitmapClusters[i] = Cluster;

        Offset.QuadPart = ClusterToSector(DeviceExt, Cluster) * DeviceExt->FatInfo.BytesPerSector;
        Status = VfatReadDisk(DeviceExt->StorageDevice, &Offset, BytesPerCluster,
                              Buffer + i * BytesPerCluster, FALSE);
        if (!NT_SUCCESS(Status))
            goto Failed;

        if (i + 1 < ClusterCount)
        {
            if (DeviceExt->BitmapChain.NoFatChain)
            {
                Cluster++;
            }
            else
            {
                Status = ExfatReadFatEntry(DeviceExt, Cluster, &Cluster);
                if (!NT_SUCCESS(Status))
                    goto Failed;
            }
        }
    }

    RtlInitializeBitMap(&DeviceExt->Bitmap, (PULONG)Buffer, DeviceExt->FatInfo.NumberOfClusters);
    RtlInitializeBitMap(&DeviceExt->BitmapDirty, DirtyBuffer, DirtyBits);
    RtlClearAllBits(&DeviceExt->BitmapDirty);
    return STATUS_SUCCESS;

Failed:
    if (DeviceExt->BitmapClusters)
        ExFreePoolWithTag(DeviceExt->BitmapClusters, TAG_BUFFER);
    if (Buffer)
        ExFreePoolWithTag(Buffer, TAG_BUFFER);
    if (DirtyBuffer)
        ExFreePoolWithTag(DirtyBuffer, TAG_BUFFER);
    DeviceExt->BitmapClusters = NULL;
    return Status;
}

VOID
ExfatFreeBitmap(
    PDEVICE_EXTENSION DeviceExt)
{
    if (DeviceExt->Bitmap.Buffer)
        ExFreePoolWithTag(DeviceExt->Bitmap.Buffer, TAG_BUFFER);
    if (DeviceExt->BitmapDirty.Buffer)
        ExFreePoolWithTag(DeviceExt->BitmapDirty.Buffer, TAG_BUFFER);
    if (DeviceExt->BitmapClusters)
        ExFreePoolWithTag(DeviceExt->BitmapClusters, TAG_BUFFER);
    DeviceExt->Bitmap.Buffer = NULL;
    DeviceExt->BitmapDirty.Buffer = NULL;
    DeviceExt->BitmapClusters = NULL;
}

static
VOID
ExfatSetBitmapRange(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster,
    ULONG Count,
    BOOLEAN Allocated)
{
    ULONG Bit = Cluster - EXFAT_FIRST_DATA_CLUSTER;
    ULONG BitsPerSector = DeviceExt->FatInfo.BytesPerSector * 8;
    ULONG FirstSector = Bit / BitsPerSector;
    ULONG LastSector = (Bit + Count - 1) / BitsPerSector;

    if (Allocated)
        RtlSetBits(&DeviceExt->Bitmap, Bit, Count);
    else
        RtlClearBits(&DeviceExt->Bitmap, Bit, Count);
    RtlSetBits(&DeviceExt->BitmapDirty, FirstSector, LastSector - FirstSector + 1);

    if (DeviceExt->AvailableClustersValid)
    {
        if (Allocated)
            DeviceExt->AvailableClusters -= Count;
        else
            DeviceExt->AvailableClusters += Count;
    }
}

/*
 * FUNCTION: Write the changed sectors of the allocation bitmap back to disk,
 *           each as a whole, aligned sector.
 */
NTSTATUS
ExfatFlushBitmap(
    PDEVICE_EXTENSION DeviceExt)
{
    return ExfatFlushBitmapEx(DeviceExt, TRUE);
}

/*
 * The lazy writer calls this (through VfatAcquireForLazyWrite) before it
 * writes FAT or directory pages, so the bitmap marks clusters in use before
 * anything on disk refers to them. Without Wait, it gives up when the
 * bitmap is busy and returns STATUS_CANT_WAIT.
 */
NTSTATUS
ExfatFlushBitmapEx(
    PDEVICE_EXTENSION DeviceExt,
    BOOLEAN Wait)
{
    ULONG BytesPerSector = DeviceExt->FatInfo.BytesPerSector;
    ULONG SectorsPerCluster = DeviceExt->FatInfo.SectorsPerCluster;
    ULONG Sector;
    ULONG Run;
    ULONG Hint = 0;
    LARGE_INTEGER Offset;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!DeviceExt->Bitmap.Buffer)
        return STATUS_SUCCESS;

    if (!ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, Wait))
        return STATUS_CANT_WAIT;
    while ((Sector = RtlFindSetBits(&DeviceExt->BitmapDirty, 1, Hint)) != MAXULONG)
    {
        if (Sector < Hint)
            break;  /* wrapped around: everything has been written */

        /* Write the run of dirty sectors that stays within one cluster. */
        Run = 1;
        while ((Sector + Run) % SectorsPerCluster != 0 &&
               Sector + Run < DeviceExt->BitmapDirty.SizeOfBitMap &&
               RtlCheckBit(&DeviceExt->BitmapDirty, Sector + Run))
        {
            Run++;
        }

        Offset.QuadPart = (ClusterToSector(DeviceExt, DeviceExt->BitmapClusters[Sector / SectorsPerCluster]) +
                           Sector % SectorsPerCluster) * BytesPerSector;
        Status = VfatWriteDisk(DeviceExt->StorageDevice, &Offset, Run * BytesPerSector,
                               (PUCHAR)DeviceExt->Bitmap.Buffer + Sector * BytesPerSector, FALSE);
        if (!NT_SUCCESS(Status))
            break;

        RtlClearBits(&DeviceExt->BitmapDirty, Sector, Run);
        Hint = Sector + Run;
    }
    ExReleaseResourceLite(&DeviceExt->FatResource);
    return Status;
}

/*
 * FUNCTION: Put the allocation state on disk: the bitmap, then the FAT.
 *           Directory pages refer to clusters through both, so this runs
 *           before any directory page is written. Without Wait it gives up
 *           when that would block, returning STATUS_CANT_WAIT.
 */
NTSTATUS
ExfatFlushAllocationEx(
    PDEVICE_EXTENSION DeviceExt,
    BOOLEAN Wait)
{
    IO_STATUS_BLOCK IoStatus;
    PVFATFCB FatFcb;
    NTSTATUS Status;

    Status = ExfatFlushBitmapEx(DeviceExt, Wait);
    if (!NT_SUCCESS(Status) || !DeviceExt->FATFileObject)
        return Status;

    FatFcb = DeviceExt->FATFileObject->FsContext;
    if (!Wait)
    {
        /* Flushing the FAT blocks on I/O; only do it when allowed to wait. */
        return STATUS_CANT_WAIT;
    }
    CcFlushCache(&FatFcb->SectionObjectPointers, NULL, 0, &IoStatus);
    return IoStatus.Status;
}

NTSTATUS
ExfatFlushAllocation(
    PDEVICE_EXTENSION DeviceExt)
{
    return ExfatFlushAllocationEx(DeviceExt, TRUE);
}

/*
 * FUNCTION: Allocate one free cluster, preferably the one at Hint. The
 *           cluster is marked in the bitmap and its FAT entry set to end of
 *           chain, so it can start or extend a FAT chain.
 *           The caller holds FatResource exclusively.
 */
NTSTATUS
ExfatAllocateCluster(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Hint,
    PULONG Cluster)
{
    ULONG Bit;
    NTSTATUS Status;

    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        return STATUS_MEDIA_WRITE_PROTECTED;

    if (!ExfatIsDataCluster(DeviceExt, Hint))
        Hint = DeviceExt->LastAvailableCluster;
    if (!ExfatIsDataCluster(DeviceExt, Hint))
        Hint = EXFAT_FIRST_DATA_CLUSTER;

    Bit = RtlFindClearBits(&DeviceExt->Bitmap, 1, Hint - EXFAT_FIRST_DATA_CLUSTER);
    if (Bit == MAXULONG)
        return STATUS_DISK_FULL;

    *Cluster = Bit + EXFAT_FIRST_DATA_CLUSTER;
    Status = ExfatWriteFatEntry(DeviceExt, *Cluster, EXFAT_CLUSTER_EOF);
    if (!NT_SUCCESS(Status))
        return Status;

    ExfatSetBitmapRange(DeviceExt, *Cluster, 1, TRUE);
    DeviceExt->LastAvailableCluster = *Cluster + 1;
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Give a contiguous ("no FAT chain") allocation FAT links, so it
 *           can grow by clusters that are not adjacent.
 *           The caller holds FatResource exclusively.
 */
NTSTATUS
ExfatConvertToFatChain(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_CHAIN Chain)
{
    ULONG i;
    NTSTATUS Status;

    if (!Chain->NoFatChain)
        return STATUS_SUCCESS;

    for (i = 0; i < Chain->Count; i++)
    {
        Status = ExfatWriteFatEntry(DeviceExt, Chain->FirstCluster + i,
                                    i + 1 < Chain->Count ? Chain->FirstCluster + i + 1 : EXFAT_CLUSTER_EOF);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    Chain->NoFatChain = FALSE;
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Keep the first KeepClusters clusters of a chain and free the rest.
 *           The caller holds FatResource exclusively.
 */
NTSTATUS
ExfatFreeClusters(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_CHAIN Chain,
    ULONG KeepClusters)
{
    ULONG Cluster;
    ULONG Next;
    ULONG i;
    NTSTATUS Status;

    if (Chain->FirstCluster == 0 || KeepClusters >= Chain->Count)
        return STATUS_SUCCESS;

    if (Chain->NoFatChain)
    {
        ExfatSetBitmapRange(DeviceExt, Chain->FirstCluster + KeepClusters,
                            Chain->Count - KeepClusters, FALSE);
    }
    else
    {
        /* Find the last cluster to keep and end the chain there. */
        Cluster = Chain->FirstCluster;
        for (i = 1; i < KeepClusters; i++)
        {
            Status = ExfatReadFatEntry(DeviceExt, Cluster, &Cluster);
            if (!NT_SUCCESS(Status))
                return Status;
        }
        if (KeepClusters > 0)
        {
            Status = ExfatReadFatEntry(DeviceExt, Cluster, &Next);
            if (!NT_SUCCESS(Status))
                return Status;
            Status = ExfatWriteFatEntry(DeviceExt, Cluster, EXFAT_CLUSTER_EOF);
            if (!NT_SUCCESS(Status))
                return Status;
            Cluster = Next;
        }

        for (i = KeepClusters; i < Chain->Count; i++)
        {
            if (!ExfatIsDataCluster(DeviceExt, Cluster))
            {
                ExfatReportCorruption("chain ends early at", Cluster);
                break;
            }
            Status = ExfatReadFatEntry(DeviceExt, Cluster, &Next);
            if (!NT_SUCCESS(Status))
                return Status;
            Status = ExfatWriteFatEntry(DeviceExt, Cluster, EXFAT_CLUSTER_FREE);
            if (!NT_SUCCESS(Status))
                return Status;
            ExfatSetBitmapRange(DeviceExt, Cluster, 1, FALSE);
            Cluster = Next;
        }
    }

    Chain->Count = KeepClusters;
    if (KeepClusters == 0)
    {
        Chain->FirstCluster = 0;
        Chain->NoFatChain = FALSE;
    }
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Move *CurrentCluster to the next cluster of the chain. At the end
 *           of the chain, *CurrentCluster becomes EXFAT_CLUSTER_EOF unless
 *           Extend is set, in which case a cluster is appended.
 *           A chain with no clusters yet gets its first cluster when Extend
 *           is set and FirstCluster is 0.
 */
NTSTATUS
NextCluster(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_CHAIN Chain,
    PULONG CurrentCluster,
    BOOLEAN Extend)
{
    ULONG Next;
    ULONG NewCluster;
    NTSTATUS Status;

    if (Chain->FirstCluster == 0)
    {
        if (!Extend)
        {
            *CurrentCluster = EXFAT_CLUSTER_EOF;
            return STATUS_SUCCESS;
        }

        ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
        Status = ExfatAllocateCluster(DeviceExt, 0, &NewCluster);
        if (NT_SUCCESS(Status))
        {
            Chain->FirstCluster = NewCluster;
            Chain->Count = 1;
            Chain->NoFatChain = FALSE;
            *CurrentCluster = NewCluster;
        }
        ExReleaseResourceLite(&DeviceExt->FatResource);
        return Status;
    }

    if (Chain->NoFatChain)
    {
        if (*CurrentCluster < Chain->FirstCluster ||
            *CurrentCluster - Chain->FirstCluster >= Chain->Count)
        {
            ExfatReportCorruption("walk outside contiguous chain at", *CurrentCluster);
            return STATUS_FILE_CORRUPT_ERROR;
        }
        if (*CurrentCluster - Chain->FirstCluster + 1 < Chain->Count)
        {
            (*CurrentCluster)++;
            return STATUS_SUCCESS;
        }
        if (!Extend)
        {
            *CurrentCluster = EXFAT_CLUSTER_EOF;
            return STATUS_SUCCESS;
        }

        ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
        Next = *CurrentCluster + 1;
        if (ExfatIsDataCluster(DeviceExt, Next) &&
            !RtlCheckBit(&DeviceExt->Bitmap, Next - EXFAT_FIRST_DATA_CLUSTER) &&
            !BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        {
            /* The adjacent cluster is free: the file stays contiguous. */
            ExfatSetBitmapRange(DeviceExt, Next, 1, TRUE);
            Chain->Count++;
            *CurrentCluster = Next;
            ExReleaseResourceLite(&DeviceExt->FatResource);
            return STATUS_SUCCESS;
        }
        Status = ExfatConvertToFatChain(DeviceExt, Chain);
        ExReleaseResourceLite(&DeviceExt->FatResource);
        if (!NT_SUCCESS(Status))
            return Status;
        /* Now continue as a FAT chain. */
    }

    Status = ExfatReadFatEntry(DeviceExt, *CurrentCluster, &Next);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Next != EXFAT_CLUSTER_EOF)
    {
        if (!ExfatIsDataCluster(DeviceExt, Next))
        {
            ExfatReportCorruption("FAT entry", Next);
            return STATUS_FILE_CORRUPT_ERROR;
        }
        *CurrentCluster = Next;
        return STATUS_SUCCESS;
    }

    if (!Extend)
    {
        *CurrentCluster = EXFAT_CLUSTER_EOF;
        return STATUS_SUCCESS;
    }

    ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
    Status = ExfatAllocateCluster(DeviceExt, *CurrentCluster + 1, &NewCluster);
    if (NT_SUCCESS(Status))
    {
        Status = ExfatWriteFatEntry(DeviceExt, *CurrentCluster, NewCluster);
        if (NT_SUCCESS(Status))
        {
            Chain->Count++;
            *CurrentCluster = NewCluster;
        }
    }
    ExReleaseResourceLite(&DeviceExt->FatResource);
    return Status;
}

/*
 * FUNCTION: Find the cluster holding byte Offset, counted from StartCluster
 *           (which must be a cluster of Chain), optionally extending the chain.
 */
NTSTATUS
OffsetToCluster(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_CHAIN Chain,
    ULONG StartCluster,
    ULONGLONG Offset,
    PULONG Cluster,
    BOOLEAN Extend)
{
    ULONGLONG Steps = Offset / DeviceExt->FatInfo.BytesPerCluster;
    ULONG CurrentCluster = StartCluster;
    NTSTATUS Status;

    if (StartCluster == 0)
    {
        DPRINT1("OffsetToCluster called with no start cluster\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* A contiguous chain needs no walk while the offset stays inside it. */
    if (Chain->NoFatChain &&
        StartCluster >= Chain->FirstCluster &&
        (ULONGLONG)(StartCluster - Chain->FirstCluster) + Steps < Chain->Count)
    {
        *Cluster = StartCluster + (ULONG)Steps;
        return STATUS_SUCCESS;
    }

    while (Steps-- > 0)
    {
        Status = NextCluster(DeviceExt, Chain, &CurrentCluster, Extend);
        if (!NT_SUCCESS(Status))
            return Status;
        if (CurrentCluster == EXFAT_CLUSTER_EOF)
            return STATUS_END_OF_FILE;
    }
    *Cluster = CurrentCluster;
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Converts the cluster number to a sector number for this physical
 *           device
 */
ULONGLONG
ClusterToSector(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster)
{
    return DeviceExt->FatInfo.dataStart +
           ((ULONGLONG)(Cluster - EXFAT_FIRST_DATA_CLUSTER) * DeviceExt->FatInfo.SectorsPerCluster);
}

NTSTATUS
CountAvailableClusters(
    PDEVICE_EXTENSION DeviceExt,
    PLARGE_INTEGER Clusters)
{
    ExAcquireResourceSharedLite(&DeviceExt->FatResource, TRUE);
    if (!DeviceExt->AvailableClustersValid)
    {
        DeviceExt->AvailableClusters = RtlNumberOfClearBits(&DeviceExt->Bitmap);
        DeviceExt->AvailableClustersValid = TRUE;
    }
    if (Clusters)
        Clusters->QuadPart = DeviceExt->AvailableClusters;
    ExReleaseResourceLite(&DeviceExt->FatResource);
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Read or change the VolumeDirty bit of the main boot sector.
 *           VolumeFlags is excluded from the boot region checksum, and the
 *           backup boot sector is never modified.
 */
static
NTSTATUS
ExfatUpdateVolumeFlags(
    PDEVICE_EXTENSION DeviceExt,
    USHORT Set,
    USHORT Clear,
    PUSHORT Flags)
{
    PEXFAT_BOOT_SECTOR Boot;
    LARGE_INTEGER Offset;
    NTSTATUS Status;

    Boot = ExAllocatePoolWithTag(NonPagedPool, DeviceExt->FatInfo.BytesPerSector, TAG_BUFFER);
    if (!Boot)
        return STATUS_INSUFFICIENT_RESOURCES;

    Offset.QuadPart = 0;
    Status = VfatReadDisk(DeviceExt->StorageDevice, &Offset, DeviceExt->FatInfo.BytesPerSector,
                          (PUCHAR)Boot, FALSE);
    if (NT_SUCCESS(Status) && (Set || Clear))
    {
        Boot->VolumeFlags = (Boot->VolumeFlags | Set) & ~Clear;
        Status = VfatWriteDisk(DeviceExt->StorageDevice, &Offset, DeviceExt->FatInfo.BytesPerSector,
                               (PUCHAR)Boot, FALSE);
    }
    if (NT_SUCCESS(Status) && Flags)
        *Flags = Boot->VolumeFlags;

    ExFreePoolWithTag(Boot, TAG_BUFFER);
    return Status;
}

NTSTATUS
GetDirtyStatus(
    PDEVICE_EXTENSION DeviceExt,
    PBOOLEAN DirtyStatus)
{
    USHORT Flags;
    NTSTATUS Status = ExfatUpdateVolumeFlags(DeviceExt, 0, 0, &Flags);

    if (NT_SUCCESS(Status))
        *DirtyStatus = BooleanFlagOn(Flags, EXFAT_VOLUME_DIRTY);
    return Status;
}

NTSTATUS
SetDirtyStatus(
    PDEVICE_EXTENSION DeviceExt,
    BOOLEAN DirtyStatus)
{
    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        return STATUS_MEDIA_WRITE_PROTECTED;

    return ExfatUpdateVolumeFlags(DeviceExt,
                                  DirtyStatus ? EXFAT_VOLUME_DIRTY : 0,
                                  DirtyStatus ? 0 : EXFAT_VOLUME_DIRTY,
                                  NULL);
}

/* EOF */
