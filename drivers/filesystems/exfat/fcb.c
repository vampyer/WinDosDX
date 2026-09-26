/*
 * PROJECT:     VFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Routines to manipulate FCBs
 * COPYRIGHT:   Copyright 1998 Jason Filby <jasonfilby@yahoo.com>
 *              Copyright 2001 Rex Jolliff <rex@lvcablemodem.com>
 *              Copyright 2005-2022 Hervé Poussineau <hpoussin@reactos.org>
 *              Copyright 2008-2018 Pierre Schweitzer <pierre@reactos.org>
 */

/*  -------------------------------------------------------  INCLUDES  */

#include "vfat.h"

#define NDEBUG
#include <debug.h>

#ifdef __GNUC__
#include <wctype.h> /* towlower prototype */
#endif

/*  --------------------------------------------------------  DEFINES  */

#ifdef KDBG
extern UNICODE_STRING DebugFile;
#endif

/*  --------------------------------------------------------  PUBLICS  */

static
ULONG
vfatNameHash(
    ULONG hash,
    PUNICODE_STRING NameU)
{
    PWCHAR last;
    PWCHAR curr;
    WCHAR c;

    // LFN could start from "."
    //ASSERT(NameU->Buffer[0] != L'.');
    curr = NameU->Buffer;
    last = NameU->Buffer + NameU->Length / sizeof(WCHAR);

    while(curr < last)
    {
        c = towlower(*curr++);
        hash = (hash + (c << 4) + (c >> 4)) * 11;
    }
    return hash;
}

VOID
vfatSplitPathName(
    PUNICODE_STRING PathNameU,
    PUNICODE_STRING DirNameU,
    PUNICODE_STRING FileNameU)
{
    PWCHAR pName;
    USHORT Length = 0;
    pName = PathNameU->Buffer + PathNameU->Length / sizeof(WCHAR) - 1;
    while (*pName != L'\\' && pName >= PathNameU->Buffer)
    {
        pName--;
        Length++;
    }
    ASSERT(*pName == L'\\' || pName < PathNameU->Buffer);
    if (FileNameU)
    {
        FileNameU->Buffer = pName + 1;
        FileNameU->Length = FileNameU->MaximumLength = Length * sizeof(WCHAR);
    }
    if (DirNameU)
    {
        DirNameU->Buffer = PathNameU->Buffer;
        DirNameU->Length = (pName + 1 - PathNameU->Buffer) * sizeof(WCHAR);
        DirNameU->MaximumLength = DirNameU->Length;
    }
}

static
VOID
vfatInitFcb(
    PVFATFCB Fcb,
    PUNICODE_STRING NameU)
{
    USHORT PathNameBufferLength;

    if (NameU)
        PathNameBufferLength = NameU->Length + sizeof(WCHAR);
    else
        PathNameBufferLength = 0;

    Fcb->PathNameBuffer = ExAllocatePoolWithTag(NonPagedPool, PathNameBufferLength, TAG_FCB);
    if (!Fcb->PathNameBuffer)
    {
        /* FIXME: what to do if no more memory? */
        DPRINT1("Unable to initialize FCB for filename '%wZ'\n", NameU);
        KeBugCheckEx(FAT_FILE_SYSTEM, (ULONG_PTR)Fcb, (ULONG_PTR)NameU, 0, 0);
    }

    Fcb->RFCB.NodeTypeCode = NODE_TYPE_FCB;
    Fcb->RFCB.NodeByteSize = sizeof(VFATFCB);

    Fcb->PathNameU.Length = 0;
    Fcb->PathNameU.Buffer = Fcb->PathNameBuffer;
    Fcb->PathNameU.MaximumLength = PathNameBufferLength;
    Fcb->DirNameU.Buffer = Fcb->PathNameU.Buffer;
    if (NameU && NameU->Length)
    {
        RtlCopyUnicodeString(&Fcb->PathNameU, NameU);
        vfatSplitPathName(&Fcb->PathNameU, &Fcb->DirNameU, &Fcb->LongNameU);
    }
    else
    {
        Fcb->DirNameU.Buffer = Fcb->LongNameU.Buffer = NULL;
        Fcb->DirNameU.MaximumLength = Fcb->DirNameU.Length = 0;
        Fcb->LongNameU.MaximumLength = Fcb->LongNameU.Length = 0;
    }
    RtlZeroMemory(&Fcb->FCBShareAccess, sizeof(SHARE_ACCESS));
    Fcb->OpenHandleCount = 0;
}

