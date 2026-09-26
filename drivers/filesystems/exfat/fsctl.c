/*
 * PROJECT:     VFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Filesystem routines
 * COPYRIGHT:   Copyright 2002-2013 Eric Kohl <eric.kohl@reactos.org>
 *              Copyright 2008-2018 Pierre Schweitzer <pierre@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "vfat.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

#define  CACHEPAGESIZE(pDeviceExt) ((pDeviceExt)->FatInfo.BytesPerCluster > PAGE_SIZE ? \
                                    (pDeviceExt)->FatInfo.BytesPerCluster : PAGE_SIZE)

/* The boot region checksum skips VolumeFlags and PercentInUse of sector 0. */
static
ULONG
ExfatBootChecksum(
    PUCHAR Sectors,
    ULONG BytesPerSector)
{
    ULONG Checksum = 0;
    ULONG i;

    for (i = 0; i < 11 * BytesPerSector; i++)
    {
        if (i == 106 || i == 107 || i == 112)
            continue;
        Checksum = ((Checksum & 1) ? 0x80000000 : 0) + (Checksum >> 1) + Sectors[i];
    }
    return Checksum;
}

/*
 * Validate an exFAT boot region and describe the volume in FatInfo.
 * *ChecksumValid tells whether the main boot region checksum matched; a
 * volume whose checksum does not match is only mounted read-only.
 */
static
NTSTATUS
VfatHasFileSystem(
    PDEVICE_OBJECT DeviceToMount,
    PBOOLEAN RecognizedFS,
    PFATINFO pFatInfo,
    BOOLEAN Override,
    PBOOLEAN ChecksumValid)
{
    NTSTATUS Status;
    PARTITION_INFORMATION PartitionInfo;
    DISK_GEOMETRY DiskGeometry;
    FATINFO FatInfo;
    ULONG Size;
    ULONG i;
    ULONG BytesPerSector;
    ULONG Checksum;
    LARGE_INTEGER Offset;
    PUCHAR Region;
    PEXFAT_BOOT_SECTOR Boot;
    BOOLEAN PartitionInfoIsValid = FALSE;

    DPRINT("VfatHasFileSystem\n");

    *RecognizedFS = FALSE;
    if (ChecksumValid)
        *ChecksumValid = FALSE;

    Size = sizeof(DISK_GEOMETRY);
    Status = VfatBlockDeviceIoControl(DeviceToMount,
                                      IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                      NULL,
                                      0,
                                      &DiskGeometry,
                                      &Size,
                                      Override);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("VfatBlockDeviceIoControl failed (%x)\n", Status);
        return Status;
    }

    RtlZeroMemory(&FatInfo, sizeof(FatInfo));
    FatInfo.FixedMedia = DiskGeometry.MediaType == FixedMedia ? TRUE : FALSE;
    if (DiskGeometry.MediaType == FixedMedia || DiskGeometry.MediaType == RemovableMedia)
    {
        Size = sizeof(PARTITION_INFORMATION);
        Status = VfatBlockDeviceIoControl(DeviceToMount,
                                          IOCTL_DISK_GET_PARTITION_INFO,
                                          NULL,
                                          0,
                                          &PartitionInfo,
                                          &Size,
                                          Override);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("VfatBlockDeviceIoControl failed (%x)\n", Status);
            return Status;
        }
        PartitionInfoIsValid = TRUE;
    }

    BytesPerSector = DiskGeometry.BytesPerSector;
    if (BytesPerSector < 512 || BytesPerSector > 4096 || (BytesPerSector & (BytesPerSector - 1)))
        return STATUS_SUCCESS;

    /* Main boot region: 11 sectors, then the checksum sector. */
    Region = ExAllocatePoolWithTag(NonPagedPool, 12 * BytesPerSector, TAG_BUFFER);
    if (Region == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Offset.QuadPart = 0;
    Status = VfatReadDisk(DeviceToMount, &Offset, 12 * BytesPerSector, Region, Override);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Region, TAG_BUFFER);
        return Status;
    }

    Boot = (PEXFAT_BOOT_SECTOR)Region;
    *RecognizedFS = TRUE;

    if (RtlCompareMemory(Boot->FileSystemName, "EXFAT   ", 8) != 8 ||
        Boot->BootSignature != 0xAA55)
    {
        *RecognizedFS = FALSE;
    }

    for (i = 0; *RecognizedFS && i < sizeof(Boot->MustBeZero); i++)
    {
        if (Boot->MustBeZero[i] != 0)
            *RecognizedFS = FALSE;
    }

    if (*RecognizedFS &&
        (Boot->BytesPerSectorShift < 9 || Boot->BytesPerSectorShift > 12 ||
         (1UL << Boot->BytesPerSectorShift) != BytesPerSector ||
         Boot->SectorsPerClusterShift > 25 - Boot->BytesPerSectorShift ||
         (Boot->NumberOfFats != 1 && Boot->NumberOfFats != 2) ||
         (Boot->FileSystemRevision >> 8) != 1))
    {
        DPRINT1("exFAT: unsupported geometry or revision (sector shift %u, cluster shift %u, FATs %u, revision %x)\n",
                Boot->BytesPerSectorShift, Boot->SectorsPerClusterShift,
                Boot->NumberOfFats, Boot->FileSystemRevision);
        *RecognizedFS = FALSE;
    }

    if (*RecognizedFS &&
        (Boot->FatOffset < 24 ||
         Boot->FatLength < ((ULONGLONG)Boot->ClusterCount + 2) * 4 / BytesPerSector ||
         Boot->ClusterHeapOffset < Boot->FatOffset + (ULONGLONG)Boot->FatLength * Boot->NumberOfFats ||
         Boot->ClusterCount == 0 ||
         Boot->ClusterCount > (Boot->VolumeLength - Boot->ClusterHeapOffset) >> Boot->SectorsPerClusterShift ||
         Boot->FirstClusterOfRootDirectory < EXFAT_FIRST_DATA_CLUSTER ||
         Boot->FirstClusterOfRootDirectory - EXFAT_FIRST_DATA_CLUSTER >= Boot->ClusterCount))
    {
        DPRINT1("exFAT: inconsistent boot sector layout\n");
        *RecognizedFS = FALSE;
    }

    if (*RecognizedFS && PartitionInfoIsValid &&
        Boot->VolumeLength > (ULONGLONG)PartitionInfo.PartitionLength.QuadPart / BytesPerSector)
    {
        DPRINT1("exFAT: volume is larger than its partition\n");
        *RecognizedFS = FALSE;
    }

    if (*RecognizedFS)
    {
        Checksum = ExfatBootChecksum(Region, BytesPerSector);
        if (ChecksumValid)
        {
            *ChecksumValid = TRUE;
            for (i = 0; i < BytesPerSector / sizeof(ULONG); i++)
            {
                if (((PULONG)(Region + 11 * BytesPerSector))[i] != Checksum)
                {
                    DPRINT1("exFAT: boot region checksum mismatch\n");
                    *ChecksumValid = FALSE;
                    break;
                }
            }
        }

        FatInfo.VolumeID = Boot->VolumeSerialNumber;
        FatInfo.BytesPerSector = BytesPerSector;
        FatInfo.SectorShift = Boot->BytesPerSectorShift;
        FatInfo.ClusterShift = Boot->SectorsPerClusterShift;
        FatInfo.SectorsPerCluster = 1UL << Boot->SectorsPerClusterShift;
        FatInfo.BytesPerCluster = BytesPerSector << Boot->SectorsPerClusterShift;
        /* Only the active FAT is used; writes are refused with two FATs. */
        FatInfo.FATCount = Boot->NumberOfFats;
        FatInfo.FATSectors = Boot->FatLength;
        FatInfo.FATStart = Boot->FatOffset +
            ((Boot->NumberOfFats == 2 && (Boot->VolumeFlags & EXFAT_VOLUME_ACTIVE_FAT)) ? Boot->FatLength : 0);
        FatInfo.dataStart = Boot->ClusterHeapOffset;
        FatInfo.NumberOfClusters = Boot->ClusterCount;
        FatInfo.RootCluster = Boot->FirstClusterOfRootDirectory;
        FatInfo.Sectors = Boot->VolumeLength;
        FatInfo.VolumeFlags = Boot->VolumeFlags;
        FatInfo.Revision = Boot->FileSystemRevision;

        if (pFatInfo)
            *pFatInfo = FatInfo;
    }

    ExFreePoolWithTag(Region, TAG_BUFFER);
    DPRINT("VfatHasFileSystem done\n");
    return STATUS_SUCCESS;
}

