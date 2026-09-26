/*
 * PROJECT:     VFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Directory control
 * COPYRIGHT:   Copyright 1998 Jason Filby <jasonfilby@yahoo.com>
 *              Copyright 2004-2005 Hervé Poussineau <hpoussin@reactos.org>
 *              Copyright 2012-2018 Pierre Schweitzer <pierre@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "vfat.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* Times, sizes and attributes of an entry, as the directory information
   classes report them. Directories report no size, as on FAT. */
static
VOID
ExfatGetEntryInfo(
    PDEVICE_EXTENSION DeviceExt,
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PLARGE_INTEGER CreationTime,
    PLARGE_INTEGER LastAccessTime,
    PLARGE_INTEGER LastWriteTime,
    PLARGE_INTEGER ChangeTime,
    PLARGE_INTEGER EndOfFile,
    PLARGE_INTEGER AllocationSize,
    PULONG FileAttributes)
{
    PEXFAT_FILE_ENTRY File = &DirContext->DirEntry.File;

    ExfatTimestampToSystemTime(File->CreateTimestamp, File->Create10msIncrement,
                               File->CreateUtcOffset, CreationTime);
    ExfatTimestampToSystemTime(File->LastAccessedTimestamp, 0,
                               File->LastAccessedUtcOffset, LastAccessTime);
    ExfatTimestampToSystemTime(File->LastModifiedTimestamp, File->LastModified10msIncrement,
                               File->LastModifiedUtcOffset, LastWriteTime);
    *ChangeTime = *LastWriteTime;

    if (BooleanFlagOn(File->FileAttributes, FILE_ATTRIBUTE_DIRECTORY))
    {
        EndOfFile->QuadPart = 0;
        AllocationSize->QuadPart = 0;
    }
    else
    {
        EndOfFile->QuadPart = DirContext->DirEntry.Stream.DataLength;
        AllocationSize->QuadPart = ROUND_UP_64(DirContext->DirEntry.Stream.DataLength,
                                               DeviceExt->FatInfo.BytesPerCluster);
    }

    *FileAttributes = File->FileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                              FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY |
                                              FILE_ATTRIBUTE_ARCHIVE);
    if (*FileAttributes == 0)
        *FileAttributes = FILE_ATTRIBUTE_NORMAL;
}

#define ULONG_ROUND_UP(x)   ROUND_UP((x), (sizeof(ULONG)))

static
NTSTATUS
VfatGetFileNamesInformation(
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PFILE_NAMES_INFORMATION pInfo,
    ULONG BufferLength,
    PULONG Written,
    BOOLEAN First)
{
    NTSTATUS Status;
    ULONG BytesToCopy = 0;

    *Written = 0;
    Status = STATUS_BUFFER_OVERFLOW;

    if (FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) > BufferLength)
        return Status;

    if (First || (BufferLength > FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) + DirContext->LongNameU.Length))
    {
        pInfo->FileNameLength = DirContext->LongNameU.Length;

        *Written = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
        pInfo->NextEntryOffset = 0;
        if (BufferLength > FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName))
        {
            BytesToCopy = min(DirContext->LongNameU.Length, BufferLength - FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName));
            RtlCopyMemory(pInfo->FileName,
                         DirContext->LongNameU.Buffer,
                         BytesToCopy);
            *Written += BytesToCopy;

            if (BytesToCopy == DirContext->LongNameU.Length)
            {
                pInfo->NextEntryOffset = ULONG_ROUND_UP(sizeof(FILE_NAMES_INFORMATION) +
                                                        BytesToCopy);
                Status = STATUS_SUCCESS;
            }
        }
    }

    return Status;
}