PVFATFCB
vfatNewFCB(
    PDEVICE_EXTENSION pVCB,
    PUNICODE_STRING pFileNameU)
{
    PVFATFCB  rcFCB;

    DPRINT("'%wZ'\n", pFileNameU);

    rcFCB = ExAllocateFromNPagedLookasideList(&VfatGlobalData->FcbLookasideList);
    if (rcFCB == NULL)
    {
        return NULL;
    }
    RtlZeroMemory(rcFCB, sizeof(VFATFCB));
    vfatInitFcb(rcFCB, pFileNameU);
    UNREFERENCED_PARAMETER(pVCB);
    /* exFAT attributes are 16 bits; all the defined ones are in the low byte. */
    rcFCB->Attributes = (PUCHAR)&rcFCB->entry.File.FileAttributes;
    rcFCB->Hash.Hash = vfatNameHash(0, &rcFCB->PathNameU);
    rcFCB->Hash.self = rcFCB;
    ExInitializeResourceLite(&rcFCB->PagingIoResource);
    ExInitializeResourceLite(&rcFCB->MainResource);
    FsRtlInitializeFileLock(&rcFCB->FileLock, NULL, NULL);
    ExInitializeFastMutex(&rcFCB->LastMutex);
    rcFCB->RFCB.PagingIoResource = &rcFCB->PagingIoResource;
    rcFCB->RFCB.Resource = &rcFCB->MainResource;
    rcFCB->RFCB.IsFastIoPossible = FastIoIsNotPossible;
    InitializeListHead(&rcFCB->ParentListHead);

    return  rcFCB;
}

static
VOID
vfatDelFCBFromTable(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB pFCB)
{
    ULONG Index;
    HASHENTRY* entry;

    Index = pFCB->Hash.Hash % pVCB->HashTableSize;
    entry = pVCB->FcbHashTable[Index];
    if (entry->self == pFCB)
    {
        pVCB->FcbHashTable[Index] = entry->next;
    }
    else
    {
        while (entry->next->self != pFCB)
        {
            entry = entry->next;
        }
        entry->next = pFCB->Hash.next;
    }

    RemoveEntryList(&pFCB->FcbListEntry);
}