static
ULONG
ExfatUpcaseChecksum(
    PUCHAR Table,
    ULONG Length)
{
    ULONG Checksum = 0;
    ULONG i;

    for (i = 0; i < Length; i++)
        Checksum = ((Checksum & 1) ? 0x80000000 : 0) + (Checksum >> 1) + Table[i];
    return Checksum;
}

/* Read the whole of a FAT-chained system file (bitmap or up-case table). */
static
NTSTATUS
ExfatReadChain(
    PDEVICE_EXTENSION DeviceExt,
    ULONG FirstCluster,
    ULONGLONG Length,
    PUCHAR *Buffer)
{
    ULONG BytesPerCluster = DeviceExt->FatInfo.BytesPerCluster;
    ULONG Clusters = (ULONG)((Length + BytesPerCluster - 1) / BytesPerCluster);
    ULONG Cluster = FirstCluster;
    ULONG i;
    LARGE_INTEGER Offset;
    NTSTATUS Status = STATUS_SUCCESS;

    *Buffer = ExAllocatePoolWithTag(PagedPool, Clusters * BytesPerCluster, TAG_BUFFER);
    if (!*Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; i < Clusters; i++)
    {
        if (Cluster < EXFAT_FIRST_DATA_CLUSTER ||
            Cluster - EXFAT_FIRST_DATA_CLUSTER >= DeviceExt->FatInfo.NumberOfClusters)
        {
            Status = STATUS_DISK_CORRUPT_ERROR;
            break;
        }
        Offset.QuadPart = ClusterToSector(DeviceExt, Cluster) * DeviceExt->FatInfo.BytesPerSector;
        Status = VfatReadDisk(DeviceExt->StorageDevice, &Offset, BytesPerCluster,
                              *Buffer + i * BytesPerCluster, FALSE);
        if (!NT_SUCCESS(Status))
            break;
        if (i + 1 < Clusters)
        {
            Status = ExfatReadFatEntry(DeviceExt, Cluster, &Cluster);
            if (!NT_SUCCESS(Status))
                break;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(*Buffer, TAG_BUFFER);
        *Buffer = NULL;
    }
    return Status;
}

/* Expand the compressed $UpCase table (0xFFFF, n = n identity mappings). */
static
NTSTATUS
ExfatLoadUpcase(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_UPCASE_ENTRY Entry)
{
    PUCHAR Raw;
    PUSHORT Words;
    ULONG WordCount;
    ULONG Index = 0;
    ULONG i;
    NTSTATUS Status;

    if (Entry->DataLength == 0 || Entry->DataLength > 0x20000 || (Entry->DataLength & 1))
        return STATUS_DISK_CORRUPT_ERROR;

    Status = ExfatReadChain(DeviceExt, Entry->FirstCluster, Entry->DataLength, &Raw);
    if (!NT_SUCCESS(Status))
        return Status;

    if (ExfatUpcaseChecksum(Raw, (ULONG)Entry->DataLength) != Entry->TableChecksum)
    {
        DPRINT1("exFAT: up-case table checksum mismatch\n");
        ExFreePoolWithTag(Raw, TAG_BUFFER);
        return STATUS_DISK_CORRUPT_ERROR;
    }

    DeviceExt->UpcaseTable = ExAllocatePoolWithTag(PagedPool, 0x10000 * sizeof(USHORT), TAG_BUFFER);
    if (!DeviceExt->UpcaseTable)
    {
        ExFreePoolWithTag(Raw, TAG_BUFFER);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (i = 0; i < 0x10000; i++)
        DeviceExt->UpcaseTable[i] = (USHORT)i;

    Words = (PUSHORT)Raw;
    WordCount = (ULONG)Entry->DataLength / sizeof(USHORT);
    for (i = 0; i < WordCount && Index < 0x10000; i++)
    {
        if (Words[i] == 0xFFFF && i + 1 < WordCount)
        {
            Index += Words[++i];
        }
        else
        {
            DeviceExt->UpcaseTable[Index++] = Words[i];
        }
    }

    ExFreePoolWithTag(Raw, TAG_BUFFER);
    return STATUS_SUCCESS;
}

/*
 * Scan the root directory for the allocation bitmap, up-case table and
 * volume label entries, and load the first two. Called once the FAT stream
 * is set up.
 */
static
NTSTATUS
ExfatReadRootSystemEntries(
    PDEVICE_EXTENSION DeviceExt,
    PBOOLEAN UpcaseValid)
{
    ULONG BytesPerCluster = DeviceExt->FatInfo.BytesPerCluster;
    ULONG Cluster = DeviceExt->FatInfo.RootCluster;
    ULONG Visited = 0;
    ULONG i;
    PUCHAR Buffer;
    PEXFAT_GENERIC_ENTRY Entry;
    EXFAT_UPCASE_ENTRY Upcase;
    LARGE_INTEGER Offset;
    BOOLEAN FoundBitmap = FALSE;
    BOOLEAN FoundUpcase = FALSE;
    BOOLEAN Done = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    *UpcaseValid = FALSE;
    Buffer = ExAllocatePoolWithTag(NonPagedPool, BytesPerCluster, TAG_BUFFER);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    while (!Done)
    {
        if (Cluster < EXFAT_FIRST_DATA_CLUSTER ||
            Cluster - EXFAT_FIRST_DATA_CLUSTER >= DeviceExt->FatInfo.NumberOfClusters ||
            ++Visited > DeviceExt->FatInfo.NumberOfClusters)
        {
            Status = STATUS_DISK_CORRUPT_ERROR;
            break;
        }

        Offset.QuadPart = ClusterToSector(DeviceExt, Cluster) * DeviceExt->FatInfo.BytesPerSector;
        Status = VfatReadDisk(DeviceExt->StorageDevice, &Offset, BytesPerCluster, Buffer, FALSE);
        if (!NT_SUCCESS(Status))
            break;

        for (i = 0; i < BytesPerCluster && !Done; i += EXFAT_ENTRY_SIZE)
        {
            Entry = (PEXFAT_GENERIC_ENTRY)(Buffer + i);
            switch (Entry->EntryType)
            {
                case EXFAT_TYPE_END:
                    Done = TRUE;
                    break;

                case EXFAT_TYPE_BITMAP:
                    /* The first bitmap belongs to the first FAT, the one used here. */
                    if (!FoundBitmap && (((PEXFAT_BITMAP_ENTRY)Entry)->BitmapFlags & 1) == 0)
                    {
                        DeviceExt->BitmapChain.FirstCluster = Entry->FirstCluster;
                        DeviceExt->BitmapChain.NoFatChain = FALSE;
                        DeviceExt->BitmapBytes = (ULONG)Entry->DataLength;
                        FoundBitmap = TRUE;
                    }
                    break;

                case EXFAT_TYPE_UPCASE:
                    if (!FoundUpcase)
                    {
                        RtlCopyMemory(&Upcase, Entry, sizeof(Upcase));
                        FoundUpcase = TRUE;
                    }
                    break;

                case EXFAT_TYPE_LABEL:
                {
                    PEXFAT_LABEL_ENTRY Label = (PEXFAT_LABEL_ENTRY)Entry;
                    if (Label->CharacterCount <= 11)
                    {
                        RtlCopyMemory(DeviceExt->VolumeLabel, Label->VolumeLabel,
                                      Label->CharacterCount * sizeof(WCHAR));
                        DeviceExt->VolumeLabelLength = Label->CharacterCount * sizeof(WCHAR);
                    }
                    break;
                }
            }
        }

        if (!Done)
        {
            Status = ExfatReadFatEntry(DeviceExt, Cluster, &Cluster);
            if (!NT_SUCCESS(Status))
                break;
            if (Cluster == EXFAT_CLUSTER_EOF)
                Done = TRUE;
        }
    }
    ExFreePoolWithTag(Buffer, TAG_BUFFER);

    if (NT_SUCCESS(Status) && !FoundBitmap)
    {
        DPRINT1("exFAT: the root directory has no allocation bitmap\n");
        Status = STATUS_DISK_CORRUPT_ERROR;
    }
    if (NT_SUCCESS(Status))
        Status = ExfatLoadBitmap(DeviceExt);
    if (NT_SUCCESS(Status) && FoundUpcase)
    {
        /* Without a valid up-case table names still resolve through the
           system table, but new names could not be hashed as Windows does. */
        *UpcaseValid = NT_SUCCESS(ExfatLoadUpcase(DeviceExt, &Upcase));
    }
    return Status;
}

/*
 * FUNCTION: Mount the filesystem
 */
static
NTSTATUS
VfatMount(
    PVFAT_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT DeviceObject = NULL;
    PDEVICE_EXTENSION DeviceExt = NULL;
    BOOLEAN RecognizedFS;
    BOOLEAN ChecksumValid;
    BOOLEAN UpcaseValid = FALSE;
    NTSTATUS Status;
    PVFATFCB Fcb = NULL;
    PVFATFCB VolumeFcb = NULL;
    PDEVICE_OBJECT DeviceToMount;
    PVPB Vpb;
    UNICODE_STRING NameU = RTL_CONSTANT_STRING(L"\\$$Fat$$");
    UNICODE_STRING VolumeNameU = RTL_CONSTANT_STRING(L"\\$$Volume$$");
    ULONG HashTableSize;
    ULONG i;
    FATINFO FatInfo;
    BOOLEAN Dirty;

    DPRINT("VfatMount(IrpContext %p)\n", IrpContext);

    ASSERT(IrpContext);

    if (IrpContext->DeviceObject != VfatGlobalData->DeviceObject)
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto ByeBye;
    }

    DeviceToMount = IrpContext->Stack->Parameters.MountVolume.DeviceObject;
    Vpb = IrpContext->Stack->Parameters.MountVolume.Vpb;

    Status = VfatHasFileSystem(DeviceToMount, &RecognizedFS, &FatInfo, FALSE, &ChecksumValid);
    if (!NT_SUCCESS(Status))
    {
        goto ByeBye;
    }

    if (RecognizedFS == FALSE)
    {
        DPRINT("exFAT: Unrecognized Volume\n");
        Status = STATUS_UNRECOGNIZED_VOLUME;
        goto ByeBye;
    }

    HashTableSize = 65537; // 65536 = 64 * 1024;
    DPRINT("exFAT: Recognized volume\n");
    Status = IoCreateDevice(VfatGlobalData->DriverObject,
                            ROUND_UP(sizeof (DEVICE_EXTENSION), sizeof(ULONG)) + sizeof(HASHENTRY*) * HashTableSize,
                            NULL,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            DeviceToMount->Characteristics,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        goto ByeBye;
    }

    DeviceExt = DeviceObject->DeviceExtension;
    RtlZeroMemory(DeviceExt, ROUND_UP(sizeof(DEVICE_EXTENSION), sizeof(ULONG)) + sizeof(HASHENTRY*) * HashTableSize);
    DeviceExt->FcbHashTable = (HASHENTRY**)((ULONG_PTR)DeviceExt + ROUND_UP(sizeof(DEVICE_EXTENSION), sizeof(ULONG)));
    DeviceExt->HashTableSize = HashTableSize;
    DeviceExt->VolumeDevice = DeviceObject;

    KeInitializeSpinLock(&DeviceExt->OverflowQueueSpinLock);
    InitializeListHead(&DeviceExt->OverflowQueue);
    DeviceExt->OverflowQueueCount = 0;
    DeviceExt->PostedRequestCount = 0;

    /* use same vpb as device disk */
    DeviceObject->Vpb = Vpb;
    DeviceToMount->Vpb = Vpb;

    RtlCopyMemory(&DeviceExt->FatInfo, &FatInfo, sizeof(FATINFO));

    DPRINT("BytesPerSector:     %u\n", DeviceExt->FatInfo.BytesPerSector);
    DPRINT("SectorsPerCluster:  %u\n", DeviceExt->FatInfo.SectorsPerCluster);
    DPRINT("FATStart:           %u\n", DeviceExt->FatInfo.FATStart);
    DPRINT("FATSectors:         %u\n", DeviceExt->FatInfo.FATSectors);
    DPRINT("DataStart:          %u\n", DeviceExt->FatInfo.dataStart);
    DPRINT("RootCluster:        %u\n", DeviceExt->FatInfo.RootCluster);

    /*
     * Decide whether this volume may be written. Everything below must hold;
     * otherwise the volume is mounted read-only.
     */
    if (!BooleanFlagOn(VfatGlobalData->Flags, VFAT_ENABLE_WRITE_SUPPORT))
    {
        DPRINT1("exFAT: write support is not enabled; mounting read-only\n");
        DeviceExt->Flags |= VCB_WRITE_PROTECTED;
    }
    else if (FatInfo.FATCount != 1)
    {
        DPRINT1("exFAT: volumes with two FATs (TexFAT) are mounted read-only\n");
        DeviceExt->Flags |= VCB_WRITE_PROTECTED;
    }
    else if (!ChecksumValid)
    {
        DPRINT1("exFAT: boot region checksum mismatch; mounting read-only\n");
        DeviceExt->Flags |= VCB_WRITE_PROTECTED;
    }
    else if (FatInfo.VolumeFlags & (EXFAT_VOLUME_DIRTY | EXFAT_VOLUME_MEDIA_FAILURE))
    {
        DPRINT1("exFAT: volume is dirty or reports media failure; mounting read-only until it is checked\n");
        DeviceExt->Flags |= VCB_WRITE_PROTECTED;
    }
    else if (BooleanFlagOn(DeviceToMount->Characteristics, FILE_READ_ONLY_DEVICE))
    {
        DeviceExt->Flags |= VCB_WRITE_PROTECTED;
    }

    DeviceExt->StorageDevice = DeviceToMount;
    DeviceExt->StorageDevice->Vpb->DeviceObject = DeviceObject;
    DeviceExt->StorageDevice->Vpb->RealDevice = DeviceExt->StorageDevice;
    DeviceExt->StorageDevice->Vpb->Flags |= VPB_MOUNTED;
    DeviceObject->StackSize = DeviceExt->StorageDevice->StackSize + 1;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DPRINT("FsDeviceObject %p\n", DeviceObject);

    /* Initialize this resource early ... it's used in VfatCleanup */
    ExInitializeResourceLite(&DeviceExt->DirResource);
    ExInitializeResourceLite(&DeviceExt->FatResource);

    DeviceExt->IoVPB = DeviceObject->Vpb;
    DeviceExt->SpareVPB = ExAllocatePoolWithTag(NonPagedPool, sizeof(VPB), TAG_VPB);
    if (DeviceExt->SpareVPB == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ByeBye;
    }

    DeviceExt->Statistics = ExAllocatePoolWithTag(NonPagedPool,
                                                  sizeof(STATISTICS) * VfatGlobalData->NumberProcessors,
                                                  TAG_STATS);
    if (DeviceExt->Statistics == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ByeBye;
    }

    RtlZeroMemory(DeviceExt->Statistics, sizeof(STATISTICS) * VfatGlobalData->NumberProcessors);
    for (i = 0; i < VfatGlobalData->NumberProcessors; ++i)
    {
        DeviceExt->Statistics[i].Base.FileSystemType = FILESYSTEM_STATISTICS_TYPE_FAT;
        DeviceExt->Statistics[i].Base.Version = 1;
        DeviceExt->Statistics[i].Base.SizeOfCompleteStructure = sizeof(STATISTICS);
    }

    DeviceExt->FATFileObject = IoCreateStreamFileObject(NULL, DeviceExt->StorageDevice);
    Fcb = vfatNewFCB(DeviceExt, &NameU);
    if (Fcb == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ByeBye;
    }

    Status = vfatAttachFCBToFileObject(DeviceExt, Fcb, DeviceExt->FATFileObject);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    DeviceExt->FATFileObject->PrivateCacheMap = NULL;
    Fcb->FileObject = DeviceExt->FATFileObject;

    Fcb->Flags = FCB_IS_FAT;
    Fcb->RFCB.FileSize.QuadPart = (LONGLONG)DeviceExt->FatInfo.FATSectors * DeviceExt->FatInfo.BytesPerSector;
    Fcb->RFCB.ValidDataLength = Fcb->RFCB.FileSize;
    Fcb->RFCB.AllocationSize = Fcb->RFCB.FileSize;

    _SEH2_TRY
    {
        CcInitializeCacheMap(DeviceExt->FATFileObject,
                             (PCC_FILE_SIZES)(&Fcb->RFCB.AllocationSize),
                             TRUE,
                             &VfatGlobalData->CacheMgrCallbacks,
                             Fcb);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        goto ByeBye;
    }
    _SEH2_END;

    /* Bitmap, up-case table and label live in the root directory. */
    Status = ExfatReadRootSystemEntries(DeviceExt, &UpcaseValid);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("exFAT: cannot read the root directory system entries (0x%08lx)\n", Status);
        goto ByeBye;
    }
    if (!UpcaseValid && !BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
    {
        DPRINT1("exFAT: no valid up-case table; mounting read-only\n");
        DeviceExt->Flags |= VCB_WRITE_PROTECTED;
    }

    DeviceExt->LastAvailableCluster = EXFAT_FIRST_DATA_CLUSTER;
    CountAvailableClusters(DeviceExt, NULL);

    InitializeListHead(&DeviceExt->FcbListHead);

    VolumeFcb = vfatNewFCB(DeviceExt, &VolumeNameU);
    if (VolumeFcb == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ByeBye;
    }

    VolumeFcb->Flags = FCB_IS_VOLUME;
    VolumeFcb->RFCB.FileSize.QuadPart = (LONGLONG)DeviceExt->FatInfo.Sectors * DeviceExt->FatInfo.BytesPerSector;
    VolumeFcb->RFCB.ValidDataLength = VolumeFcb->RFCB.FileSize;
    VolumeFcb->RFCB.AllocationSize = VolumeFcb->RFCB.FileSize;
    DeviceExt->VolumeFcb = VolumeFcb;

    ExAcquireResourceExclusiveLite(&VfatGlobalData->VolumeListLock, TRUE);
    InsertHeadList(&VfatGlobalData->VolumeListHead, &DeviceExt->VolumeListEntry);
    ExReleaseResourceLite(&VfatGlobalData->VolumeListLock);

    /* serial number and label */
    DeviceObject->Vpb->SerialNumber = DeviceExt->FatInfo.VolumeID;
    RtlCopyMemory(DeviceObject->Vpb->VolumeLabel, DeviceExt->VolumeLabel, DeviceExt->VolumeLabelLength);
    Vpb->VolumeLabelLength = DeviceExt->VolumeLabelLength;

    /*
     * A writable volume is marked dirty for as long as it is mounted, and
     * cleaned when it is flushed and dismounted. A crash leaves it dirty, so
     * Windows checks it before using it.
     */
    if (!BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
    {
        Status = GetDirtyStatus(DeviceExt, &Dirty);
        if (NT_SUCCESS(Status) && !Dirty)
        {
            if (NT_SUCCESS(SetDirtyStatus(DeviceExt, TRUE)))
            {
                VolumeFcb->Flags |= VCB_CLEAR_DIRTY;
                VolumeFcb->Flags |= VCB_IS_DIRTY;
            }
            else
            {
                DeviceExt->Flags |= VCB_WRITE_PROTECTED;
            }
        }
        else
        {
            DeviceExt->Flags |= VCB_WRITE_PROTECTED;
        }
    }
    if (FatInfo.VolumeFlags & EXFAT_VOLUME_DIRTY)
        VolumeFcb->Flags |= VCB_IS_DIRTY;

    if (BooleanFlagOn(Vpb->RealDevice->Flags, DO_SYSTEM_BOOT_PARTITION))
    {
        SetFlag(DeviceExt->Flags, VCB_IS_SYS_OR_HAS_PAGE);
    }

    /* Initialize the notify list and synchronization object */
    InitializeListHead(&DeviceExt->NotifyList);
    FsRtlNotifyInitializeSync(&DeviceExt->NotifySync);

    /* The VCB is OK for usage */
    SetFlag(DeviceExt->Flags, VCB_GOOD);

    /* Send the mount notification */
    FsRtlNotifyVolumeEvent(DeviceExt->FATFileObject, FSRTL_VOLUME_MOUNT);

    DPRINT1("exFAT: mounted %lu clusters of %lu bytes%s\n",
            DeviceExt->FatInfo.NumberOfClusters, DeviceExt->FatInfo.BytesPerCluster,
            BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED) ? " (read-only)" : "");

    Status = STATUS_SUCCESS;

ByeBye:
    if (!NT_SUCCESS(Status))
    {
        /* Cleanup */
        if (DeviceExt && DeviceExt->FATFileObject)
        {
            LARGE_INTEGER Zero = {{0,0}};
            PVFATCCB Ccb = (PVFATCCB)DeviceExt->FATFileObject->FsContext2;

            CcUninitializeCacheMap(DeviceExt->FATFileObject,
                                   &Zero,
                                   NULL);
            ObDereferenceObject(DeviceExt->FATFileObject);
            if (Ccb)
                vfatDestroyCCB(Ccb);
            DeviceExt->FATFileObject = NULL;
        }
        if (Fcb)
            vfatDestroyFCB(Fcb);
        if (DeviceExt)
        {
            ExfatFreeBitmap(DeviceExt);
            if (DeviceExt->UpcaseTable)
                ExFreePoolWithTag(DeviceExt->UpcaseTable, TAG_BUFFER);
        }
        if (DeviceExt && DeviceExt->SpareVPB)
            ExFreePoolWithTag(DeviceExt->SpareVPB, TAG_VPB);
        if (DeviceExt && DeviceExt->Statistics)
            ExFreePoolWithTag(DeviceExt->Statistics, TAG_STATS);
        if (DeviceObject)
            IoDeleteDevice(DeviceObject);
    }

    return Status;
}


/*
 * FUNCTION: Verify the filesystem
 */
static
NTSTATUS
VfatVerify(
    PVFAT_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT DeviceToVerify;
    NTSTATUS Status;
    FATINFO FatInfo;
    BOOLEAN RecognizedFS;
    PDEVICE_EXTENSION DeviceExt;
    BOOLEAN AllowRaw;
    PVPB Vpb;
    ULONG ChangeCount, BufSize = sizeof(ChangeCount);

    DPRINT("VfatVerify(IrpContext %p)\n", IrpContext);

    DeviceToVerify = IrpContext->Stack->Parameters.VerifyVolume.DeviceObject;
    DeviceExt = DeviceToVerify->DeviceExtension;
    Vpb = IrpContext->Stack->Parameters.VerifyVolume.Vpb;
    AllowRaw = BooleanFlagOn(IrpContext->Stack->Flags, SL_ALLOW_RAW_MOUNT);

    if (!BooleanFlagOn(Vpb->RealDevice->Flags, DO_VERIFY_VOLUME))
    {
        DPRINT("Already verified\n");
        return STATUS_SUCCESS;
    }

    Status = VfatBlockDeviceIoControl(DeviceExt->StorageDevice,
                                      IOCTL_DISK_CHECK_VERIFY,
                                      NULL,
                                      0,
                                      &ChangeCount,
                                      &BufSize,
                                      TRUE);
    if (!NT_SUCCESS(Status) && Status != STATUS_VERIFY_REQUIRED)
    {
        DPRINT("VfatBlockDeviceIoControl() failed (Status %lx)\n", Status);
        Status = (AllowRaw ? STATUS_WRONG_VOLUME : Status);
    }
    else
    {
        Status = VfatHasFileSystem(DeviceExt->StorageDevice, &RecognizedFS, &FatInfo, TRUE, NULL);
        if (!NT_SUCCESS(Status) || RecognizedFS == FALSE)
        {
            if (NT_SUCCESS(Status) || AllowRaw)
            {
                Status = STATUS_WRONG_VOLUME;
            }
        }
        else
        {
            /* The same volume has the same layout and serial number. Its
               flags differ once this driver has marked it dirty. */
            FatInfo.VolumeFlags = DeviceExt->FatInfo.VolumeFlags;
            if (sizeof(FATINFO) != RtlCompareMemory(&FatInfo, &DeviceExt->FatInfo, sizeof(FATINFO)))
            {
                Status = STATUS_WRONG_VOLUME;
            }
            else
            {
                DPRINT("Same volume\n");
            }
        }
    }

    Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;

    return Status;
}


static
NTSTATUS
VfatGetVolumeBitmap(
    PVFAT_IRP_CONTEXT IrpContext)
{
    DPRINT("VfatGetVolumeBitmap (IrpContext %p)\n", IrpContext);
    return STATUS_INVALID_DEVICE_REQUEST;
}


static
NTSTATUS
VfatGetRetrievalPointers(
    PVFAT_IRP_CONTEXT IrpContext)
{
    PIO_STACK_LOCATION Stack;
    LARGE_INTEGER Vcn;
    PRETRIEVAL_POINTERS_BUFFER RetrievalPointers;
    PFILE_OBJECT FileObject;
    ULONG MaxExtentCount;
    PVFATFCB Fcb;
    PDEVICE_EXTENSION DeviceExt;
    ULONG FirstCluster;
    ULONG CurrentCluster;
    ULONG LastCluster;
    NTSTATUS Status;

    DPRINT("VfatGetRetrievalPointers(IrpContext %p)\n", IrpContext);

    DeviceExt = IrpContext->DeviceExt;
    FileObject = IrpContext->FileObject;
    Stack = IrpContext->Stack;
    if (Stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(STARTING_VCN_INPUT_BUFFER) ||
        Stack->Parameters.DeviceIoControl.Type3InputBuffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (IrpContext->Irp->UserBuffer == NULL ||
        Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(RETRIEVAL_POINTERS_BUFFER))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    Fcb = FileObject->FsContext;

    ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);

    Vcn = ((PSTARTING_VCN_INPUT_BUFFER)Stack->Parameters.DeviceIoControl.Type3InputBuffer)->StartingVcn;
    RetrievalPointers = IrpContext->Irp->UserBuffer;

    MaxExtentCount = ((Stack->Parameters.DeviceIoControl.OutputBufferLength - sizeof(RetrievalPointers->ExtentCount) - sizeof(RetrievalPointers->StartingVcn)) / sizeof(RetrievalPointers->Extents[0]));

    if (Vcn.QuadPart >= Fcb->RFCB.AllocationSize.QuadPart / DeviceExt->FatInfo.BytesPerCluster)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto ByeBye;
    }

    FirstCluster = Fcb->Chain.FirstCluster;
    Status = OffsetToCluster(DeviceExt, &Fcb->Chain, FirstCluster,
                             (ULONGLONG)Vcn.QuadPart * DeviceExt->FatInfo.BytesPerCluster,
                             &CurrentCluster, FALSE);
    if (!NT_SUCCESS(Status))
    {
        goto ByeBye;
    }

    RetrievalPointers->StartingVcn = Vcn;
    RetrievalPointers->ExtentCount = 0;
    RetrievalPointers->Extents[0].Lcn.u.HighPart = 0;
    RetrievalPointers->Extents[0].Lcn.u.LowPart = CurrentCluster - 2;
    LastCluster = 0;
    while (CurrentCluster != 0xffffffff && RetrievalPointers->ExtentCount < MaxExtentCount)
    {
        LastCluster = CurrentCluster;
        Status = NextCluster(DeviceExt, &Fcb->Chain, &CurrentCluster, FALSE);
        Vcn.QuadPart++;
        if (!NT_SUCCESS(Status))
        {
            goto ByeBye;
        }

        if (LastCluster + 1 != CurrentCluster)
        {
            RetrievalPointers->Extents[RetrievalPointers->ExtentCount].NextVcn = Vcn;
            RetrievalPointers->ExtentCount++;
            if (RetrievalPointers->ExtentCount < MaxExtentCount)
            {
                RetrievalPointers->Extents[RetrievalPointers->ExtentCount].Lcn.u.HighPart = 0;
                RetrievalPointers->Extents[RetrievalPointers->ExtentCount].Lcn.u.LowPart = CurrentCluster - 2;
            }
        }
    }

    IrpContext->Irp->IoStatus.Information = sizeof(RETRIEVAL_POINTERS_BUFFER) + (sizeof(RetrievalPointers->Extents[0]) * (RetrievalPointers->ExtentCount - 1));
    Status = STATUS_SUCCESS;

ByeBye:
    ExReleaseResourceLite(&Fcb->MainResource);

    return Status;
}

