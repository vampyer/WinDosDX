/*
 * PROJECT:     exFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Writing exFAT directory entry sets
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 *              Based on the vfatfs directory writing routines by
 *              Rex Jolliff, Hervé Poussineau and Pierre Schweitzer
 */

/* INCLUDES *****************************************************************/

#include "vfat.h"

#define NDEBUG
#include <debug.h>

NTSTATUS
vfatFCBInitializeCacheFromVolume(
    PVCB vcb,
    PVFATFCB fcb)
{
    PFILE_OBJECT fileObject;
    PVFATCCB newCCB;
    NTSTATUS status;
    BOOLEAN Acquired;

    /* Don't re-initialize if already done */
    if (BooleanFlagOn(fcb->Flags, FCB_CACHE_INITIALIZED))
    {
        return STATUS_SUCCESS;
    }

    ASSERT(vfatFCBIsDirectory(fcb));
    ASSERT(fcb->FileObject == NULL);

    Acquired = FALSE;
    if (!ExIsResourceAcquiredExclusive(&vcb->DirResource))
    {
        ExAcquireResourceExclusiveLite(&vcb->DirResource, TRUE);
        Acquired = TRUE;
    }

    fileObject = IoCreateStreamFileObject (NULL, vcb->StorageDevice);
    if (fileObject == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Quit;
    }

    newCCB = ExAllocateFromNPagedLookasideList(&VfatGlobalData->CcbLookasideList);
    if (newCCB == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        ObDereferenceObject(fileObject);
        goto Quit;
    }
    RtlZeroMemory(newCCB, sizeof (VFATCCB));

    fileObject->SectionObjectPointer = &fcb->SectionObjectPointers;
    fileObject->FsContext = fcb;
    fileObject->FsContext2 = newCCB;
    fileObject->Vpb = vcb->IoVPB;
    fcb->FileObject = fileObject;

    _SEH2_TRY
    {
        CcInitializeCacheMap(fileObject,
                             (PCC_FILE_SIZES)(&fcb->RFCB.AllocationSize),
                             TRUE,
                             &VfatGlobalData->CacheMgrCallbacks,
                             fcb);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        status = _SEH2_GetExceptionCode();
        fcb->FileObject = NULL;
        ExFreeToNPagedLookasideList(&VfatGlobalData->CcbLookasideList, newCCB);
        ObDereferenceObject(fileObject);
        if (Acquired)
        {
            ExReleaseResourceLite(&vcb->DirResource);
        }
        return status;
    }
    _SEH2_END;

    vfatGrabFCB(vcb, fcb);
    SetFlag(fcb->Flags, FCB_CACHE_INITIALIZED);
    status = STATUS_SUCCESS;

Quit:
    if (Acquired)
    {
        ExReleaseResourceLite(&vcb->DirResource);
    }

    return status;
}

/*
 * Copy Count entries starting at StartIndex between a directory's cached
 * stream and Buffer. Sets may straddle a page, so each page is handled on
 * its own.
 */