static
NTSTATUS
vfatMakeFullName(
    PVFATFCB directoryFCB,
    PUNICODE_STRING LongNameU,
    PUNICODE_STRING NameU)
{
    PWCHAR PathNameBuffer;
    USHORT PathNameLength;

    PathNameLength = directoryFCB->PathNameU.Length + LongNameU->Length;
    if (!vfatFCBIsRoot(directoryFCB))
    {
        PathNameLength += sizeof(WCHAR);
    }

    if (PathNameLength > LONGNAME_MAX_LENGTH * sizeof(WCHAR))
    {
        return  STATUS_OBJECT_NAME_INVALID;
    }
    PathNameBuffer = ExAllocatePoolWithTag(NonPagedPool, PathNameLength + sizeof(WCHAR), TAG_FCB);
    if (!PathNameBuffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NameU->Buffer = PathNameBuffer;
    NameU->Length = 0;
    NameU->MaximumLength = PathNameLength;

    RtlCopyUnicodeString(NameU, &directoryFCB->PathNameU);
    if (!vfatFCBIsRoot(directoryFCB))
    {
        RtlAppendUnicodeToString(NameU, L"\\");
    }
    RtlAppendUnicodeStringToString(NameU, LongNameU);
    NameU->Buffer[NameU->Length / sizeof(WCHAR)] = 0;

    return STATUS_SUCCESS;
}

VOID
vfatDestroyCCB(
    PVFATCCB pCcb)
{
    if (pCcb->SearchPattern.Buffer)
    {
        ExFreePoolWithTag(pCcb->SearchPattern.Buffer, TAG_SEARCH);
    }
    ExFreeToNPagedLookasideList(&VfatGlobalData->CcbLookasideList, pCcb);
}

VOID
vfatDestroyFCB(
    PVFATFCB pFCB)
{
#ifdef KDBG
    if (DebugFile.Buffer != NULL && FsRtlIsNameInExpression(&DebugFile, &pFCB->LongNameU, FALSE, NULL))
    {
        DPRINT1("Destroying: %p (%wZ) %d\n", pFCB, &pFCB->PathNameU, pFCB->RefCount);
    }
#endif

    FsRtlUninitializeFileLock(&pFCB->FileLock);

    if (!vfatFCBIsRoot(pFCB) &&
        !BooleanFlagOn(pFCB->Flags, FCB_IS_FAT) && !BooleanFlagOn(pFCB->Flags, FCB_IS_VOLUME))
    {
        RemoveEntryList(&pFCB->ParentListEntry);
    }
    ExFreePool(pFCB->PathNameBuffer);
    ExDeleteResourceLite(&pFCB->PagingIoResource);
    ExDeleteResourceLite(&pFCB->MainResource);
    ASSERT(IsListEmpty(&pFCB->ParentListHead));
    ExFreeToNPagedLookasideList(&VfatGlobalData->FcbLookasideList, pFCB);
}

BOOLEAN
vfatFCBIsRoot(
    PVFATFCB FCB)
{
    return FCB->PathNameU.Length == sizeof(WCHAR) && FCB->PathNameU.Buffer[0] == L'\\' ? TRUE : FALSE;
}

VOID
#ifndef KDBG
vfatGrabFCB(
#else
_vfatGrabFCB(
#endif
    PDEVICE_EXTENSION pVCB,
    PVFATFCB pFCB
#ifdef KDBG
    ,
    PCSTR File,
    ULONG Line,
    PCSTR Func
#endif
)
{
#ifdef KDBG
    if (DebugFile.Buffer != NULL && FsRtlIsNameInExpression(&DebugFile, &pFCB->LongNameU, FALSE, NULL))
    {
        DPRINT1("Inc ref count (%d, oc: %d) for: %p (%wZ) at: %s(%d) %s\n", pFCB->RefCount, pFCB->OpenHandleCount, pFCB, &pFCB->PathNameU, File, Line, Func);
    }
#else
    DPRINT("Grabbing FCB at %p: %wZ, refCount:%d\n",
           pFCB, &pFCB->PathNameU, pFCB->RefCount);
#endif

    ASSERT(ExIsResourceAcquiredExclusive(&pVCB->DirResource));

    ASSERT(!BooleanFlagOn(pFCB->Flags, FCB_IS_FAT));
    ASSERT(pFCB != pVCB->VolumeFcb && !BooleanFlagOn(pFCB->Flags, FCB_IS_VOLUME));
    ASSERT(pFCB->RefCount > 0);
    ++pFCB->RefCount;
}

VOID
#ifndef KDBG
vfatReleaseFCB(
#else
_vfatReleaseFCB(
#endif
    PDEVICE_EXTENSION pVCB,
    PVFATFCB pFCB
#ifdef KDBG
    ,
    PCSTR File,
    ULONG Line,
    PCSTR Func
#endif
)
{
    PVFATFCB tmpFcb;

#ifdef KDBG
    if (DebugFile.Buffer != NULL && FsRtlIsNameInExpression(&DebugFile, &pFCB->LongNameU, FALSE, NULL))
    {
        DPRINT1("Dec ref count (%d, oc: %d) for: %p (%wZ) at: %s(%d) %s\n", pFCB->RefCount, pFCB->OpenHandleCount, pFCB, &pFCB->PathNameU, File, Line, Func);
    }
#else
    DPRINT("Releasing FCB at %p: %wZ, refCount:%d\n",
           pFCB, &pFCB->PathNameU, pFCB->RefCount);
#endif

    ASSERT(ExIsResourceAcquiredExclusive(&pVCB->DirResource));

    while (pFCB)
    {
        ULONG RefCount;

        ASSERT(!BooleanFlagOn(pFCB->Flags, FCB_IS_FAT));
        ASSERT(pFCB != pVCB->VolumeFcb && !BooleanFlagOn(pFCB->Flags, FCB_IS_VOLUME));
        ASSERT(pFCB->RefCount > 0);
        RefCount = --pFCB->RefCount;

        if (RefCount == 1 && BooleanFlagOn(pFCB->Flags, FCB_CACHE_INITIALIZED))
        {
            PFILE_OBJECT tmpFileObject;
            tmpFileObject = pFCB->FileObject;

            pFCB->FileObject = NULL;
            CcUninitializeCacheMap(tmpFileObject, NULL, NULL);
            ClearFlag(pFCB->Flags, FCB_CACHE_INITIALIZED);
            ObDereferenceObject(tmpFileObject);
        }

        if (RefCount == 0)
        {
            ASSERT(pFCB->OpenHandleCount == 0);
            tmpFcb = pFCB->parentFcb;
            vfatDelFCBFromTable(pVCB, pFCB);
            vfatDestroyFCB(pFCB);
        }
        else
        {
            tmpFcb = NULL;
        }
        pFCB = tmpFcb;
    }
}

static
VOID
vfatAddFCBToTable(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB pFCB)
{
    ULONG Index;

    ASSERT(pFCB->Hash.Hash == vfatNameHash(0, &pFCB->PathNameU));
    Index = pFCB->Hash.Hash % pVCB->HashTableSize;

    InsertTailList(&pVCB->FcbListHead, &pFCB->FcbListEntry);

    pFCB->Hash.next = pVCB->FcbHashTable[Index];
    pVCB->FcbHashTable[Index] = &pFCB->Hash;
    if (pFCB->parentFcb)
    {
        vfatGrabFCB(pVCB, pFCB->parentFcb);
    }
}

static
VOID
vfatInitFCBFromDirEntry(
    PDEVICE_EXTENSION Vcb,
    PVFATFCB ParentFcb,
    PVFATFCB Fcb,
    PVFAT_DIRENTRY_CONTEXT DirContext)
{
    ULONGLONG DataLength;

    RtlCopyMemory(&Fcb->entry, &DirContext->DirEntry, sizeof (DIR_ENTRY));
    Fcb->Hash.Hash = vfatNameHash(0, &Fcb->PathNameU);
    ExfatChainFromEntry(Vcb, &Fcb->entry, &Fcb->Chain);

    Fcb->startIndex = DirContext->StartIndex;
    Fcb->EntryCount = DirContext->EntryCount;
    Fcb->parentFcb = ParentFcb;

    DataLength = Fcb->entry.Stream.DataLength;
    Fcb->RFCB.FileSize.QuadPart = DataLength;
    if (vfatFCBIsDirectory(Fcb))
        Fcb->RFCB.ValidDataLength.QuadPart = DataLength;
    else
        Fcb->RFCB.ValidDataLength.QuadPart = min(Fcb->entry.Stream.ValidDataLength, DataLength);
    Fcb->RFCB.AllocationSize.QuadPart = (LONGLONG)Fcb->Chain.Count * Vcb->FatInfo.BytesPerCluster;
}

NTSTATUS
vfatSetFCBNewDirName(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB Fcb,
    PVFATFCB ParentFcb)
{
    NTSTATUS Status;
    UNICODE_STRING NewNameU;

    /* Get full path name */
    Status = vfatMakeFullName(ParentFcb, &Fcb->LongNameU, &NewNameU);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Delete old name */
    if (Fcb->PathNameBuffer)
    {
        ExFreePoolWithTag(Fcb->PathNameBuffer, TAG_FCB);
    }
    Fcb->PathNameU = NewNameU;

    /* Delete from table */
    vfatDelFCBFromTable(pVCB, Fcb);

    /* Split it properly */
    Fcb->PathNameBuffer = Fcb->PathNameU.Buffer;
    Fcb->DirNameU.Buffer = Fcb->PathNameU.Buffer;
    vfatSplitPathName(&Fcb->PathNameU, &Fcb->DirNameU, &Fcb->LongNameU);
    Fcb->Hash.Hash = vfatNameHash(0, &Fcb->PathNameU);

    vfatAddFCBToTable(pVCB, Fcb);
    vfatReleaseFCB(pVCB, ParentFcb);

    return STATUS_SUCCESS;
}

NTSTATUS
vfatUpdateFCB(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB Fcb,
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PVFATFCB ParentFcb)
{
    NTSTATUS Status;
    PVFATFCB OldParent;

    DPRINT("vfatUpdateFCB(%p, %p, %p, %p)\n", pVCB, Fcb, DirContext, ParentFcb);

    /* Get full path name */
    Status = vfatMakeFullName(ParentFcb, &DirContext->LongNameU, &Fcb->PathNameU);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Delete old name */
    if (Fcb->PathNameBuffer)
    {
        ExFreePoolWithTag(Fcb->PathNameBuffer, TAG_FCB);
    }

    /* Delete from table */
    vfatDelFCBFromTable(pVCB, Fcb);

    /* Split it properly */
    Fcb->PathNameBuffer = Fcb->PathNameU.Buffer;
    Fcb->DirNameU.Buffer = Fcb->PathNameU.Buffer;
    vfatSplitPathName(&Fcb->PathNameU, &Fcb->DirNameU, &Fcb->LongNameU);

    /* Save old parent */
    OldParent = Fcb->parentFcb;
    RemoveEntryList(&Fcb->ParentListEntry);

    /* Reinit FCB */
    vfatInitFCBFromDirEntry(pVCB, ParentFcb, Fcb, DirContext);

    if (vfatFCBIsDirectory(Fcb))
    {
        ExfatFlushAllocation(pVCB);
        CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, NULL);
    }
    InsertTailList(&ParentFcb->ParentListHead, &Fcb->ParentListEntry);
    vfatAddFCBToTable(pVCB, Fcb);

    /* If we moved across directories, dereference our old parent
     * We also dereference in case we're just renaming since AddFCBToTable references it
     */
    vfatReleaseFCB(pVCB, OldParent);

    return STATUS_SUCCESS;
}

PVFATFCB
vfatGrabFCBFromTable(
    PDEVICE_EXTENSION pVCB,
    PUNICODE_STRING PathNameU)
{
    PVFATFCB  rcFCB;
    ULONG Hash;
    UNICODE_STRING DirNameU;
    UNICODE_STRING FileNameU;
    PUNICODE_STRING FcbNameU;

    HASHENTRY* entry;

    DPRINT("'%wZ'\n", PathNameU);

    ASSERT(PathNameU->Length >= sizeof(WCHAR) && PathNameU->Buffer[0] == L'\\');
    Hash = vfatNameHash(0, PathNameU);

    entry = pVCB->FcbHashTable[Hash % pVCB->HashTableSize];
    if (entry)
    {
        vfatSplitPathName(PathNameU, &DirNameU, &FileNameU);
    }

    while (entry)
    {
        if (entry->Hash == Hash)
        {
            rcFCB = entry->self;
            DPRINT("'%wZ' '%wZ'\n", &DirNameU, &rcFCB->DirNameU);
            if (RtlEqualUnicodeString(&DirNameU, &rcFCB->DirNameU, TRUE))
            {
                FcbNameU = &rcFCB->LongNameU;
                /* compare the file name */
                DPRINT("'%wZ' '%wZ'\n", &FileNameU, FcbNameU);
                if (RtlEqualUnicodeString(&FileNameU, FcbNameU, TRUE))
                {
                    vfatGrabFCB(pVCB, rcFCB);
                    return rcFCB;
                }
            }
        }
        entry = entry->next;
    }
    return NULL;
}

PVFATFCB
vfatMakeRootFCB(
    PDEVICE_EXTENSION pVCB)
{
    PVFATFCB FCB;
    ULONG CurrentCluster;
    ULONG Count = 0;
    ULONGLONG Size;
    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING NameU = RTL_CONSTANT_STRING(L"\\");

    ASSERT(pVCB->RootFcb == NULL);

    FCB = vfatNewFCB(pVCB, &NameU);
    FCB->entry.File.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;

    /* The root directory has no entry; its size is the length of its FAT chain. */
    FCB->Chain.FirstCluster = pVCB->FatInfo.RootCluster;
    FCB->Chain.NoFatChain = FALSE;
    CurrentCluster = FCB->Chain.FirstCluster;
    while (CurrentCluster != EXFAT_CLUSTER_EOF && NT_SUCCESS(Status) &&
           Count < pVCB->FatInfo.NumberOfClusters)
    {
        Count++;
        Status = NextCluster(pVCB, &FCB->Chain, &CurrentCluster, FALSE);
    }
    FCB->Chain.Count = Count;
    Size = (ULONGLONG)Count * pVCB->FatInfo.BytesPerCluster;

    FCB->entry.Stream.FirstCluster = FCB->Chain.FirstCluster;
    FCB->entry.Stream.DataLength = Size;
    FCB->entry.Stream.ValidDataLength = Size;
    FCB->RefCount = 2;
    FCB->RFCB.FileSize.QuadPart = Size;
    FCB->RFCB.ValidDataLength.QuadPart = Size;
    FCB->RFCB.AllocationSize.QuadPart = Size;
    FCB->RFCB.IsFastIoPossible = FastIoIsNotPossible;

    vfatFCBInitializeCacheFromVolume(pVCB, FCB);
    vfatAddFCBToTable(pVCB, FCB);

    /* Cache it */
    pVCB->RootFcb = FCB;

    return FCB;
}

PVFATFCB
vfatOpenRootFCB(
    PDEVICE_EXTENSION pVCB)
{
    PVFATFCB FCB;
    UNICODE_STRING NameU = RTL_CONSTANT_STRING(L"\\");

    FCB = vfatGrabFCBFromTable(pVCB, &NameU);
    if (FCB == NULL)
    {
        FCB = vfatMakeRootFCB(pVCB);
    }
    ASSERT(FCB == pVCB->RootFcb);

    return FCB;
}

NTSTATUS
vfatMakeFCBFromDirEntry(
    PVCB vcb,
    PVFATFCB directoryFCB,
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PVFATFCB *fileFCB)
{
    PVFATFCB rcFCB;
    UNICODE_STRING NameU;
    NTSTATUS Status;

    Status = vfatMakeFullName(directoryFCB, &DirContext->LongNameU, &NameU);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    rcFCB = vfatNewFCB(vcb, &NameU);
    vfatInitFCBFromDirEntry(vcb, directoryFCB, rcFCB, DirContext);

    rcFCB->RefCount = 1;
    InsertTailList(&directoryFCB->ParentListHead, &rcFCB->ParentListEntry);
    vfatAddFCBToTable(vcb, rcFCB);
    *fileFCB = rcFCB;

    ExFreePoolWithTag(NameU.Buffer, TAG_FCB);
    return STATUS_SUCCESS;
}

NTSTATUS
vfatAttachFCBToFileObject(
    PDEVICE_EXTENSION vcb,
    PVFATFCB fcb,
    PFILE_OBJECT fileObject)
{
    PVFATCCB newCCB;

#ifdef KDBG
    if (DebugFile.Buffer != NULL && FsRtlIsNameInExpression(&DebugFile, &fcb->LongNameU, FALSE, NULL))
    {
        DPRINT1("Attaching %p to %p (%d)\n", fcb, fileObject, fcb->RefCount);
    }
#endif

    newCCB = ExAllocateFromNPagedLookasideList(&VfatGlobalData->CcbLookasideList);
    if (newCCB == NULL)
    {
        return  STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(newCCB, sizeof (VFATCCB));

    fileObject->SectionObjectPointer = &fcb->SectionObjectPointers;
    fileObject->FsContext = fcb;
    fileObject->FsContext2 = newCCB;
    fileObject->Vpb = vcb->IoVPB;
    DPRINT("file open: fcb:%p PathName:%wZ\n", fcb, &fcb->PathNameU);

#ifdef KDBG
    fcb->Flags &= ~FCB_CLEANED_UP;
    fcb->Flags &= ~FCB_CLOSED;
#endif

    return STATUS_SUCCESS;
}

NTSTATUS
vfatDirFindFile(
    PDEVICE_EXTENSION pDeviceExt,
    PVFATFCB pDirectoryFCB,
    PUNICODE_STRING FileToFindU,
    PVFATFCB *pFoundFCB)
{
    NTSTATUS status;
    PVOID Context = NULL;
    PVOID Page = NULL;
    BOOLEAN First = TRUE;
    VFAT_DIRENTRY_CONTEXT DirContext;
    WCHAR LongNameBuffer[LONGNAME_MAX_LENGTH];
    USHORT NameHash;

    ASSERT(pDeviceExt);
    ASSERT(pDirectoryFCB);
    ASSERT(FileToFindU);

    DPRINT("vfatDirFindFile(VCB:%p, dirFCB:%p, File:%wZ)\n",
           pDeviceExt, pDirectoryFCB, FileToFindU);
    DPRINT("Dir Path:%wZ\n", &pDirectoryFCB->PathNameU);

    RtlZeroMemory(&DirContext, sizeof(DirContext));
    DirContext.DirIndex = 0;
    DirContext.LongNameU.Buffer = LongNameBuffer;
    DirContext.LongNameU.Length = 0;
    DirContext.LongNameU.MaximumLength = sizeof(LongNameBuffer);
    DirContext.DeviceExt = pDeviceExt;

    /* The on-disk name hash rules out most entries before names are compared. */
    NameHash = ExfatNameHash(pDeviceExt, FileToFindU);

    while (TRUE)
    {
        status = VfatGetNextDirEntry(pDeviceExt,
            &Context,
            &Page,
            pDirectoryFCB,
            &DirContext,
            First);
        First = FALSE;
        if (status == STATUS_NO_MORE_ENTRIES)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        DPRINT("  Index:%u  longName:%wZ\n",
               DirContext.DirIndex, &DirContext.LongNameU);

        if (DirContext.DirEntry.Stream.NameHash == NameHash &&
            ExfatNamesEqual(pDeviceExt, FileToFindU, &DirContext.LongNameU))
        {
            status = vfatMakeFCBFromDirEntry(pDeviceExt,
                pDirectoryFCB,
                &DirContext,
                pFoundFCB);
            CcUnpinData(Context);
            return status;
        }
        DirContext.DirIndex++;
    }
}

NTSTATUS
vfatGetFCBForFile(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB *pParentFCB,
    PVFATFCB *pFCB,
    PUNICODE_STRING pFileNameU)
{
    NTSTATUS status;
    PVFATFCB FCB = NULL;
    PVFATFCB parentFCB;
    UNICODE_STRING NameU;
    UNICODE_STRING RootNameU = RTL_CONSTANT_STRING(L"\\");
    UNICODE_STRING FileNameU;
    WCHAR NameBuffer[260];
    PWCHAR curr, prev, last;
    ULONG Length;

    DPRINT("vfatGetFCBForFile (%p,%p,%p,%wZ)\n",
           pVCB, pParentFCB, pFCB, pFileNameU);

    RtlInitEmptyUnicodeString(&FileNameU, NameBuffer, sizeof(NameBuffer));

    parentFCB = *pParentFCB;

    if (parentFCB == NULL)
    {
        /* Passed-in name is the full name */
        RtlCopyUnicodeString(&FileNameU, pFileNameU);

        //  Trivial case, open of the root directory on volume
        if (RtlEqualUnicodeString(&FileNameU, &RootNameU, FALSE))
        {
            DPRINT("returning root FCB\n");

            FCB = vfatOpenRootFCB(pVCB);
            *pFCB = FCB;
            *pParentFCB = NULL;

            return (FCB != NULL) ? STATUS_SUCCESS : STATUS_OBJECT_PATH_NOT_FOUND;
        }

        /* Check for an existing FCB */
        FCB = vfatGrabFCBFromTable(pVCB, &FileNameU);
        if (FCB)
        {
            *pFCB = FCB;
            *pParentFCB = FCB->parentFcb;
            vfatGrabFCB(pVCB, *pParentFCB);
            return STATUS_SUCCESS;
        }

        last = curr = FileNameU.Buffer + FileNameU.Length / sizeof(WCHAR) - 1;
        while (*curr != L'\\' && curr > FileNameU.Buffer)
        {
            curr--;
        }

        if (curr > FileNameU.Buffer)
        {
            NameU.Buffer = FileNameU.Buffer;
            NameU.MaximumLength = NameU.Length = (curr - FileNameU.Buffer) * sizeof(WCHAR);
            FCB = vfatGrabFCBFromTable(pVCB, &NameU);
            if (FCB)
            {
                Length = (curr - FileNameU.Buffer) * sizeof(WCHAR);
                if (Length != FCB->PathNameU.Length)
                {
                    if (FileNameU.Length + FCB->PathNameU.Length - Length > FileNameU.MaximumLength)
                    {
                        vfatReleaseFCB(pVCB, FCB);
                        return STATUS_OBJECT_NAME_INVALID;
                    }
                    RtlMoveMemory(FileNameU.Buffer + FCB->PathNameU.Length / sizeof(WCHAR),
                        curr, FileNameU.Length - Length);
                    FileNameU.Length += (USHORT)(FCB->PathNameU.Length - Length);
                    curr = FileNameU.Buffer + FCB->PathNameU.Length / sizeof(WCHAR);
                    last = FileNameU.Buffer + FileNameU.Length / sizeof(WCHAR) - 1;
                }
                RtlCopyMemory(FileNameU.Buffer, FCB->PathNameU.Buffer, FCB->PathNameU.Length);
            }
        }
        else
        {
            FCB = NULL;
        }

        if (FCB == NULL)
        {
            FCB = vfatOpenRootFCB(pVCB);
            curr = FileNameU.Buffer;
        }

        parentFCB = NULL;
        prev = curr;
    }
    else
    {
        /* Make absolute path */
        RtlCopyUnicodeString(&FileNameU, &parentFCB->PathNameU);
        curr = FileNameU.Buffer + FileNameU.Length / sizeof(WCHAR) - 1;
        if (*curr != L'\\')
        {
            RtlAppendUnicodeToString(&FileNameU, L"\\");
            curr++;
        }
        ASSERT(*curr == L'\\');
        RtlAppendUnicodeStringToString(&FileNameU, pFileNameU);

        FCB = parentFCB;
        parentFCB = NULL;
        prev = curr;
        last = FileNameU.Buffer + FileNameU.Length / sizeof(WCHAR) - 1;
    }

    while (curr <= last)
    {
        if (parentFCB)
        {
            vfatReleaseFCB(pVCB, parentFCB);
            parentFCB = NULL;
        }
        //  fail if element in FCB is not a directory
        if (!vfatFCBIsDirectory(FCB))
        {
            DPRINT ("Element in requested path is not a directory\n");

            vfatReleaseFCB(pVCB, FCB);
            FCB = NULL;
            *pParentFCB = NULL;
            *pFCB = NULL;

            return  STATUS_OBJECT_PATH_NOT_FOUND;
        }
        parentFCB = FCB;
        if (prev < curr)
        {
            Length = (curr - prev) * sizeof(WCHAR);
            if (Length != parentFCB->LongNameU.Length)
            {
                if (FileNameU.Length + parentFCB->LongNameU.Length - Length > FileNameU.MaximumLength)
                {
                    vfatReleaseFCB(pVCB, parentFCB);
                    *pParentFCB = NULL;
                    *pFCB = NULL;
                    return STATUS_OBJECT_NAME_INVALID;
                }
                RtlMoveMemory(prev + parentFCB->LongNameU.Length / sizeof(WCHAR), curr,
                    FileNameU.Length - (curr - FileNameU.Buffer) * sizeof(WCHAR));
                FileNameU.Length += (USHORT)(parentFCB->LongNameU.Length - Length);
                curr = prev + parentFCB->LongNameU.Length / sizeof(WCHAR);
                last = FileNameU.Buffer + FileNameU.Length / sizeof(WCHAR) - 1;
            }
            RtlCopyMemory(prev, parentFCB->LongNameU.Buffer, parentFCB->LongNameU.Length);
        }
        curr++;
        prev = curr;
        while (*curr != L'\\' && curr <= last)
        {
            curr++;
        }
        NameU.Buffer = FileNameU.Buffer;
        NameU.Length = (curr - NameU.Buffer) * sizeof(WCHAR);
        NameU.MaximumLength = FileNameU.MaximumLength;
        DPRINT("%wZ\n", &NameU);
        FCB = vfatGrabFCBFromTable(pVCB, &NameU);
        if (FCB == NULL)
        {
            NameU.Buffer = prev;
            NameU.MaximumLength = NameU.Length = (curr - prev) * sizeof(WCHAR);
            status = vfatDirFindFile(pVCB, parentFCB, &NameU, &FCB);
            if (status == STATUS_OBJECT_NAME_NOT_FOUND)
            {
                *pFCB = NULL;
                if (curr > last)
                {
                    *pParentFCB = parentFCB;
                    return STATUS_OBJECT_NAME_NOT_FOUND;
                }
                else
                {
                    vfatReleaseFCB(pVCB, parentFCB);
                    *pParentFCB = NULL;
                    return STATUS_OBJECT_PATH_NOT_FOUND;
                }
            }
            else if (!NT_SUCCESS(status))
            {
                vfatReleaseFCB(pVCB, parentFCB);
                *pParentFCB = NULL;
                *pFCB = NULL;

                return status;
            }
        }
    }

    *pParentFCB = parentFCB;
    *pFCB = FCB;

    return STATUS_SUCCESS;
}