static
NTSTATUS
VfatMoveFile(
    PVFAT_IRP_CONTEXT IrpContext)
{
    DPRINT("VfatMoveFile(IrpContext %p)\n", IrpContext);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static
NTSTATUS
VfatIsVolumeDirty(
    PVFAT_IRP_CONTEXT IrpContext)
{
    PULONG Flags;

    DPRINT("VfatIsVolumeDirty(IrpContext %p)\n", IrpContext);

    if (IrpContext->Stack->Parameters.FileSystemControl.OutputBufferLength != sizeof(ULONG))
        return STATUS_INVALID_BUFFER_SIZE;
    else if (!IrpContext->Irp->AssociatedIrp.SystemBuffer)
        return STATUS_INVALID_USER_BUFFER;

    Flags = (PULONG)IrpContext->Irp->AssociatedIrp.SystemBuffer;
    *Flags = 0;

    if (BooleanFlagOn(IrpContext->DeviceExt->VolumeFcb->Flags, VCB_IS_DIRTY) &&
        !BooleanFlagOn(IrpContext->DeviceExt->VolumeFcb->Flags, VCB_CLEAR_DIRTY))
    {
        *Flags |= VOLUME_IS_DIRTY;
    }

    IrpContext->Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}

static
NTSTATUS
VfatMarkVolumeDirty(
    PVFAT_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExt;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("VfatMarkVolumeDirty(IrpContext %p)\n", IrpContext);
    DeviceExt = IrpContext->DeviceExt;

    if (!BooleanFlagOn(DeviceExt->VolumeFcb->Flags, VCB_IS_DIRTY))
    {
        Status = SetDirtyStatus(DeviceExt, TRUE);
    }

    DeviceExt->VolumeFcb->Flags &= ~VCB_CLEAR_DIRTY;

    return Status;
}

static
NTSTATUS
VfatLockOrUnlockVolume(
    PVFAT_IRP_CONTEXT IrpContext,
    BOOLEAN Lock)
{
    PFILE_OBJECT FileObject;
    PDEVICE_EXTENSION DeviceExt;
    PVFATFCB Fcb;
    PVPB Vpb;

    DPRINT("VfatLockOrUnlockVolume(%p, %d)\n", IrpContext, Lock);

    DeviceExt = IrpContext->DeviceExt;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;
    Vpb = DeviceExt->FATFileObject->Vpb;

    /* Only allow locking with the volume open */
    if (!BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME))
    {
        return STATUS_ACCESS_DENIED;
    }

    /* Bail out if it's already in the demanded state */
    if ((BooleanFlagOn(DeviceExt->Flags, VCB_VOLUME_LOCKED) && Lock) ||
        (!BooleanFlagOn(DeviceExt->Flags, VCB_VOLUME_LOCKED) && !Lock))
    {
        return STATUS_ACCESS_DENIED;
    }

    /* Bail out if it's already in the demanded state */
    if ((BooleanFlagOn(Vpb->Flags, VPB_LOCKED) && Lock) ||
        (!BooleanFlagOn(Vpb->Flags, VPB_LOCKED) && !Lock))
    {
        return STATUS_ACCESS_DENIED;
    }

    if (Lock)
    {
        FsRtlNotifyVolumeEvent(IrpContext->Stack->FileObject, FSRTL_VOLUME_LOCK);
    }

    /* Deny locking if we're not alone */
    if (Lock && DeviceExt->OpenHandleCount != 1)
    {
        PLIST_ENTRY ListEntry;

#if 1
        /* FIXME: Hack that allows locking the system volume on
         * boot so that autochk can run properly
         * That hack is, on purpose, really restrictive
         * it will only allow locking with two directories
         * open: current directory of smss and autochk.
         */
        BOOLEAN ForceLock = TRUE;
        ULONG HandleCount = 0;

        /* Only allow boot volume */
        if (BooleanFlagOn(DeviceExt->Flags, VCB_IS_SYS_OR_HAS_PAGE))
        {
            /* We'll browse all the FCB */
            ListEntry = DeviceExt->FcbListHead.Flink;
            while (ListEntry != &DeviceExt->FcbListHead)
            {
                Fcb = CONTAINING_RECORD(ListEntry, VFATFCB, FcbListEntry);
                ListEntry = ListEntry->Flink;

                /* If no handle: that FCB is no problem for locking
                 * so ignore it
                 */
                if (Fcb->OpenHandleCount == 0)
                {
                    continue;
                }

                /* Not a dir? We're no longer at boot */
                if (!vfatFCBIsDirectory(Fcb))
                {
                    ForceLock = FALSE;
                    break;
                }

                /* If we have cached initialized and several handles, we're
                   not in the boot case
                 */
                if (Fcb->FileObject != NULL && Fcb->OpenHandleCount > 1)
                {
                    ForceLock = FALSE;
                    break;
                }

                /* Count the handles */
                HandleCount += Fcb->OpenHandleCount;
                /* More than two handles? Then, we're not booting anymore */
                if (HandleCount > 2)
                {
                    ForceLock = FALSE;
                    break;
                }
            }
        }
        else
        {
            ForceLock = FALSE;
        }

        /* Here comes the hack, ignore the failure! */
        if (!ForceLock)
        {
#endif

        DPRINT1("Can't lock: %u opened\n", DeviceExt->OpenHandleCount);

        ListEntry = DeviceExt->FcbListHead.Flink;
        while (ListEntry != &DeviceExt->FcbListHead)
        {
            Fcb = CONTAINING_RECORD(ListEntry, VFATFCB, FcbListEntry);
            ListEntry = ListEntry->Flink;

            if (Fcb->OpenHandleCount  > 0)
            {
                DPRINT1("Opened (%u - %u): %wZ\n", Fcb->OpenHandleCount, Fcb->RefCount, &Fcb->PathNameU);
            }
        }

        FsRtlNotifyVolumeEvent(IrpContext->Stack->FileObject, FSRTL_VOLUME_LOCK_FAILED);

        return STATUS_ACCESS_DENIED;

#if 1
        /* End of the hack: be verbose about its usage,
         * just in case we would mess up everything!
         */
        }
        else
        {
            DPRINT1("HACK: Using lock-hack!\n");
        }
#endif
    }

    /* Finally, proceed */
    if (Lock)
    {
        /* Flush volume & files */
        VfatFlushVolume(DeviceExt, DeviceExt->VolumeFcb);

        /* The volume is now clean */
        if (BooleanFlagOn(DeviceExt->VolumeFcb->Flags, VCB_CLEAR_DIRTY) &&
            BooleanFlagOn(DeviceExt->VolumeFcb->Flags, VCB_IS_DIRTY))
        {
            /* Drop the dirty bit */
            if (NT_SUCCESS(SetDirtyStatus(DeviceExt, FALSE)))
                ClearFlag(DeviceExt->VolumeFcb->Flags, VCB_IS_DIRTY);
        }

        DeviceExt->Flags |= VCB_VOLUME_LOCKED;
        Vpb->Flags |= VPB_LOCKED;
    }
    else
    {
        DeviceExt->Flags &= ~VCB_VOLUME_LOCKED;
        Vpb->Flags &= ~VPB_LOCKED;

        FsRtlNotifyVolumeEvent(IrpContext->Stack->FileObject, FSRTL_VOLUME_UNLOCK);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
VfatDismountVolume(
    PVFAT_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExt;
    PLIST_ENTRY NextEntry;
    PVFATFCB Fcb;
    PFILE_OBJECT FileObject;

    DPRINT("VfatDismountVolume(%p)\n", IrpContext);

    DeviceExt = IrpContext->DeviceExt;
    FileObject = IrpContext->FileObject;

    /* We HAVE to be locked. Windows also allows dismount with no lock
     * but we're here mainly for 1st stage, so KISS
     */
    if (!BooleanFlagOn(DeviceExt->Flags, VCB_VOLUME_LOCKED))
    {
        return STATUS_ACCESS_DENIED;
    }

    /* Deny dismount of boot volume */
    if (BooleanFlagOn(DeviceExt->Flags, VCB_IS_SYS_OR_HAS_PAGE))
    {
        return STATUS_ACCESS_DENIED;
    }

    /* Race condition? */
    if (BooleanFlagOn(DeviceExt->Flags, VCB_DISMOUNT_PENDING))
    {
        return STATUS_VOLUME_DISMOUNTED;
    }

    /* Notify we'll dismount. Pass that point there's no reason we fail */
    FsRtlNotifyVolumeEvent(IrpContext->Stack->FileObject, FSRTL_VOLUME_DISMOUNT);

    ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);

    /* Flush volume & files */
    VfatFlushVolume(DeviceExt, (PVFATFCB)FileObject->FsContext);

    /* The volume is now clean */
    if (BooleanFlagOn(DeviceExt->VolumeFcb->Flags, VCB_CLEAR_DIRTY) &&
        BooleanFlagOn(DeviceExt->VolumeFcb->Flags, VCB_IS_DIRTY))
    {
        /* Drop the dirty bit */
        if (NT_SUCCESS(SetDirtyStatus(DeviceExt, FALSE)))
            DeviceExt->VolumeFcb->Flags &= ~VCB_IS_DIRTY;
    }

    /* Rebrowse the FCB in order to free them now */
    while (!IsListEmpty(&DeviceExt->FcbListHead))
    {
        NextEntry = RemoveTailList(&DeviceExt->FcbListHead);
        Fcb = CONTAINING_RECORD(NextEntry, VFATFCB, FcbListEntry);

        if (Fcb == DeviceExt->RootFcb)
            DeviceExt->RootFcb = NULL;
        else if (Fcb == DeviceExt->VolumeFcb)
            DeviceExt->VolumeFcb = NULL;

        vfatDestroyFCB(Fcb);
    }

    /* We are uninitializing, the VCB cannot be used anymore */
    ClearFlag(DeviceExt->Flags, VCB_GOOD);

    /* Mark we're being dismounted */
    DeviceExt->Flags |= VCB_DISMOUNT_PENDING;
#ifndef ENABLE_SWAPOUT
    IrpContext->DeviceObject->Vpb->Flags &= ~VPB_MOUNTED;
#endif

    ExReleaseResourceLite(&DeviceExt->FatResource);

    return STATUS_SUCCESS;
}

static
NTSTATUS
VfatGetStatistics(
    PVFAT_IRP_CONTEXT IrpContext)
{
    PVOID Buffer;
    ULONG Length;
    NTSTATUS Status;
    PDEVICE_EXTENSION DeviceExt;

    DeviceExt = IrpContext->DeviceExt;
    Length = IrpContext->Stack->Parameters.FileSystemControl.OutputBufferLength;
    Buffer = IrpContext->Irp->AssociatedIrp.SystemBuffer;

    if (Length < sizeof(FILESYSTEM_STATISTICS))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Buffer == NULL)
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    if (Length >= sizeof(STATISTICS) * VfatGlobalData->NumberProcessors)
    {
        Length = sizeof(STATISTICS) * VfatGlobalData->NumberProcessors;
        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = STATUS_BUFFER_OVERFLOW;
    }

    RtlCopyMemory(Buffer, DeviceExt->Statistics, Length);
    IrpContext->Irp->IoStatus.Information = Length;

    return Status;
}