static
NTSTATUS
ExfatTransferSet(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB DirFcb,
    ULONG StartIndex,
    ULONG Count,
    PUCHAR Buffer,
    BOOLEAN Write)
{
    ULONG Done = 0;
    ULONG Index;
    ULONG Chunk;
    LARGE_INTEGER Offset;
    PVOID Context;
    PUCHAR Entries;
    NTSTATUS Status;

    /*
     * A set refers to clusters through the bitmap and the FAT: put those on
     * disk before the set enters the directory's cache. The lazy writer
     * does the same, but the memory manager can write directory pages on
     * its own (page-out, a cache map torn down) without asking it.
     */
    if (Write)
    {
        Status = ExfatFlushAllocation(DeviceExt);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    while (Done < Count)
    {
        Index = StartIndex + Done;
        Chunk = min(EXFAT_ENTRIES_PER_PAGE - Index % EXFAT_ENTRIES_PER_PAGE, Count - Done);
        Offset.QuadPart = (LONGLONG)Index * EXFAT_ENTRY_SIZE;
        if (Offset.QuadPart + Chunk * EXFAT_ENTRY_SIZE > DirFcb->RFCB.FileSize.QuadPart)
            return STATUS_FILE_CORRUPT_ERROR;

        _SEH2_TRY
        {
            if (Write)
                CcPinRead(DirFcb->FileObject, &Offset, Chunk * EXFAT_ENTRY_SIZE, PIN_WAIT, &Context, (PVOID*)&Entries);
            else
                CcMapData(DirFcb->FileObject, &Offset, Chunk * EXFAT_ENTRY_SIZE, MAP_WAIT, &Context, (PVOID*)&Entries);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;

        if (Write)
        {
            RtlCopyMemory(Entries, Buffer + Done * EXFAT_ENTRY_SIZE, Chunk * EXFAT_ENTRY_SIZE);
            CcSetDirtyPinnedData(Context, NULL);
        }
        else
        {
            RtlCopyMemory(Buffer + Done * EXFAT_ENTRY_SIZE, Entries, Chunk * EXFAT_ENTRY_SIZE);
        }
        CcUnpinData(Context);
        Done += Chunk;
    }
    return STATUS_SUCCESS;
}

/*
 * Write a file's entry set from the directory's cache to disk now. Used
 * before clusters the old set referred to are freed: on disk, the set must
 * stop referring to them first.
 */
NTSTATUS
ExfatFlushFcbSet(
    PVFATFCB pFcb)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;

    Offset.QuadPart = (LONGLONG)pFcb->startIndex * EXFAT_ENTRY_SIZE;
    CcFlushCache(&pFcb->parentFcb->SectionObjectPointers, &Offset,
                 pFcb->EntryCount * EXFAT_ENTRY_SIZE, &IoStatus);
    return IoStatus.Status;
}

/*
 * Read a file's whole entry set and check that it still is the set the FCB
 * describes. The caller frees *Set.
 */
static
NTSTATUS
ExfatReadFcbSet(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB pFcb,
    PUCHAR *Set)
{
    NTSTATUS Status;

    Status = vfatFCBInitializeCacheFromVolume(DeviceExt, pFcb->parentFcb);
    if (!NT_SUCCESS(Status))
        return Status;

    if (pFcb->EntryCount < 3 || pFcb->EntryCount > EXFAT_MAX_SET_ENTRIES)
        return STATUS_FILE_CORRUPT_ERROR;

    *Set = ExAllocatePoolWithTag(NonPagedPool, pFcb->EntryCount * EXFAT_ENTRY_SIZE, TAG_DIRENT);
    if (!*Set)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ExfatTransferSet(DeviceExt, pFcb->parentFcb, pFcb->startIndex, pFcb->EntryCount, *Set, FALSE);
    if (NT_SUCCESS(Status) &&
        ((*Set)[0] != EXFAT_TYPE_FILE ||
         ((PEXFAT_FILE_ENTRY)*Set)->SecondaryCount + 1 != pFcb->EntryCount ||
         (*Set)[EXFAT_ENTRY_SIZE] != EXFAT_TYPE_STREAM))
    {
        DPRINT1("exFAT: entry set of '%wZ' at %lu is not where the FCB expects it\n",
                &pFcb->PathNameU, pFcb->startIndex);
        Status = STATUS_FILE_CORRUPT_ERROR;
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(*Set, TAG_DIRENT);
        *Set = NULL;
    }
    return Status;
}

/* Bring the FCB's copy of the Stream entry up to date with its sizes and chain. */
VOID
ExfatSyncEntryFromFcb(
    PVFATFCB pFcb)
{
    PEXFAT_STREAM_ENTRY Stream = &pFcb->entry.Stream;

    Stream->FirstCluster = pFcb->Chain.FirstCluster;
    Stream->GeneralSecondaryFlags &= ~EXFAT_FLAG_NO_FAT_CHAIN;
    Stream->GeneralSecondaryFlags |= EXFAT_FLAG_ALLOCATION_POSSIBLE;
    if (pFcb->Chain.NoFatChain && pFcb->Chain.FirstCluster != 0)
        Stream->GeneralSecondaryFlags |= EXFAT_FLAG_NO_FAT_CHAIN;

    Stream->DataLength = pFcb->RFCB.FileSize.QuadPart;
    if (vfatFCBIsDirectory(pFcb))
        Stream->ValidDataLength = Stream->DataLength;
    else
        Stream->ValidDataLength = min((ULONGLONG)pFcb->RFCB.ValidDataLength.QuadPart, Stream->DataLength);
}

/*
 * Write the FCB's attributes, times, sizes and clusters back into its entry
 * set, with a new checksum.
 */
NTSTATUS
VfatUpdateEntry(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB pFcb)
{
    PUCHAR Set;
    NTSTATUS Status;

    ASSERT(pFcb);
    DPRINT("updEntry startIndex %u, PathName \'%wZ\'\n", pFcb->startIndex, &pFcb->PathNameU);

    if (vfatFCBIsRoot(pFcb) || BooleanFlagOn(pFcb->Flags, FCB_IS_FAT | FCB_IS_VOLUME))
    {
        return STATUS_SUCCESS;
    }
    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
    {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    ASSERT(pFcb->parentFcb);

    Status = ExfatReadFcbSet(DeviceExt, pFcb, &Set);
    if (!NT_SUCCESS(Status))
        return Status;

    ExfatSyncEntryFromFcb(pFcb);

    /* File entry: attributes and times (bytes 4..31) */
    RtlCopyMemory(Set + 4, (PUCHAR)&pFcb->entry.File + 4, EXFAT_ENTRY_SIZE - 4);
    /* Stream entry: keep the name length and hash, replace flags, clusters and sizes. */
    ((PEXFAT_STREAM_ENTRY)(Set + EXFAT_ENTRY_SIZE))->GeneralSecondaryFlags = pFcb->entry.Stream.GeneralSecondaryFlags;
    ((PEXFAT_STREAM_ENTRY)(Set + EXFAT_ENTRY_SIZE))->FirstCluster = pFcb->entry.Stream.FirstCluster;
    ((PEXFAT_STREAM_ENTRY)(Set + EXFAT_ENTRY_SIZE))->DataLength = pFcb->entry.Stream.DataLength;
    ((PEXFAT_STREAM_ENTRY)(Set + EXFAT_ENTRY_SIZE))->ValidDataLength = pFcb->entry.Stream.ValidDataLength;
    ((PEXFAT_FILE_ENTRY)Set)->SetChecksum = ExfatEntrySetChecksum(Set, pFcb->EntryCount);
    pFcb->entry.File.SetChecksum = ((PEXFAT_FILE_ENTRY)Set)->SetChecksum;

    Status = ExfatTransferSet(DeviceExt, pFcb->parentFcb, pFcb->startIndex, pFcb->EntryCount, Set, TRUE);
    if (NT_SUCCESS(Status))
        pFcb->Flags &= ~FCB_IS_DIRTY;

    ExFreePoolWithTag(Set, TAG_DIRENT);
    return Status;
}

/*
 * rename an existing entry: a new set with the new name replaces the old one
 */
NTSTATUS
vfatRenameEntry(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB pFcb,
    IN PUNICODE_STRING FileName,
    IN BOOLEAN CaseChangeOnly)
{
    UNREFERENCED_PARAMETER(CaseChangeOnly);
    DPRINT("vfatRenameEntry(%p, %p, %wZ)\n", DeviceExt, pFcb, FileName);
    return VfatMoveEntry(DeviceExt, pFcb, FileName, pFcb->parentFcb);
}

/* Zero a cluster directly on disk, before anything refers to it. */
static
NTSTATUS
ExfatZeroCluster(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster)
{
    ULONG BytesPerCluster = DeviceExt->FatInfo.BytesPerCluster;
    ULONG Chunk = min(BytesPerCluster, 0x10000);
    ULONG Done;
    PUCHAR Zeroes;
    LARGE_INTEGER Offset;
    NTSTATUS Status = STATUS_SUCCESS;

    Zeroes = ExAllocatePoolWithTag(NonPagedPool, Chunk, TAG_BUFFER);
    if (!Zeroes)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Zeroes, Chunk);

    for (Done = 0; Done < BytesPerCluster && NT_SUCCESS(Status); Done += Chunk)
    {
        Offset.QuadPart = ClusterToSector(DeviceExt, Cluster) * DeviceExt->FatInfo.BytesPerSector + Done;
        Status = VfatWriteDisk(DeviceExt->StorageDevice, &Offset, Chunk, Zeroes, FALSE);
    }
    ExFreePoolWithTag(Zeroes, TAG_BUFFER);
    return Status;
}

/*
 * Find nbSlots consecutive unused entries in a directory, growing it by
 * zeroed clusters when needed. *start receives the first entry's index.
 */
BOOLEAN
vfatFindDirSpace(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB pDirFcb,
    IN ULONG nbSlots,
    OUT PULONG start)
{
    LARGE_INTEGER FileOffset;
    LARGE_INTEGER AllocationSize;
    ULONG i;
    ULONG Count;
    ULONG Run = 0;
    PUCHAR Entry = NULL;
    PVOID Context = NULL;
    NTSTATUS Status;

    Status = vfatFCBInitializeCacheFromVolume(DeviceExt, pDirFcb);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Count = (ULONG)(pDirFcb->RFCB.FileSize.QuadPart / EXFAT_ENTRY_SIZE);
    for (i = 0; i < Count; i++, Entry += EXFAT_ENTRY_SIZE)
    {
        if (Context == NULL || (i % EXFAT_ENTRIES_PER_PAGE) == 0)
        {
            if (Context)
                CcUnpinData(Context);
            FileOffset.QuadPart = (LONGLONG)i * EXFAT_ENTRY_SIZE;
            _SEH2_TRY
            {
                CcMapData(pDirFcb->FileObject, &FileOffset, PAGE_SIZE, MAP_WAIT, &Context, (PVOID*)&Entry);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                _SEH2_YIELD(return FALSE);
            }
            _SEH2_END;
        }

        /* End-of-directory and deleted entries are both unused. */
        if ((Entry[0] & EXFAT_TYPE_IN_USE) == 0)
        {
            if (++Run == nbSlots)
                break;
        }
        else
        {
            Run = 0;
        }
    }
    if (Context)
        CcUnpinData(Context);

    if (Run == nbSlots)
    {
        *start = i - nbSlots + 1;
        return TRUE;
    }

    /* Continue the trailing run of unused entries into new clusters. */
    *start = Count - Run;
    while (*start + nbSlots > Count)
    {
        ULONG NewCluster;
        ULONG OldClusters = pDirFcb->Chain.Count;

        /* exFAT directories are limited to 256 MB. */
        if (pDirFcb->RFCB.FileSize.QuadPart + DeviceExt->FatInfo.BytesPerCluster > 256 * 1024 * 1024)
            return FALSE;

        AllocationSize.QuadPart = pDirFcb->RFCB.FileSize.QuadPart + DeviceExt->FatInfo.BytesPerCluster;
        Status = VfatSetAllocationSizeInformation(pDirFcb->FileObject, pDirFcb,
                                                  DeviceExt, &AllocationSize);
        if (!NT_SUCCESS(Status) || pDirFcb->Chain.Count != OldClusters + 1)
            return FALSE;

        /* Clear the new cluster, through the cache that now covers it. */
        Status = OffsetToCluster(DeviceExt, &pDirFcb->Chain, pDirFcb->Chain.FirstCluster,
                                 (ULONGLONG)OldClusters * DeviceExt->FatInfo.BytesPerCluster,
                                 &NewCluster, FALSE);
        if (!NT_SUCCESS(Status))
            return FALSE;
        for (FileOffset.QuadPart = (LONGLONG)OldClusters * DeviceExt->FatInfo.BytesPerCluster;
             FileOffset.QuadPart < pDirFcb->RFCB.FileSize.QuadPart;
             FileOffset.QuadPart += PAGE_SIZE)
        {
            _SEH2_TRY
            {
                CcPinRead(pDirFcb->FileObject, &FileOffset, PAGE_SIZE, PIN_WAIT, &Context, (PVOID*)&Entry);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                _SEH2_YIELD(return FALSE);
            }
            _SEH2_END;
            RtlZeroMemory(Entry, PAGE_SIZE);
            CcSetDirtyPinnedData(Context, NULL);
            CcUnpinData(Context);
        }
        Count = (ULONG)(pDirFcb->RFCB.FileSize.QuadPart / EXFAT_ENTRY_SIZE);
    }

    DPRINT("nbSlots %u, entry number %u\n", nbSlots, *start);
    return TRUE;
}

static
BOOLEAN
ExfatIsLegalName(
    PCUNICODE_STRING NameU)
{
    USHORT i;
    WCHAR c;

    if (NameU->Length == 0 || NameU->Length / sizeof(WCHAR) > EXFAT_MAX_NAME_LENGTH)
        return FALSE;
    for (i = 0; i < NameU->Length / sizeof(WCHAR); i++)
    {
        c = NameU->Buffer[i];
        if (c < 0x20 || vfatIsLongIllegal(c))
            return FALSE;
    }
    return TRUE;
}

/*
 * create a new entry set; with MoveContext, for an existing file being
 * renamed or moved (its FCB is *Fcb and keeps its clusters)
 */
NTSTATUS
VfatAddEntry(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PUNICODE_STRING NameU,
    IN PVFATFCB* Fcb,
    IN PVFATFCB ParentFcb,
    IN ULONG RequestedOptions,
    IN USHORT ReqAttr,
    IN PVFAT_MOVE_CONTEXT MoveContext)
{
    VFAT_DIRENTRY_CONTEXT DirContext;
    PEXFAT_FILE_ENTRY FileEntry;
    PEXFAT_STREAM_ENTRY StreamEntry;
    PEXFAT_NAME_ENTRY NameEntry;
    EXFAT_CHAIN DirChain = { 0 };
    LARGE_INTEGER SystemTime;
    ULONG NameChars;
    ULONG NameEntries;
    ULONG Count;
    ULONG Cluster = 0;
    ULONG i;
    PUCHAR Set;
    BOOLEAN IsDirectory;
    NTSTATUS Status;

    DPRINT("addEntry: Name='%wZ', Dir='%wZ'\n", NameU, &ParentFcb->PathNameU);

    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (!ExfatIsLegalName(NameU))
        return STATUS_OBJECT_NAME_INVALID;

    IsDirectory = BooleanFlagOn(RequestedOptions, FILE_DIRECTORY_FILE);
    NameChars = NameU->Length / sizeof(WCHAR);
    NameEntries = (NameChars + EXFAT_NAME_CHARS_PER_ENTRY - 1) / EXFAT_NAME_CHARS_PER_ENTRY;
    Count = 2 + NameEntries;

    Set = ExAllocatePoolWithTag(NonPagedPool, Count * EXFAT_ENTRY_SIZE, TAG_DIRENT);
    if (!Set)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Set, Count * EXFAT_ENTRY_SIZE);
    FileEntry = (PEXFAT_FILE_ENTRY)Set;
    StreamEntry = (PEXFAT_STREAM_ENTRY)(Set + EXFAT_ENTRY_SIZE);

    /* File entry */
    if (MoveContext)
    {
        /* Keep attributes and times */
        RtlCopyMemory(FileEntry, &MoveContext->Entry.File, sizeof(EXFAT_FILE_ENTRY));
    }
    else
    {
        KeQuerySystemTime(&SystemTime);
        ExfatSystemTimeToTimestamp(&SystemTime, &FileEntry->CreateTimestamp,
                                   &FileEntry->Create10msIncrement, &FileEntry->CreateUtcOffset);
        FileEntry->LastModifiedTimestamp = FileEntry->CreateTimestamp;
        FileEntry->LastModified10msIncrement = FileEntry->Create10msIncrement;
        FileEntry->LastModifiedUtcOffset = FileEntry->CreateUtcOffset;
        FileEntry->LastAccessedTimestamp = FileEntry->CreateTimestamp;
        FileEntry->LastAccessedUtcOffset = FileEntry->CreateUtcOffset;
    }
    FileEntry->EntryType = EXFAT_TYPE_FILE;
    FileEntry->SecondaryCount = (UCHAR)(Count - 1);
    FileEntry->FileAttributes = (ReqAttr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                            FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE)) |
                                (IsDirectory ? FILE_ATTRIBUTE_DIRECTORY : 0);

    /* Stream extension */
    StreamEntry->EntryType = EXFAT_TYPE_STREAM;
    StreamEntry->NameLength = (UCHAR)NameChars;
    StreamEntry->NameHash = ExfatNameHash(DeviceExt, NameU);
    if (MoveContext)
    {
        StreamEntry->GeneralSecondaryFlags = MoveContext->Entry.Stream.GeneralSecondaryFlags;
        StreamEntry->FirstCluster = MoveContext->Chain.FirstCluster;
        StreamEntry->DataLength = MoveContext->Entry.Stream.DataLength;
        StreamEntry->ValidDataLength = MoveContext->Entry.Stream.ValidDataLength;
    }
    else
    {
        StreamEntry->GeneralSecondaryFlags = EXFAT_FLAG_ALLOCATION_POSSIBLE;
    }

    /* Name entries */
    for (i = 0; i < NameEntries; i++)
    {
        NameEntry = (PEXFAT_NAME_ENTRY)(Set + (2 + i) * EXFAT_ENTRY_SIZE);
        NameEntry->EntryType = EXFAT_TYPE_NAME;
        RtlCopyMemory(NameEntry->FileName, NameU->Buffer + i * EXFAT_NAME_CHARS_PER_ENTRY,
                      min(EXFAT_NAME_CHARS_PER_ENTRY, NameChars - i * EXFAT_NAME_CHARS_PER_ENTRY) * sizeof(WCHAR));
    }

    RtlZeroMemory(&DirContext, sizeof(DirContext));
    if (!vfatFindDirSpace(DeviceExt, ParentFcb, Count, &DirContext.StartIndex))
    {
        ExFreePoolWithTag(Set, TAG_DIRENT);
        return STATUS_DISK_FULL;
    }

    /* A new directory gets one cluster, zeroed on disk before the entry
       that points at it is written. */
    if (IsDirectory && !MoveContext)
    {
        Status = NextCluster(DeviceExt, &DirChain, &Cluster, TRUE);
        if (NT_SUCCESS(Status))
            Status = ExfatZeroCluster(DeviceExt, Cluster);
        if (!NT_SUCCESS(Status))
        {
            if (DirChain.FirstCluster)
            {
                ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
                ExfatFreeClusters(DeviceExt, &DirChain, 0);
                ExReleaseResourceLite(&DeviceExt->FatResource);
            }
            ExFreePoolWithTag(Set, TAG_DIRENT);
            return Status;
        }
        StreamEntry->FirstCluster = Cluster;
        StreamEntry->DataLength = DeviceExt->FatInfo.BytesPerCluster;
        StreamEntry->ValidDataLength = DeviceExt->FatInfo.BytesPerCluster;
    }

    FileEntry->SetChecksum = ExfatEntrySetChecksum(Set, Count);
    ASSERT(BooleanFlagOn(ParentFcb->Flags, FCB_CACHE_INITIALIZED));
    Status = ExfatTransferSet(DeviceExt, ParentFcb, DirContext.StartIndex, Count, Set, TRUE);
    if (!NT_SUCCESS(Status))
    {
        if (DirChain.FirstCluster)
        {
            ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
            ExfatFreeClusters(DeviceExt, &DirChain, 0);
            ExReleaseResourceLite(&DeviceExt->FatResource);
        }
        ExFreePoolWithTag(Set, TAG_DIRENT);
        return Status;
    }

    DirContext.DeviceExt = DeviceExt;
    DirContext.DirIndex = DirContext.StartIndex + Count - 1;
    DirContext.EntryCount = Count;
    RtlCopyMemory(&DirContext.DirEntry.File, FileEntry, sizeof(EXFAT_FILE_ENTRY));
    RtlCopyMemory(&DirContext.DirEntry.Stream, StreamEntry, sizeof(EXFAT_STREAM_ENTRY));
    DirContext.LongNameU = *NameU;
    ExFreePoolWithTag(Set, TAG_DIRENT);

    if (MoveContext != NULL)
    {
        /* We're modifying an existing FCB - likely rename/move */
        Status = vfatUpdateFCB(DeviceExt, *Fcb, &DirContext, ParentFcb);
        if (NT_SUCCESS(Status))
            (*Fcb)->Chain = MoveContext->Chain;
    }
    else
    {
        Status = vfatMakeFCBFromDirEntry(DeviceExt, ParentFcb, &DirContext, Fcb);
    }

    DPRINT("addentry %s\n", NT_SUCCESS(Status) ? "ok" : "failed");
    return Status;
}