static
NTSTATUS
VfatGetFileDirectoryInformation(
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_DIRECTORY_INFORMATION pInfo,
    ULONG BufferLength,
    PULONG Written,
    BOOLEAN First)
{
    NTSTATUS Status;
    ULONG BytesToCopy = 0;

    *Written = 0;
    Status = STATUS_BUFFER_OVERFLOW;

    if (FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) > BufferLength)
        return Status;

    if (First || (BufferLength > FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) + DirContext->LongNameU.Length))
    {
        pInfo->FileNameLength = DirContext->LongNameU.Length;
        /* pInfo->FileIndex = ; */

        *Written = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
        pInfo->NextEntryOffset = 0;
        if (BufferLength > FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName))
        {
            BytesToCopy = min(DirContext->LongNameU.Length, BufferLength - FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName));
            RtlCopyMemory(pInfo->FileName,
                         DirContext->LongNameU.Buffer,
                         BytesToCopy);
            *Written += BytesToCopy;

            if (BytesToCopy == DirContext->LongNameU.Length)
            {
                pInfo->NextEntryOffset = ULONG_ROUND_UP(sizeof(FILE_DIRECTORY_INFORMATION) +
                                                        BytesToCopy);
                Status = STATUS_SUCCESS;
            }
        }



        ExfatGetEntryInfo(DeviceExt, DirContext, &pInfo->CreationTime, &pInfo->LastAccessTime,
                          &pInfo->LastWriteTime, &pInfo->ChangeTime, &pInfo->EndOfFile,
                          &pInfo->AllocationSize, &pInfo->FileAttributes);
    }

    return Status;
}

static
NTSTATUS
VfatGetFileFullDirectoryInformation(
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_FULL_DIR_INFORMATION pInfo,
    ULONG BufferLength,
    PULONG Written,
    BOOLEAN First)
{
    NTSTATUS Status;
    ULONG BytesToCopy = 0;

    *Written = 0;
    Status = STATUS_BUFFER_OVERFLOW;

    if (FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) > BufferLength)
        return Status;

    if (First || (BufferLength > FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) + DirContext->LongNameU.Length))
    {
        pInfo->FileNameLength = DirContext->LongNameU.Length;
        /* pInfo->FileIndex = ; */
        pInfo->EaSize = 0;

        *Written = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
        pInfo->NextEntryOffset = 0;
        if (BufferLength > FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName))
        {
            BytesToCopy = min(DirContext->LongNameU.Length, BufferLength - FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName));
            RtlCopyMemory(pInfo->FileName,
                         DirContext->LongNameU.Buffer,
                         BytesToCopy);
            *Written += BytesToCopy;

            if (BytesToCopy == DirContext->LongNameU.Length)
            {
                pInfo->NextEntryOffset = ULONG_ROUND_UP(sizeof(FILE_FULL_DIR_INFORMATION) +
                                                        BytesToCopy);
                Status = STATUS_SUCCESS;
            }
        }

        ExfatGetEntryInfo(DeviceExt, DirContext, &pInfo->CreationTime, &pInfo->LastAccessTime,
                          &pInfo->LastWriteTime, &pInfo->ChangeTime, &pInfo->EndOfFile,
                          &pInfo->AllocationSize, &pInfo->FileAttributes);
    }

    return Status;
}

static
NTSTATUS
VfatGetFileBothInformation(
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_BOTH_DIR_INFORMATION pInfo,
    ULONG BufferLength,
    PULONG Written,
    BOOLEAN First)
{
    NTSTATUS Status;
    ULONG BytesToCopy = 0;

    *Written = 0;
    Status = STATUS_BUFFER_OVERFLOW;

    if (FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) > BufferLength)
        return Status;

    if (First || (BufferLength > FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) + DirContext->LongNameU.Length))
    {
        pInfo->FileNameLength = DirContext->LongNameU.Length;
        pInfo->EaSize = 0;

        *Written = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
        pInfo->NextEntryOffset = 0;
        if (BufferLength > FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName))
        {
            BytesToCopy = min(DirContext->LongNameU.Length, BufferLength - FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName));
            RtlCopyMemory(pInfo->FileName,
                         DirContext->LongNameU.Buffer,
                         BytesToCopy);
            *Written += BytesToCopy;

            if (BytesToCopy == DirContext->LongNameU.Length)
            {
                pInfo->NextEntryOffset = ULONG_ROUND_UP(sizeof(FILE_BOTH_DIR_INFORMATION) +
                                                        BytesToCopy);
                Status = STATUS_SUCCESS;
            }
        }

        /* exFAT has no short names */
        pInfo->ShortName[0] = 0;
        pInfo->ShortNameLength = 0;
        ExfatGetEntryInfo(DeviceExt, DirContext, &pInfo->CreationTime, &pInfo->LastAccessTime,
                          &pInfo->LastWriteTime, &pInfo->ChangeTime, &pInfo->EndOfFile,
                          &pInfo->AllocationSize, &pInfo->FileAttributes);
    }

    return Status;
}