/*
 * FUNCTION: File system control
 */
NTSTATUS
VfatFileSystemControl(
    PVFAT_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status;

    DPRINT("VfatFileSystemControl(IrpContext %p)\n", IrpContext);

    ASSERT(IrpContext);
    ASSERT(IrpContext->Irp);
    ASSERT(IrpContext->Stack);

    IrpContext->Irp->IoStatus.Information = 0;

    switch (IrpContext->MinorFunction)
    {
        case IRP_MN_KERNEL_CALL:
        case IRP_MN_USER_FS_REQUEST:
            switch(IrpContext->Stack->Parameters.DeviceIoControl.IoControlCode)
            {
                case FSCTL_GET_VOLUME_BITMAP:
                    Status = VfatGetVolumeBitmap(IrpContext);
                    break;

                case FSCTL_GET_RETRIEVAL_POINTERS:
                    Status = VfatGetRetrievalPointers(IrpContext);
                    break;

                case FSCTL_MOVE_FILE:
                    Status = VfatMoveFile(IrpContext);
                    break;

                case FSCTL_IS_VOLUME_DIRTY:
                    Status = VfatIsVolumeDirty(IrpContext);
                    break;

                case FSCTL_MARK_VOLUME_DIRTY:
                    Status = VfatMarkVolumeDirty(IrpContext);
                    break;

                case FSCTL_LOCK_VOLUME:
                    Status = VfatLockOrUnlockVolume(IrpContext, TRUE);
                    break;

                case FSCTL_UNLOCK_VOLUME:
                    Status = VfatLockOrUnlockVolume(IrpContext, FALSE);
                    break;

                case FSCTL_DISMOUNT_VOLUME:
                    Status = VfatDismountVolume(IrpContext);
                    break;

                case FSCTL_FILESYSTEM_GET_STATISTICS:
                    Status = VfatGetStatistics(IrpContext);
                    break;

                default:
                    Status = STATUS_INVALID_DEVICE_REQUEST;
            }
            break;

        case IRP_MN_MOUNT_VOLUME:
            Status = VfatMount(IrpContext);
            break;

        case IRP_MN_VERIFY_VOLUME:
            DPRINT("VFATFS: IRP_MN_VERIFY_VOLUME\n");
            Status = VfatVerify(IrpContext);
            break;

        default:
            DPRINT("VFAT FSC: MinorFunction %u\n", IrpContext->MinorFunction);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return Status;
}