/*
 * delete an existing entry set; with MoveContext, keep the file's clusters
 * and describe the file for the entry that replaces it
 */
NTSTATUS
VfatDelEntry(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB pFcb,
    OUT PVFAT_MOVE_CONTEXT MoveContext)
{
    PUCHAR Set;
    ULONG i;
    NTSTATUS Status;

    ASSERT(pFcb);
    ASSERT(pFcb->parentFcb);

    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        return STATUS_MEDIA_WRITE_PROTECTED;

    DPRINT("delEntry PathName \'%wZ\', entries %u..%u\n", &pFcb->PathNameU,
           pFcb->startIndex, pFcb->startIndex + pFcb->EntryCount - 1);

    Status = ExfatReadFcbSet(DeviceExt, pFcb, &Set);
    if (!NT_SUCCESS(Status))
        return Status;

    if (MoveContext != NULL)
    {
        ExfatSyncEntryFromFcb(pFcb);
        MoveContext->Entry = pFcb->entry;
        MoveContext->Chain = pFcb->Chain;
    }

    /* Clearing the in-use bit of each entry frees the set. */
    for (i = 0; i < pFcb->EntryCount; i++)
        Set[i * EXFAT_ENTRY_SIZE] &= ~EXFAT_TYPE_IN_USE;

    Status = ExfatTransferSet(DeviceExt, pFcb->parentFcb, pFcb->startIndex, pFcb->EntryCount, Set, TRUE);
    ExFreePoolWithTag(Set, TAG_DIRENT);
    if (!NT_SUCCESS(Status))
        return Status;

    /* In case of moving, don't delete data */
    if (MoveContext == NULL)
    {
        /* The name is free now: a new file may take it even while this FCB
           waits for its last reference (the caller holds DirResource). */
        vfatUnlinkFCB(DeviceExt, pFcb);

        /* The freed set goes to disk before the clusters are released. */
        Status = ExfatFlushFcbSet(pFcb);
        if (!NT_SUCCESS(Status))
            return Status;
        ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
        Status = ExfatFreeClusters(DeviceExt, &pFcb->Chain, 0);
        ExReleaseResourceLite(&DeviceExt->FatResource);
    }

    return Status;
}