static
NTSTATUS
DoQuery(
    PVFAT_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG BufferLength = 0;
    PUNICODE_STRING pSearchPattern = NULL;
    FILE_INFORMATION_CLASS FileInformationClass;
    PUCHAR Buffer = NULL;
    PFILE_NAMES_INFORMATION Buffer0 = NULL;
    PVFATFCB pFcb;
    PVFATCCB pCcb;
    BOOLEAN FirstQuery = FALSE;
    BOOLEAN FirstCall = TRUE;
    VFAT_DIRENTRY_CONTEXT DirContext;
    WCHAR LongNameBuffer[LONGNAME_MAX_LENGTH + 1];
    ULONG Written;
    BOOLEAN IsDot;

    PIO_STACK_LOCATION Stack = IrpContext->Stack;

    pCcb = (PVFATCCB)IrpContext->FileObject->FsContext2;
    pFcb = (PVFATFCB)IrpContext->FileObject->FsContext;

    /* Determine Buffer for result : */
    BufferLength = Stack->Parameters.QueryDirectory.Length;
#if 0
    /* Do not probe the user buffer until SEH is available */
    if (IrpContext->Irp->RequestorMode != KernelMode &&
        IrpContext->Irp->MdlAddress == NULL &&
        IrpContext->Irp->UserBuffer != NULL)
    {
        ProbeForWrite(IrpContext->Irp->UserBuffer, BufferLength, 1);
    }
#endif
    Buffer = VfatGetUserBuffer(IrpContext->Irp, FALSE);

    if (!ExAcquireResourceExclusiveLite(&IrpContext->DeviceExt->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        Status = VfatLockUserBuffer(IrpContext->Irp, BufferLength, IoWriteAccess);
        if (NT_SUCCESS(Status))
            Status = STATUS_PENDING;

        return Status;
    }

    if (!ExAcquireResourceSharedLite(&pFcb->MainResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        ExReleaseResourceLite(&IrpContext->DeviceExt->DirResource);
        Status = VfatLockUserBuffer(IrpContext->Irp, BufferLength, IoWriteAccess);
        if (NT_SUCCESS(Status))
            Status = STATUS_PENDING;

        return Status;
    }

    /* Obtain the callers parameters */
    pSearchPattern = (PUNICODE_STRING)Stack->Parameters.QueryDirectory.FileName;
    FileInformationClass = Stack->Parameters.QueryDirectory.FileInformationClass;

    /* Allocate search pattern in case:
     * -> We don't have one already in context
     * -> We have been given an input pattern
     * -> The pattern length is not null
     * -> The pattern buffer is not null
     * Otherwise, we'll fall later and allocate a match all (*) pattern
     */
    if (pSearchPattern &&
        pSearchPattern->Length != 0 && pSearchPattern->Buffer != NULL)
    {
        if (!pCcb->SearchPattern.Buffer)
        {
            FirstQuery = TRUE;
            pCcb->SearchPattern.MaximumLength = pSearchPattern->Length + sizeof(WCHAR);
            pCcb->SearchPattern.Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                                               pCcb->SearchPattern.MaximumLength,
                                                               TAG_SEARCH);
            if (!pCcb->SearchPattern.Buffer)
            {
                ExReleaseResourceLite(&pFcb->MainResource);
                ExReleaseResourceLite(&IrpContext->DeviceExt->DirResource);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            RtlCopyUnicodeString(&pCcb->SearchPattern, pSearchPattern);
            pCcb->SearchPattern.Buffer[pCcb->SearchPattern.Length / sizeof(WCHAR)] = 0;
        }
    }
    else if (!pCcb->SearchPattern.Buffer)
    {
        FirstQuery = TRUE;
        pCcb->SearchPattern.MaximumLength = 2 * sizeof(WCHAR);
        pCcb->SearchPattern.Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                                           2 * sizeof(WCHAR),
                                                           TAG_SEARCH);
        if (!pCcb->SearchPattern.Buffer)
        {
            ExReleaseResourceLite(&pFcb->MainResource);
            ExReleaseResourceLite(&IrpContext->DeviceExt->DirResource);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        pCcb->SearchPattern.Buffer[0] = L'*';
        pCcb->SearchPattern.Buffer[1] = 0;
        pCcb->SearchPattern.Length = sizeof(WCHAR);
    }

    RtlZeroMemory(&DirContext, sizeof(DirContext));
    if (BooleanFlagOn(IrpContext->Stack->Flags, SL_INDEX_SPECIFIED))
    {
        DirContext.DirIndex = pCcb->Entry = Stack->Parameters.QueryDirectory.FileIndex;
        pCcb->DotsReturned = 2;
    }
    else if (FirstQuery || BooleanFlagOn(IrpContext->Stack->Flags, SL_RESTART_SCAN))
    {
        DirContext.DirIndex = pCcb->Entry = 0;
        pCcb->DotsReturned = 0;
    }
    else
    {
        DirContext.DirIndex = pCcb->Entry;
    }

    DPRINT("Buffer=%p tofind=%wZ\n", Buffer, &pCcb->SearchPattern);

    DirContext.DeviceExt = IrpContext->DeviceExt;
    DirContext.LongNameU.Buffer = LongNameBuffer;
    DirContext.LongNameU.MaximumLength = sizeof(LongNameBuffer);

    Written = 0;
    while ((Status == STATUS_SUCCESS) && (BufferLength > 0))
    {
        IsDot = FALSE;
        if (!vfatFCBIsRoot(pFcb) && pCcb->DotsReturned < 2)
        {
            /* exFAT directories hold no "." and ".." entries; report them
               first, as Windows does, with the directories' own details. */
            PVFATFCB DotFcb = pFcb;

            DirContext.LongNameU.Length = (USHORT)((pCcb->DotsReturned + 1) * sizeof(WCHAR));
            DirContext.LongNameU.Buffer[0] = L'.';
            DirContext.LongNameU.Buffer[1] = L'.';
            if (pCcb->DotsReturned == 1 && pFcb->parentFcb && !vfatFCBIsRoot(pFcb->parentFcb))
                DotFcb = pFcb->parentFcb;
            if (!FsRtlIsNameInExpression(&pCcb->SearchPattern, &DirContext.LongNameU, TRUE, NULL))
            {
                pCcb->DotsReturned++;
                continue;
            }
            DirContext.DirEntry = DotFcb->entry;
            IsDot = TRUE;
            Status = STATUS_SUCCESS;
        }
        else
        {
            Status = FindFile(IrpContext->DeviceExt,
                              pFcb,
                              &pCcb->SearchPattern,
                              &DirContext,
                              FirstCall);
            /* If this entry does not fit in the caller's buffer, the next
               query resumes here: at the File entry that starts its set.
               DirIndex is the set's last entry, which a resumed scan would
               skip as a stray secondary entry. */
            pCcb->Entry = NT_SUCCESS(Status) ? DirContext.StartIndex : DirContext.DirIndex;
            FirstCall = FALSE;
        }

        DPRINT("Found %wZ, Status=%x, entry %x\n", &DirContext.LongNameU, Status, pCcb->Entry);
        if (NT_SUCCESS(Status))
        {
            switch (FileInformationClass)
            {
                case FileDirectoryInformation:
                    Status = VfatGetFileDirectoryInformation(&DirContext,
                                                             IrpContext->DeviceExt,
                                                             (PFILE_DIRECTORY_INFORMATION)Buffer,
                                                             BufferLength,
                                                             &Written,
                                                             Buffer0 == NULL);
                    break;

                case FileFullDirectoryInformation:
                    Status = VfatGetFileFullDirectoryInformation(&DirContext,
                                                                 IrpContext->DeviceExt,
                                                                 (PFILE_FULL_DIR_INFORMATION)Buffer,
                                                                 BufferLength,
                                                                 &Written,
                                                                 Buffer0 == NULL);
                    break;

                case FileBothDirectoryInformation:
                    Status = VfatGetFileBothInformation(&DirContext,
                                                        IrpContext->DeviceExt,
                                                        (PFILE_BOTH_DIR_INFORMATION)Buffer,
                                                        BufferLength,
                                                        &Written,
                                                        Buffer0 == NULL);
                    break;

                case FileNamesInformation:
                    Status = VfatGetFileNamesInformation(&DirContext,
                                                         (PFILE_NAMES_INFORMATION)Buffer,
                                                         BufferLength,
                                                         &Written,
                                                         Buffer0 == NULL);
                     break;

                default:
                    Status = STATUS_INVALID_INFO_CLASS;
                    break;
            }

            if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_INVALID_INFO_CLASS)
                break;
        }
        else
        {
            Status = (FirstQuery ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES);
            break;
        }

        Buffer0 = (PFILE_NAMES_INFORMATION) Buffer;
        if (IsDot)
        {
            Buffer0->FileIndex = 0;
            pCcb->DotsReturned++;
        }
        else
        {
            Buffer0->FileIndex = DirContext.DirIndex;
            pCcb->Entry = ++DirContext.DirIndex;
        }
        BufferLength -= Buffer0->NextEntryOffset;

        if (BooleanFlagOn(IrpContext->Stack->Flags, SL_RETURN_SINGLE_ENTRY))
            break;

        Buffer += Buffer0->NextEntryOffset;
    }

    if (Buffer0)
    {
        Buffer0->NextEntryOffset = 0;
        Status = STATUS_SUCCESS;
        IrpContext->Irp->IoStatus.Information = Stack->Parameters.QueryDirectory.Length - BufferLength;
    }
    else
    {
        ASSERT(Status != STATUS_SUCCESS || BufferLength == 0);
        ASSERT(Written <= Stack->Parameters.QueryDirectory.Length);
        IrpContext->Irp->IoStatus.Information = Written;
    }

    ExReleaseResourceLite(&pFcb->MainResource);
    ExReleaseResourceLite(&IrpContext->DeviceExt->DirResource);

    return Status;
}

NTSTATUS VfatNotifyChangeDirectory(PVFAT_IRP_CONTEXT IrpContext)
{
    PVCB pVcb;
    PVFATFCB pFcb;
    PIO_STACK_LOCATION Stack;
    Stack = IrpContext->Stack;
    pVcb = IrpContext->DeviceExt;
    pFcb = (PVFATFCB) IrpContext->FileObject->FsContext;

    FsRtlNotifyFullChangeDirectory(pVcb->NotifySync,
                                   &(pVcb->NotifyList),
                                   IrpContext->FileObject->FsContext2,
                                   (PSTRING)&(pFcb->PathNameU),
                                   BooleanFlagOn(Stack->Flags, SL_WATCH_TREE),
                                   FALSE,
                                   Stack->Parameters.NotifyDirectory.CompletionFilter,
                                   IrpContext->Irp,
                                   NULL,
                                   NULL);

    /* We won't handle IRP completion */
    IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;

    return STATUS_PENDING;
}

/*
 * FUNCTION: directory control : read/write directory informations
 */
NTSTATUS
VfatDirectoryControl(
    PVFAT_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_SUCCESS;

    IrpContext->Irp->IoStatus.Information = 0;

    switch (IrpContext->MinorFunction)
    {
        case IRP_MN_QUERY_DIRECTORY:
            Status = DoQuery (IrpContext);
            break;

        case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
            Status = VfatNotifyChangeDirectory(IrpContext);
            break;

        default:
            /* Error */
            DPRINT("Unexpected minor function %x in VFAT driver\n",
                   IrpContext->MinorFunction);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    if (Status == STATUS_PENDING && BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_COMPLETE))
    {
        return VfatMarkIrpContextForQueue(IrpContext);
    }

    return Status;
}

/* EOF */