/*
 * move an existing entry
 */
NTSTATUS
VfatMoveEntry(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB pFcb,
    IN PUNICODE_STRING FileName,
    IN PVFATFCB ParentFcb)
{
    NTSTATUS Status;
    PVFATFCB OldParent;
    VFAT_MOVE_CONTEXT MoveContext;

    DPRINT("VfatMoveEntry(%p, %p, %wZ, %p)\n", DeviceExt, pFcb, FileName, ParentFcb);

    /* Delete old entry while keeping data */
    Status = VfatDelEntry(DeviceExt, pFcb, &MoveContext);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    OldParent = pFcb->parentFcb;
    ExfatFlushAllocation(DeviceExt);
    CcFlushCache(&OldParent->SectionObjectPointers, NULL, 0, NULL);
    MoveContext.InPlace = (OldParent == ParentFcb);

    /* Add our new entry with our cluster */
    Status = VfatAddEntry(DeviceExt,
                          FileName,
                          &pFcb,
                          ParentFcb,
                          (vfatFCBIsDirectory(pFcb) ? FILE_DIRECTORY_FILE : 0),
                          pFcb->entry.File.FileAttributes,
                          &MoveContext);
    if (!NT_SUCCESS(Status))
    {
        /* Put the file back where it was, in the space just freed, so a
           failed rename does not lose it. */
        NTSTATUS RestoreStatus;
        WCHAR OldNameBuffer[LONGNAME_MAX_LENGTH];
        UNICODE_STRING OldName;

        /* The FCB's name buffer is rewritten by the add, so copy the name. */
        RtlInitEmptyUnicodeString(&OldName, OldNameBuffer, sizeof(OldNameBuffer));
        RtlCopyUnicodeString(&OldName, &pFcb->LongNameU);
        MoveContext.InPlace = TRUE;
        RestoreStatus = VfatAddEntry(DeviceExt, &OldName, &pFcb, OldParent,
                                     (vfatFCBIsDirectory(pFcb) ? FILE_DIRECTORY_FILE : 0),
                                     pFcb->entry.File.FileAttributes, &MoveContext);
        if (!NT_SUCCESS(RestoreStatus))
        {
            DPRINT1("exFAT: rename of '%wZ' failed (0x%08lx) and it could not be restored (0x%08lx)\n",
                    &pFcb->PathNameU, Status, RestoreStatus);
        }
    }

    ExfatFlushAllocation(DeviceExt);
    CcFlushCache(&pFcb->parentFcb->SectionObjectPointers, NULL, 0, NULL);

    return Status;
}

/* EOF */
