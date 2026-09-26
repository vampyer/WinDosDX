/*
 * PROJECT:     VFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     File information routines
 * COPYRIGHT:   Copyright 1998 Jason Filby <jasonfilby@yahoo.com>
 *              Copyright 2005 Hervé Poussineau <hpoussin@reactos.org>
 *              Copyright 2008-2018 Pierre Schweitzer <pierre@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "vfat.h"

#define NDEBUG
#include <debug.h>

#define NASSERTS_RENAME

/* GLOBALS ******************************************************************/

const char* FileInformationClassNames[] =
{
    "??????",
    "FileDirectoryInformation",
    "FileFullDirectoryInformation",
    "FileBothDirectoryInformation",
    "FileBasicInformation",
    "FileStandardInformation",
    "FileInternalInformation",
    "FileEaInformation",
    "FileAccessInformation",
    "FileNameInformation",
    "FileRenameInformation",
    "FileLinkInformation",
    "FileNamesInformation",
    "FileDispositionInformation",
    "FilePositionInformation",
    "FileFullEaInformation",
    "FileModeInformation",
    "FileAlignmentInformation",
    "FileAllInformation",
    "FileAllocationInformation",
    "FileEndOfFileInformation",
    "FileAlternateNameInformation",
    "FileStreamInformation",
    "FilePipeInformation",
    "FilePipeLocalInformation",
    "FilePipeRemoteInformation",
    "FileMailslotQueryInformation",
    "FileMailslotSetInformation",
    "FileCompressionInformation",
    "FileObjectIdInformation",
    "FileCompletionInformation",
    "FileMoveClusterInformation",
    "FileQuotaInformation",
    "FileReparsePointInformation",
    "FileNetworkOpenInformation",
    "FileAttributeTagInformation",
    "FileTrackingInformation",
    "FileIdBothDirectoryInformation",
    "FileIdFullDirectoryInformation",
    "FileValidDataLengthInformation",
    "FileShortNameInformation",
    "FileMaximumInformation"
};

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Retrieve the standard file information
 */
NTSTATUS
VfatGetStandardInformation(
    PVFATFCB FCB,
    PFILE_STANDARD_INFORMATION StandardInfo,
    PULONG BufferLength)
{
    if (*BufferLength < sizeof(FILE_STANDARD_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    /* PRECONDITION */
    ASSERT(StandardInfo != NULL);
    ASSERT(FCB != NULL);

    if (vfatFCBIsDirectory(FCB))
    {
        StandardInfo->AllocationSize.QuadPart = 0;
        StandardInfo->EndOfFile.QuadPart = 0;
        StandardInfo->Directory = TRUE;
    }
    else
    {
        StandardInfo->AllocationSize = FCB->RFCB.AllocationSize;
        StandardInfo->EndOfFile = FCB->RFCB.FileSize;
        StandardInfo->Directory = FALSE;
    }
    StandardInfo->NumberOfLinks = 1;
    StandardInfo->DeletePending = BooleanFlagOn(FCB->Flags, FCB_DELETE_PENDING);

    *BufferLength -= sizeof(FILE_STANDARD_INFORMATION);
    return STATUS_SUCCESS;
}

static
NTSTATUS
VfatSetPositionInformation(
    PFILE_OBJECT FileObject,
    PFILE_POSITION_INFORMATION PositionInfo)
{
    DPRINT("FsdSetPositionInformation()\n");

    DPRINT("PositionInfo %p\n", PositionInfo);
    DPRINT("Setting position %I64u\n", PositionInfo->CurrentByteOffset.QuadPart);

    FileObject->CurrentByteOffset.QuadPart =
        PositionInfo->CurrentByteOffset.QuadPart;

    return STATUS_SUCCESS;
}

static
NTSTATUS
VfatGetPositionInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB FCB,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_POSITION_INFORMATION PositionInfo,
    PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FCB);
    UNREFERENCED_PARAMETER(DeviceExt);

    DPRINT("VfatGetPositionInformation()\n");

    if (*BufferLength < sizeof(FILE_POSITION_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    PositionInfo->CurrentByteOffset.QuadPart =
        FileObject->CurrentByteOffset.QuadPart;

    DPRINT("Getting position %I64x\n",
           PositionInfo->CurrentByteOffset.QuadPart);

    *BufferLength -= sizeof(FILE_POSITION_INFORMATION);
    return STATUS_SUCCESS;
}

static
NTSTATUS
VfatSetBasicInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB FCB,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_BASIC_INFORMATION BasicInfo)
{
    ULONG NotifyFilter;
    NTSTATUS Status;

    DPRINT("VfatSetBasicInformation()\n");

    ASSERT(NULL != FileObject);
    ASSERT(NULL != FCB);
    ASSERT(NULL != DeviceExt);
    ASSERT(NULL != BasicInfo);

    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        return STATUS_MEDIA_WRITE_PROTECTED;

    NotifyFilter = 0;

    if (BasicInfo->FileAttributes != 0)
    {
        UCHAR Attributes;

        Attributes = (BasicInfo->FileAttributes & (FILE_ATTRIBUTE_ARCHIVE |
                                                   FILE_ATTRIBUTE_SYSTEM |
                                                   FILE_ATTRIBUTE_HIDDEN |
                                                   FILE_ATTRIBUTE_DIRECTORY |
                                                   FILE_ATTRIBUTE_READONLY));

        if (vfatFCBIsDirectory(FCB))
        {
            if (BooleanFlagOn(BasicInfo->FileAttributes, FILE_ATTRIBUTE_TEMPORARY))
            {
                DPRINT("Setting temporary attribute on a directory!\n");
                return STATUS_INVALID_PARAMETER;
            }

            Attributes |= FILE_ATTRIBUTE_DIRECTORY;
        }
        else
        {
            if (BooleanFlagOn(BasicInfo->FileAttributes, FILE_ATTRIBUTE_DIRECTORY))
            {
                DPRINT("Setting directory attribute on a file!\n");
                return STATUS_INVALID_PARAMETER;
            }
        }

        if (Attributes != *FCB->Attributes)
        {
            *FCB->Attributes = Attributes;
            DPRINT("Setting attributes 0x%02x\n", *FCB->Attributes);
            NotifyFilter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
        }
    }

    if (BasicInfo->CreationTime.QuadPart != 0 && BasicInfo->CreationTime.QuadPart != -1)
    {
        ExfatSystemTimeToTimestamp(&BasicInfo->CreationTime,
                                   &FCB->entry.File.CreateTimestamp,
                                   &FCB->entry.File.Create10msIncrement,
                                   &FCB->entry.File.CreateUtcOffset);
        NotifyFilter |= FILE_NOTIFY_CHANGE_CREATION;
    }

    if (BasicInfo->LastAccessTime.QuadPart != 0 && BasicInfo->LastAccessTime.QuadPart != -1)
    {
        UCHAR Unused;

        /* exFAT keeps no sub-second part for the access time. */
        ExfatSystemTimeToTimestamp(&BasicInfo->LastAccessTime,
                                   &FCB->entry.File.LastAccessedTimestamp,
                                   &Unused,
                                   &FCB->entry.File.LastAccessedUtcOffset);
        NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
    }

    if (BasicInfo->LastWriteTime.QuadPart != 0 && BasicInfo->LastWriteTime.QuadPart != -1)
    {
        ExfatSystemTimeToTimestamp(&BasicInfo->LastWriteTime,
                                   &FCB->entry.File.LastModifiedTimestamp,
                                   &FCB->entry.File.LastModified10msIncrement,
                                   &FCB->entry.File.LastModifiedUtcOffset);
        NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
    }

    Status = VfatUpdateEntry(DeviceExt, FCB);
    if (!NT_SUCCESS(Status))
        return Status;

    if (NotifyFilter != 0)
    {
        vfatReportChange(DeviceExt,
                         FCB,
                         NotifyFilter,
                         FILE_ACTION_MODIFIED);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
VfatGetBasicInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB FCB,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_BASIC_INFORMATION BasicInfo,
    PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(FileObject);

    DPRINT("VfatGetBasicInformation()\n");

    if (*BufferLength < sizeof(FILE_BASIC_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    RtlZeroMemory(BasicInfo, sizeof(FILE_BASIC_INFORMATION));

    UNREFERENCED_PARAMETER(DeviceExt);
    ExfatTimestampToSystemTime(FCB->entry.File.CreateTimestamp,
                               FCB->entry.File.Create10msIncrement,
                               FCB->entry.File.CreateUtcOffset,
                               &BasicInfo->CreationTime);
    ExfatTimestampToSystemTime(FCB->entry.File.LastAccessedTimestamp, 0,
                               FCB->entry.File.LastAccessedUtcOffset,
                               &BasicInfo->LastAccessTime);
    ExfatTimestampToSystemTime(FCB->entry.File.LastModifiedTimestamp,
                               FCB->entry.File.LastModified10msIncrement,
                               FCB->entry.File.LastModifiedUtcOffset,
                               &BasicInfo->LastWriteTime);
    BasicInfo->ChangeTime = BasicInfo->LastWriteTime;

    BasicInfo->FileAttributes = *FCB->Attributes & 0x3f;
    /* Synthesize FILE_ATTRIBUTE_NORMAL */
    if (0 == (BasicInfo->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                           FILE_ATTRIBUTE_ARCHIVE |
                                           FILE_ATTRIBUTE_SYSTEM |
                                           FILE_ATTRIBUTE_HIDDEN |
                                           FILE_ATTRIBUTE_READONLY)))
    {
        DPRINT("Synthesizing FILE_ATTRIBUTE_NORMAL\n");
        BasicInfo->FileAttributes |= FILE_ATTRIBUTE_NORMAL;
    }
    DPRINT("Getting attributes 0x%02x\n", BasicInfo->FileAttributes);

    *BufferLength -= sizeof(FILE_BASIC_INFORMATION);
    return STATUS_SUCCESS;
}


static
NTSTATUS
VfatSetDispositionInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB FCB,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_DISPOSITION_INFORMATION DispositionInfo)
{
    DPRINT("FsdSetDispositionInformation(<%wZ>, Delete %u)\n", &FCB->PathNameU, DispositionInfo->DeleteFile);

    ASSERT(DeviceExt != NULL);
    ASSERT(DeviceExt->FatInfo.BytesPerCluster != 0);
    ASSERT(FCB != NULL);

    if (!DispositionInfo->DeleteFile)
    {
        /* undelete the file */
        FCB->Flags &= ~FCB_DELETE_PENDING;
        FileObject->DeletePending = FALSE;
        return STATUS_SUCCESS;
    }

    if (BooleanFlagOn(FCB->Flags, FCB_DELETE_PENDING))
    {
        /* stream already marked for deletion. just update the file object */
        FileObject->DeletePending = TRUE;
        return STATUS_SUCCESS;
    }

    if (vfatFCBIsReadOnly(FCB))
    {
        return STATUS_CANNOT_DELETE;
    }

    if (vfatFCBIsRoot(FCB) || IsDotOrDotDot(&FCB->LongNameU))
    {
        /* we cannot delete a '.', '..' or the root directory */
        return STATUS_ACCESS_DENIED;
    }

    if (!MmFlushImageSection (FileObject->SectionObjectPointer, MmFlushForDelete))
    {
        /* can't delete a file if its mapped into a process */

        DPRINT("MmFlushImageSection returned FALSE\n");
        return STATUS_CANNOT_DELETE;
    }

    if (vfatFCBIsDirectory(FCB) && !VfatIsDirectoryEmpty(DeviceExt, FCB))
    {
        /* can't delete a non-empty directory */

        return STATUS_DIRECTORY_NOT_EMPTY;
    }

    /* all good */
    FCB->Flags |= FCB_DELETE_PENDING;
    FileObject->DeletePending = TRUE;

    return STATUS_SUCCESS;
}

static NTSTATUS
vfatPrepareTargetForRename(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB * ParentFCB,
    IN PUNICODE_STRING NewName,
    IN BOOLEAN ReplaceIfExists,
    IN PUNICODE_STRING ParentName,
    OUT PBOOLEAN Deleted)
{
    NTSTATUS Status;
    PVFATFCB TargetFcb;

    DPRINT("vfatPrepareTargetForRename(%p, %p, %wZ, %d, %wZ, %p)\n", DeviceExt, ParentFCB, NewName, ReplaceIfExists, ParentName);

    *Deleted = FALSE;
    /* Try to open target */
    Status = vfatGetFCBForFile(DeviceExt, ParentFCB, &TargetFcb, NewName);
    /* If it exists */
    if (NT_SUCCESS(Status))
    {
        DPRINT("Target file %wZ exists. FCB Flags %08x\n", NewName, TargetFcb->Flags);
        /* Check whether we are allowed to replace */
        if (ReplaceIfExists)
        {
            /* If that's a directory or a read-only file, we're not allowed */
            if (vfatFCBIsDirectory(TargetFcb) || vfatFCBIsReadOnly(TargetFcb))
            {
                DPRINT("And this is a readonly file!\n");
                vfatReleaseFCB(DeviceExt, *ParentFCB);
                *ParentFCB = NULL;
                vfatReleaseFCB(DeviceExt, TargetFcb);
                return STATUS_OBJECT_NAME_COLLISION;
            }


            /* If we still have a file object, close it. */
            if (TargetFcb->FileObject)
            {
                if (!MmFlushImageSection(TargetFcb->FileObject->SectionObjectPointer, MmFlushForDelete))
                {
                    DPRINT("MmFlushImageSection failed.\n");
                    vfatReleaseFCB(DeviceExt, *ParentFCB);
                    *ParentFCB = NULL;
                    vfatReleaseFCB(DeviceExt, TargetFcb);
                    return STATUS_ACCESS_DENIED;
                }

                TargetFcb->FileObject->DeletePending = TRUE;
                VfatCloseFile(DeviceExt, TargetFcb->FileObject);
            }

            /* If we are here, ensure the file isn't open by anyone! */
            if (TargetFcb->OpenHandleCount != 0)
            {
                DPRINT("There are still open handles for this file.\n");
                vfatReleaseFCB(DeviceExt, *ParentFCB);
                *ParentFCB = NULL;
                vfatReleaseFCB(DeviceExt, TargetFcb);
                return STATUS_ACCESS_DENIED;
            }

            /* Effectively delete old file to allow renaming */
            DPRINT("Effectively deleting the file.\n");
            VfatDelEntry(DeviceExt, TargetFcb, NULL);
            vfatReleaseFCB(DeviceExt, TargetFcb);
            *Deleted = TRUE;
            return STATUS_SUCCESS;
        }
        else
        {
            vfatReleaseFCB(DeviceExt, *ParentFCB);
            *ParentFCB = NULL;
            vfatReleaseFCB(DeviceExt, TargetFcb);
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    else if (*ParentFCB != NULL)
    {
        return STATUS_SUCCESS;
    }

    /* Failure */
    return Status;
}

static
BOOLEAN
IsThereAChildOpened(PVFATFCB FCB)
{
    PLIST_ENTRY Entry;
    PVFATFCB VolFCB;

    for (Entry = FCB->ParentListHead.Flink; Entry != &FCB->ParentListHead; Entry = Entry->Flink)
    {
        VolFCB = CONTAINING_RECORD(Entry, VFATFCB, ParentListEntry);
        if (VolFCB->OpenHandleCount != 0)
        {
            ASSERT(VolFCB->parentFcb == FCB);
            DPRINT1("At least one children file opened! %wZ (%u, %u)\n", &VolFCB->PathNameU, VolFCB->RefCount, VolFCB->OpenHandleCount);
            return TRUE;
        }

        if (vfatFCBIsDirectory(VolFCB) && !IsListEmpty(&VolFCB->ParentListHead))
        {
            if (IsThereAChildOpened(VolFCB))
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

static
VOID
VfatRenameChildFCB(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB FCB)
{
    PLIST_ENTRY Entry;
    PVFATFCB Child;

    if (IsListEmpty(&FCB->ParentListHead))
        return;

    for (Entry = FCB->ParentListHead.Flink; Entry != &FCB->ParentListHead; Entry = Entry->Flink)
    {
        NTSTATUS Status;

        Child = CONTAINING_RECORD(Entry, VFATFCB, ParentListEntry);
        DPRINT("Found %wZ with still %lu references (parent: %lu)!\n", &Child->PathNameU, Child->RefCount, FCB->RefCount);

        Status = vfatSetFCBNewDirName(DeviceExt, Child, FCB);
        if (!NT_SUCCESS(Status))
            continue;

        if (vfatFCBIsDirectory(Child))
        {
            VfatRenameChildFCB(DeviceExt, Child);
        }
    }
}

/*
 * FUNCTION: Set the file name information
 */
static
NTSTATUS
VfatSetRenameInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB FCB,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_RENAME_INFORMATION RenameInfo,
    PFILE_OBJECT TargetFileObject)
{
#ifdef NASSERTS_RENAME
#pragma push_macro("ASSERT")
#undef ASSERT
#define ASSERT(x) ((VOID) 0)
#endif
    NTSTATUS Status;
    UNICODE_STRING NewName;
    UNICODE_STRING SourcePath;
    UNICODE_STRING SourceFile;
    UNICODE_STRING NewPath;
    UNICODE_STRING NewFile;
    PFILE_OBJECT RootFileObject;
    PVFATFCB RootFCB;
    UNICODE_STRING RenameInfoString;
    PVFATFCB ParentFCB;
    IO_STATUS_BLOCK IoStatusBlock;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE TargetHandle;
    BOOLEAN DeletedTarget;
    ULONG OldReferences, NewReferences;
    PVFATFCB OldParent;

    DPRINT("VfatSetRenameInfo(%p, %p, %p, %p, %p)\n", FileObject, FCB, DeviceExt, RenameInfo, TargetFileObject);

    /* Disallow renaming root */
    if (vfatFCBIsRoot(FCB))
    {
        return STATUS_INVALID_PARAMETER;
    }

    OldReferences = FCB->parentFcb->RefCount;
#ifdef NASSERTS_RENAME
    UNREFERENCED_PARAMETER(OldReferences);
#endif

    /* If we are performing relative opening for rename, get FO for getting FCB and path name */
    if (RenameInfo->RootDirectory != NULL)
    {
        /* We cannot tolerate relative opening with a full path */
        if (RenameInfo->FileName[0] == L'\\')
        {
            return STATUS_OBJECT_NAME_INVALID;
        }

        Status = ObReferenceObjectByHandle(RenameInfo->RootDirectory,
                                           FILE_READ_DATA,
                                           *IoFileObjectType,
                                           ExGetPreviousMode(),
                                           (PVOID *)&RootFileObject,
                                           NULL);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        RootFCB = RootFileObject->FsContext;
    }

    RtlInitEmptyUnicodeString(&NewName, NULL, 0);
    ParentFCB = NULL;

    if (TargetFileObject == NULL)
    {
        /* If we don't have target file object, construct paths thanks to relative FCB, if any, and with
         * information supplied by the user
         */

        /* First, setup a string we'll work on */
        RenameInfoString.Length = RenameInfo->FileNameLength;
        RenameInfoString.MaximumLength = RenameInfo->FileNameLength;
        RenameInfoString.Buffer = RenameInfo->FileName;

        /* Check whether we have FQN */
        if (RenameInfoString.Length > 6 * sizeof(WCHAR))
        {
            if (RenameInfoString.Buffer[0] == L'\\' && RenameInfoString.Buffer[1] == L'?' &&
                RenameInfoString.Buffer[2] == L'?' && RenameInfoString.Buffer[3] == L'\\' &&
                RenameInfoString.Buffer[5] == L':' && (RenameInfoString.Buffer[4] >= L'A' &&
                RenameInfoString.Buffer[4] <= L'Z'))
            {
                /* If so, open its target directory */
                InitializeObjectAttributes(&ObjectAttributes,
                                           &RenameInfoString,
                                           OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                           NULL, NULL);

                Status = IoCreateFile(&TargetHandle,
                                      FILE_WRITE_DATA | SYNCHRONIZE,
                                      &ObjectAttributes,
                                      &IoStatusBlock,
                                      NULL, 0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      FILE_OPEN,
                                      FILE_OPEN_FOR_BACKUP_INTENT,
                                      NULL, 0,
                                      CreateFileTypeNone,
                                      NULL,
                                      IO_FORCE_ACCESS_CHECK | IO_OPEN_TARGET_DIRECTORY);
                if (!NT_SUCCESS(Status))
                {
                    goto Cleanup;
                }

                /* Get its FO to get the FCB */
                Status = ObReferenceObjectByHandle(TargetHandle,
                                                   FILE_WRITE_DATA,
                                                   *IoFileObjectType,
                                                   KernelMode,
                                                   (PVOID *)&TargetFileObject,
                                                   NULL);
                if (!NT_SUCCESS(Status))
                {
                    ZwClose(TargetHandle);
                    goto Cleanup;
                }

                /* Are we working on the same volume? */
                if (IoGetRelatedDeviceObject(TargetFileObject) != IoGetRelatedDeviceObject(FileObject))
                {
                    ObDereferenceObject(TargetFileObject);
                    ZwClose(TargetHandle);
                    TargetFileObject = NULL;
                    Status = STATUS_NOT_SAME_DEVICE;
                    goto Cleanup;
                }
            }
        }

        NewName.Length = 0;
        NewName.MaximumLength = RenameInfo->FileNameLength;
        if (RenameInfo->RootDirectory != NULL)
        {
            NewName.MaximumLength += sizeof(WCHAR) + RootFCB->PathNameU.Length;
        }
        else if (RenameInfo->FileName[0] != L'\\')
        {
            /* We don't have full path, and we don't have root directory:
             * => we move inside the same directory
             */
            NewName.MaximumLength += sizeof(WCHAR) + FCB->DirNameU.Length;
        }
        else if (TargetFileObject != NULL)
        {
            /* We had a FQN:
             * => we need to use its correct path
             */
            NewName.MaximumLength += sizeof(WCHAR) + ((PVFATFCB)TargetFileObject->FsContext)->PathNameU.Length;
        }

        NewName.Buffer = ExAllocatePoolWithTag(NonPagedPool, NewName.MaximumLength, TAG_NAME);
        if (NewName.Buffer == NULL)
        {
            if (TargetFileObject != NULL)
            {
                ObDereferenceObject(TargetFileObject);
                ZwClose(TargetHandle);
                TargetFileObject = NULL;
            }
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }

        if (RenameInfo->RootDirectory != NULL)
        {
            /* Here, copy first absolute and then append relative */
            RtlCopyUnicodeString(&NewName, &RootFCB->PathNameU);
            NewName.Buffer[NewName.Length / sizeof(WCHAR)] = L'\\';
            NewName.Length += sizeof(WCHAR);
            RtlAppendUnicodeStringToString(&NewName, &RenameInfoString);
        }
        else if (RenameInfo->FileName[0] != L'\\')
        {
            /* Here, copy first work directory and then append filename */
            RtlCopyUnicodeString(&NewName, &FCB->DirNameU);
            NewName.Buffer[NewName.Length / sizeof(WCHAR)] = L'\\';
            NewName.Length += sizeof(WCHAR);
            RtlAppendUnicodeStringToString(&NewName, &RenameInfoString);
        }
        else if (TargetFileObject != NULL)
        {
            /* Here, copy first path name and then append filename */
            RtlCopyUnicodeString(&NewName, &((PVFATFCB)TargetFileObject->FsContext)->PathNameU);
            NewName.Buffer[NewName.Length / sizeof(WCHAR)] = L'\\';
            NewName.Length += sizeof(WCHAR);
            RtlAppendUnicodeStringToString(&NewName, &RenameInfoString);
        }
        else
        {
            /* Here we should have full path, so simply copy it */
            RtlCopyUnicodeString(&NewName, &RenameInfoString);
        }

        /* Do we have to cleanup some stuff? */
        if (TargetFileObject != NULL)
        {
            ObDereferenceObject(TargetFileObject);
            ZwClose(TargetHandle);
            TargetFileObject = NULL;
        }
    }
    else
    {
        /* At that point, we shouldn't care about whether we are relative opening
         * Target FO FCB should already have full path
         */

        /* Before constructing string, just make a sanity check (just to be sure!) */
        if (IoGetRelatedDeviceObject(TargetFileObject) != IoGetRelatedDeviceObject(FileObject))
        {
            Status = STATUS_NOT_SAME_DEVICE;
            goto Cleanup;
        }

        NewName.Length = 0;
        NewName.MaximumLength = TargetFileObject->FileName.Length + ((PVFATFCB)TargetFileObject->FsContext)->PathNameU.Length + sizeof(WCHAR);
        NewName.Buffer = ExAllocatePoolWithTag(NonPagedPool, NewName.MaximumLength, TAG_NAME);
        if (NewName.Buffer == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }

        RtlCopyUnicodeString(&NewName, &((PVFATFCB)TargetFileObject->FsContext)->PathNameU);
        /* If \, it's already backslash terminated, don't add it */
        if (!vfatFCBIsRoot(TargetFileObject->FsContext))
        {
            NewName.Buffer[NewName.Length / sizeof(WCHAR)] = L'\\';
            NewName.Length += sizeof(WCHAR);
        }
        RtlAppendUnicodeStringToString(&NewName, &TargetFileObject->FileName);
    }

    /* Explode our paths to get path & filename */
    vfatSplitPathName(&FCB->PathNameU, &SourcePath, &SourceFile);
    DPRINT("Old dir: %wZ, Old file: %wZ\n", &SourcePath, &SourceFile);
    vfatSplitPathName(&NewName, &NewPath, &NewFile);
    DPRINT("New dir: %wZ, New file: %wZ\n", &NewPath, &NewFile);

    if (IsDotOrDotDot(&NewFile))
    {
        Status = STATUS_OBJECT_NAME_INVALID;
        goto Cleanup;
    }

    if (vfatFCBIsDirectory(FCB) && !IsListEmpty(&FCB->ParentListHead))
    {
        if (IsThereAChildOpened(FCB))
        {
            Status = STATUS_ACCESS_DENIED;
            ASSERT(OldReferences == FCB->parentFcb->RefCount);
            goto Cleanup;
        }
    }

    /* Are we working in place? */
    if (FsRtlAreNamesEqual(&SourcePath, &NewPath, TRUE, NULL))
    {
        if (FsRtlAreNamesEqual(&SourceFile, &NewFile, FALSE, NULL))
        {
            Status = STATUS_SUCCESS;
            ASSERT(OldReferences == FCB->parentFcb->RefCount);
            goto Cleanup;
        }

        if (FsRtlAreNamesEqual(&SourceFile, &NewFile, TRUE, NULL))
        {
            vfatReportChange(DeviceExt,
                             FCB,
                             (vfatFCBIsDirectory(FCB) ?
                              FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME),
                             FILE_ACTION_RENAMED_OLD_NAME);
            Status = vfatRenameEntry(DeviceExt, FCB, &NewFile, TRUE);
            if (NT_SUCCESS(Status))
            {
                vfatReportChange(DeviceExt,
                                 FCB,
                                 (vfatFCBIsDirectory(FCB) ?
                                  FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME),
                                 FILE_ACTION_RENAMED_NEW_NAME);
            }
        }
        else
        {
            /* Try to find target */
            ParentFCB = FCB->parentFcb;
            vfatGrabFCB(DeviceExt, ParentFCB);
            Status = vfatPrepareTargetForRename(DeviceExt,
                                                &ParentFCB,
                                                &NewFile,
                                                RenameInfo->ReplaceIfExists,
                                                &NewPath,
                                                &DeletedTarget);
            if (!NT_SUCCESS(Status))
            {
                ASSERT(OldReferences == FCB->parentFcb->RefCount - 1);
                ASSERT(OldReferences == ParentFCB->RefCount - 1);
                goto Cleanup;
            }

            vfatReportChange(DeviceExt,
                             FCB,
                             (vfatFCBIsDirectory(FCB) ?
                              FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME),
                             (DeletedTarget ? FILE_ACTION_REMOVED : FILE_ACTION_RENAMED_OLD_NAME));
            Status = vfatRenameEntry(DeviceExt, FCB, &NewFile, FALSE);
            if (NT_SUCCESS(Status))
            {
                if (DeletedTarget)
                {
                    vfatReportChange(DeviceExt,
                                     FCB,
                                     FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE
                                     | FILE_NOTIFY_CHANGE_LAST_ACCESS | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_EA,
                                     FILE_ACTION_MODIFIED);
                }
                else
                {
                    vfatReportChange(DeviceExt,
                                     FCB,
                                     (vfatFCBIsDirectory(FCB) ?
                                      FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME),
                                     FILE_ACTION_RENAMED_NEW_NAME);
                }
            }
        }

        ASSERT(OldReferences == FCB->parentFcb->RefCount - 1); // extra grab
        ASSERT(OldReferences == ParentFCB->RefCount - 1); // extra grab
    }
    else
    {

        /* Try to find target */
        ParentFCB = NULL;
        OldParent = FCB->parentFcb;
#ifdef NASSERTS_RENAME
        UNREFERENCED_PARAMETER(OldParent);
#endif
        Status = vfatPrepareTargetForRename(DeviceExt,
                                            &ParentFCB,
                                            &NewName,
                                            RenameInfo->ReplaceIfExists,
                                            &NewPath,
                                            &DeletedTarget);
        if (!NT_SUCCESS(Status))
        {
            ASSERT(OldReferences == FCB->parentFcb->RefCount);
            goto Cleanup;
        }

        NewReferences = ParentFCB->RefCount;
#ifdef NASSERTS_RENAME
        UNREFERENCED_PARAMETER(NewReferences);
#endif

        vfatReportChange(DeviceExt,
                         FCB,
                         (vfatFCBIsDirectory(FCB) ?
                          FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME),
                         FILE_ACTION_REMOVED);
        Status = VfatMoveEntry(DeviceExt, FCB, &NewFile, ParentFCB);
        if (NT_SUCCESS(Status))
        {
            if (DeletedTarget)
            {
                vfatReportChange(DeviceExt,
                                 FCB,
                                 FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE
                                 | FILE_NOTIFY_CHANGE_LAST_ACCESS | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_EA,
                                 FILE_ACTION_MODIFIED);
            }
            else
            {
                vfatReportChange(DeviceExt,
                                 FCB,
                                 (vfatFCBIsDirectory(FCB) ?
                                  FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME),
                                 FILE_ACTION_ADDED);
            }
        }
    }

    if (NT_SUCCESS(Status) && vfatFCBIsDirectory(FCB))
    {
        VfatRenameChildFCB(DeviceExt, FCB);
    }

    ASSERT(OldReferences == OldParent->RefCount + 1); // removed file
    ASSERT(NewReferences == ParentFCB->RefCount - 1); // new file
Cleanup:
    if (ParentFCB != NULL) vfatReleaseFCB(DeviceExt, ParentFCB);
    if (NewName.Buffer != NULL) ExFreePoolWithTag(NewName.Buffer, TAG_NAME);
    if (RenameInfo->RootDirectory != NULL) ObDereferenceObject(RootFileObject);

    return Status;
#ifdef NASSERTS_RENAME
#pragma pop_macro("ASSERT")
#endif
}

/*
 * FUNCTION: Retrieve the file name information
 */
static
NTSTATUS
VfatGetNameInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB FCB,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_NAME_INFORMATION NameInfo,
    PULONG BufferLength)
{
    ULONG BytesToCopy;

    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DeviceExt);

    ASSERT(NameInfo != NULL);
    ASSERT(FCB != NULL);

    /* If buffer can't hold at least the file name length, bail out */
    if (*BufferLength < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
        return STATUS_BUFFER_OVERFLOW;

    /* Save file name length, and as much file len, as buffer length allows */
    NameInfo->FileNameLength = FCB->PathNameU.Length;

    /* Calculate amount of bytes to copy not to overflow the buffer */
    BytesToCopy = min(FCB->PathNameU.Length,
                      *BufferLength - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]));

    /* Fill in the bytes */
    RtlCopyMemory(NameInfo->FileName, FCB->PathNameU.Buffer, BytesToCopy);

    /* Check if we could write more but are not able to */
    if (*BufferLength < FCB->PathNameU.Length + (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
    {
        /* Return number of bytes written */
        *BufferLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + BytesToCopy;
        return STATUS_BUFFER_OVERFLOW;
    }

    /* We filled up as many bytes, as needed */
    *BufferLength -= (FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + FCB->PathNameU.Length);

    return STATUS_SUCCESS;
}

static
NTSTATUS
VfatGetInternalInformation(
    PVFATFCB Fcb,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_INTERNAL_INFORMATION InternalInfo,
    PULONG BufferLength)
{
    ASSERT(InternalInfo);
    ASSERT(Fcb);

    if (*BufferLength < sizeof(FILE_INTERNAL_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    /* The entry's place (directory and index) is unique; empty files have
       no first cluster to tell them apart. */
    UNREFERENCED_PARAMETER(DeviceExt);
    InternalInfo->IndexNumber.QuadPart = Fcb->parentFcb ?
        ((LONGLONG)Fcb->parentFcb->Chain.FirstCluster << 32) | Fcb->startIndex :
        (LONGLONG)Fcb->Chain.FirstCluster << 32;

    *BufferLength -= sizeof(FILE_INTERNAL_INFORMATION);
    return STATUS_SUCCESS;
}


/*
 * FUNCTION: Retrieve the file network open information
 */
static
NTSTATUS
VfatGetNetworkOpenInformation(
    PVFATFCB Fcb,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_NETWORK_OPEN_INFORMATION NetworkInfo,
    PULONG BufferLength)
{
    ASSERT(NetworkInfo);
    ASSERT(Fcb);

    if (*BufferLength < sizeof(FILE_NETWORK_OPEN_INFORMATION))
        return(STATUS_BUFFER_OVERFLOW);

    UNREFERENCED_PARAMETER(DeviceExt);
    ExfatTimestampToSystemTime(Fcb->entry.File.CreateTimestamp,
                               Fcb->entry.File.Create10msIncrement,
                               Fcb->entry.File.CreateUtcOffset,
                               &NetworkInfo->CreationTime);
    ExfatTimestampToSystemTime(Fcb->entry.File.LastAccessedTimestamp, 0,
                               Fcb->entry.File.LastAccessedUtcOffset,
                               &NetworkInfo->LastAccessTime);
    ExfatTimestampToSystemTime(Fcb->entry.File.LastModifiedTimestamp,
                               Fcb->entry.File.LastModified10msIncrement,
                               Fcb->entry.File.LastModifiedUtcOffset,
                               &NetworkInfo->LastWriteTime);
    NetworkInfo->ChangeTime.QuadPart = NetworkInfo->LastWriteTime.QuadPart;

    if (vfatFCBIsDirectory(Fcb))
    {
        NetworkInfo->EndOfFile.QuadPart = 0L;
        NetworkInfo->AllocationSize.QuadPart = 0L;
    }
    else
    {
        NetworkInfo->AllocationSize = Fcb->RFCB.AllocationSize;
        NetworkInfo->EndOfFile = Fcb->RFCB.FileSize;
    }

    NetworkInfo->FileAttributes = *Fcb->Attributes & 0x3f;
    /* Synthesize FILE_ATTRIBUTE_NORMAL */
    if (0 == (NetworkInfo->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                             FILE_ATTRIBUTE_ARCHIVE |
                                             FILE_ATTRIBUTE_SYSTEM |
                                             FILE_ATTRIBUTE_HIDDEN |
                                             FILE_ATTRIBUTE_READONLY)))
    {
        DPRINT("Synthesizing FILE_ATTRIBUTE_NORMAL\n");
        NetworkInfo->FileAttributes |= FILE_ATTRIBUTE_NORMAL;
    }

    *BufferLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Retrieve the attribute tag information. FAT has no reparse
 * points, so the tag is always 0. Programs built with the Visual C++
 * std::filesystem runtime query this class for every status() call.
 */
static
NTSTATUS
VfatGetAttributeTagInformation(
    PVFATFCB Fcb,
    PFILE_ATTRIBUTE_TAG_INFORMATION TagInfo,
    PULONG BufferLength)
{
    ASSERT(TagInfo);
    ASSERT(Fcb);

    if (*BufferLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    TagInfo->FileAttributes = *Fcb->Attributes & 0x3f;
    /* Synthesize FILE_ATTRIBUTE_NORMAL, as for the other information classes */
    if (0 == (TagInfo->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                         FILE_ATTRIBUTE_ARCHIVE |
                                         FILE_ATTRIBUTE_SYSTEM |
                                         FILE_ATTRIBUTE_HIDDEN |
                                         FILE_ATTRIBUTE_READONLY)))
    {
        TagInfo->FileAttributes |= FILE_ATTRIBUTE_NORMAL;
    }
    TagInfo->ReparseTag = 0;

    *BufferLength -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
    return STATUS_SUCCESS;
}


static
NTSTATUS
VfatGetEaInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB Fcb,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_EA_INFORMATION Info,
    PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Fcb);

    /* FIXME - use SEH to access the buffer! */
    UNREFERENCED_PARAMETER(DeviceExt);
    Info->EaSize = 0;
    *BufferLength -= sizeof(*Info);
    return STATUS_SUCCESS;
}


/*
 * FUNCTION: Retrieve the all file information
 */
static
NTSTATUS
VfatGetAllInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB Fcb,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_ALL_INFORMATION Info,
    PULONG BufferLength)
{
    NTSTATUS Status;

    ASSERT(Info);
    ASSERT(Fcb);

    if (*BufferLength < FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName))
        return STATUS_BUFFER_OVERFLOW;

    *BufferLength -= (sizeof(FILE_ACCESS_INFORMATION) + sizeof(FILE_MODE_INFORMATION) + sizeof(FILE_ALIGNMENT_INFORMATION));

    /* Basic Information */
    Status = VfatGetBasicInformation(FileObject, Fcb, DeviceExt, &Info->BasicInformation, BufferLength);
    if (!NT_SUCCESS(Status)) return Status;
    /* Standard Information */
    Status = VfatGetStandardInformation(Fcb, &Info->StandardInformation, BufferLength);
    if (!NT_SUCCESS(Status)) return Status;
    /* Internal Information */
    Status = VfatGetInternalInformation(Fcb, DeviceExt, &Info->InternalInformation, BufferLength);
    if (!NT_SUCCESS(Status)) return Status;
    /* EA Information */
    Status = VfatGetEaInformation(FileObject, Fcb, DeviceExt, &Info->EaInformation, BufferLength);
    if (!NT_SUCCESS(Status)) return Status;
    /* Position Information */
    Status = VfatGetPositionInformation(FileObject, Fcb, DeviceExt, &Info->PositionInformation, BufferLength);
    if (!NT_SUCCESS(Status)) return Status;
    /* Name Information */
    Status = VfatGetNameInformation(FileObject, Fcb, DeviceExt, &Info->NameInformation, BufferLength);

    return Status;
}

/*
 * Set a file's or directory's size to AllocationSize, growing or shrinking
 * its cluster chain to match. Growing keeps the valid data length, so the
 * new bytes read as zeros without being written; a directory's new clusters
 * are cleared by its caller.
 */
NTSTATUS
VfatSetAllocationSizeInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB Fcb,
    PDEVICE_EXTENSION DeviceExt,
    PLARGE_INTEGER AllocationSize)
{
    ULONG ClusterSize = DeviceExt->FatInfo.BytesPerCluster;
    ULONGLONG NewSize;
    ULONGLONG OldFileSize;
    ULONGLONG NeededClusters;
    ULONG OldCount;
    ULONG Cluster;
    LARGE_INTEGER Available;
    BOOLEAN AllocSizeChanged = FALSE;
    EXFAT_CHAIN SavedChain;
    LARGE_INTEGER SavedFileSize;
    LARGE_INTEGER SavedValidDataLength;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("VfatSetAllocationSizeInformation(File <%wZ>, AllocationSize %I64d)\n",
           &Fcb->PathNameU, AllocationSize->QuadPart);

    if (AllocationSize->QuadPart < 0)
        return STATUS_INVALID_PARAMETER;
    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        return STATUS_MEDIA_WRITE_PROTECTED;

    NewSize = AllocationSize->QuadPart;
    if (NewSize == (ULONGLONG)Fcb->RFCB.FileSize.QuadPart)
        return STATUS_SUCCESS;

    NeededClusters = (NewSize + ClusterSize - 1) / ClusterSize;
    if (NeededClusters > DeviceExt->FatInfo.NumberOfClusters)
        return STATUS_DISK_FULL;

    OldFileSize = Fcb->RFCB.FileSize.QuadPart;
    OldCount = Fcb->Chain.Count;
    if (NeededClusters > OldCount)
    {
        /* Refuse up front rather than grow halfway. */
        CountAvailableClusters(DeviceExt, &Available);
        if (NeededClusters - OldCount > (ULONGLONG)Available.QuadPart)
            return STATUS_DISK_FULL;

        AllocSizeChanged = TRUE;
        if (Fcb->Chain.FirstCluster == 0)
        {
            Fcb->LastCluster = 0;
            Fcb->LastOffset = 0;
            Status = NextCluster(DeviceExt, &Fcb->Chain, &Cluster, TRUE);
        }
        else if (Fcb->LastCluster > 0 &&
                 Fcb->LastOffset == (ULONGLONG)(OldCount - 1) * ClusterSize)
        {
            Cluster = Fcb->LastCluster;
        }
        else
        {
            Status = OffsetToCluster(DeviceExt, &Fcb->Chain, Fcb->Chain.FirstCluster,
                                     (ULONGLONG)(OldCount - 1) * ClusterSize, &Cluster, FALSE);
        }

        /* Cluster is the last cluster of the chain; append the rest. */
        while (NT_SUCCESS(Status) && Fcb->Chain.Count < NeededClusters)
        {
            Status = NextCluster(DeviceExt, &Fcb->Chain, &Cluster, TRUE);
        }

        if (!NT_SUCCESS(Status))
        {
            /* Give back what this call added. */
            ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
            ExfatFreeClusters(DeviceExt, &Fcb->Chain, OldCount);
            ExReleaseResourceLite(&DeviceExt->FatResource);
            Fcb->LastCluster = 0;
            Fcb->LastOffset = 0;
            return (Status == STATUS_DISK_FULL) ? Status : STATUS_DISK_FULL;
        }

        Fcb->LastCluster = Cluster;
        Fcb->LastOffset = (ULONGLONG)(Fcb->Chain.Count - 1) * ClusterSize;
    }
    else if (NeededClusters < OldCount)
    {
        DPRINT("Check for the ability to set file size\n");
        if (!MmCanFileBeTruncated(FileObject->SectionObjectPointer,
                                  (PLARGE_INTEGER)AllocationSize))
        {
            DPRINT("Couldn't set file size!\n");
            return STATUS_USER_MAPPED_FILE;
        }

        /*
         * The entry with the new size goes to disk before the clusters are
         * freed: cut off in between, the volume only leaks clusters instead
         * of describing a file whose clusters are free.
         */
        SavedChain = Fcb->Chain;
        SavedFileSize = Fcb->RFCB.FileSize;
        SavedValidDataLength = Fcb->RFCB.ValidDataLength;
        Fcb->RFCB.FileSize.QuadPart = NewSize;
        Fcb->RFCB.ValidDataLength.QuadPart = min(Fcb->RFCB.ValidDataLength.QuadPart, (LONGLONG)NewSize);
        if (NeededClusters == 0)
        {
            Fcb->Chain.FirstCluster = 0;
            Fcb->Chain.Count = 0;
            Fcb->Chain.NoFatChain = FALSE;
        }
        Status = VfatUpdateEntry(DeviceExt, Fcb);
        if (NT_SUCCESS(Status))
            Status = ExfatFlushFcbSet(Fcb);
        Fcb->Chain = SavedChain;
        if (!NT_SUCCESS(Status))
        {
            Fcb->RFCB.FileSize = SavedFileSize;
            Fcb->RFCB.ValidDataLength = SavedValidDataLength;
            return Status;
        }

        AllocSizeChanged = TRUE;
        Fcb->LastCluster = 0;
        Fcb->LastOffset = 0;
        ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
        Status = ExfatFreeClusters(DeviceExt, &Fcb->Chain, (ULONG)NeededClusters);
        ExReleaseResourceLite(&DeviceExt->FatResource);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Fcb->RFCB.AllocationSize.QuadPart = (LONGLONG)Fcb->Chain.Count * ClusterSize;
    Fcb->RFCB.FileSize.QuadPart = NewSize;
    if (vfatFCBIsDirectory(Fcb))
        Fcb->RFCB.ValidDataLength.QuadPart = NewSize;
    else
        Fcb->RFCB.ValidDataLength.QuadPart = min(Fcb->RFCB.ValidDataLength.QuadPart, (LONGLONG)NewSize);
    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);

    /*
     * Shrinking: drop the pages wholly past the new end. CcSetFileSizes does
     * nothing when this file object has no cache map, yet the section can
     * still hold those pages, and growing the file again would expose them.
     */
    if (!vfatFCBIsDirectory(Fcb) && NewSize < OldFileSize)
    {
        LARGE_INTEGER PurgeStart;

        PurgeStart.QuadPart = ROUND_UP_64(NewSize, PAGE_SIZE);
        if (!CcPurgeCacheSection(&Fcb->SectionObjectPointers, &PurgeStart, 0, FALSE))
        {
            DPRINT1("exFAT: could not purge '%wZ' past %I64d\n",
                    &Fcb->PathNameU, PurgeStart.QuadPart);
        }
    }

    /*
     * Growing past the valid data length: the page that holds it may still
     * be cached with data from before a truncation (even after the last
     * cache map went away, the section can keep the page resident), and
     * the lazy writer would then make it valid data. Zero the rest of that
     * page through the cache. The file object
     * gets a cache map first: without one, CcZeroData writes to the disk
     * directly and needs sector-aligned bounds. Pages wholly past the old
     * end were purged when the file shrank, and pages read from disk go
     * through the read path that zeros everything past the valid data.
     */
    if (!vfatFCBIsDirectory(Fcb) && NewSize > OldFileSize &&
        (ULONGLONG)Fcb->RFCB.ValidDataLength.QuadPart < NewSize)
    {
        LARGE_INTEGER ZeroStart;
        LARGE_INTEGER ZeroEnd;

        ZeroStart = Fcb->RFCB.ValidDataLength;
        ZeroEnd.QuadPart = min(ROUND_UP_64(ZeroStart.QuadPart, PAGE_SIZE), NewSize);
        if (ZeroEnd.QuadPart > ZeroStart.QuadPart)
        {
            _SEH2_TRY
            {
                if (FileObject->PrivateCacheMap == NULL)
                {
                    CcInitializeCacheMap(FileObject,
                                         (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize,
                                         FALSE,
                                         &VfatGlobalData->CacheMgrCallbacks,
                                         Fcb);
                }
                CcZeroData(FileObject, &ZeroStart, &ZeroEnd, TRUE);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    /* Update the on-disk directory entry */
    Fcb->Flags |= FCB_IS_DIRTY;
    if (AllocSizeChanged)
    {
        Status = VfatUpdateEntry(DeviceExt, Fcb);
        vfatReportChange(DeviceExt, Fcb, FILE_NOTIFY_CHANGE_SIZE, FILE_ACTION_MODIFIED);
        if (Status == STATUS_MEDIA_WRITE_PROTECTED)
            return Status;
    }
    return STATUS_SUCCESS;
}

/*
 * FUNCTION: Retrieve the specified file information
 */
NTSTATUS
VfatQueryInformation(
    PVFAT_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PVFATFCB FCB;

    NTSTATUS Status = STATUS_SUCCESS;
    PVOID SystemBuffer;
    ULONG BufferLength;

    /* PRECONDITION */
    ASSERT(IrpContext);

    /* INITIALIZATION */
    FileInformationClass = IrpContext->Stack->Parameters.QueryFile.FileInformationClass;
    FCB = (PVFATFCB) IrpContext->FileObject->FsContext;

    DPRINT("VfatQueryInformation is called for '%s'\n",
           FileInformationClass >= FileMaximumInformation - 1 ? "????" : FileInformationClassNames[FileInformationClass]);

    if (FCB == NULL)
    {
        DPRINT1("IRP_MJ_QUERY_INFORMATION without FCB!\n");
        IrpContext->Irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    SystemBuffer = IrpContext->Irp->AssociatedIrp.SystemBuffer;
    BufferLength = IrpContext->Stack->Parameters.QueryFile.Length;

    if (!BooleanFlagOn(FCB->Flags, FCB_IS_PAGE_FILE))
    {
        if (!ExAcquireResourceSharedLite(&FCB->MainResource,
                                         BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
        {
            return VfatMarkIrpContextForQueue(IrpContext);
        }
    }

    switch (FileInformationClass)
    {
        case FileStandardInformation:
            Status = VfatGetStandardInformation(FCB,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FilePositionInformation:
            Status = VfatGetPositionInformation(IrpContext->FileObject,
                                                FCB,
                                                IrpContext->DeviceExt,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileBasicInformation:
            Status = VfatGetBasicInformation(IrpContext->FileObject,
                                             FCB,
                                             IrpContext->DeviceExt,
                                             SystemBuffer,
                                             &BufferLength);
            break;

        case FileNameInformation:
            Status = VfatGetNameInformation(IrpContext->FileObject,
                                            FCB,
                                            IrpContext->DeviceExt,
                                            SystemBuffer,
                                            &BufferLength);
            break;

        case FileInternalInformation:
            Status = VfatGetInternalInformation(FCB,
                                                IrpContext->DeviceExt,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileNetworkOpenInformation:
            Status = VfatGetNetworkOpenInformation(FCB,
                                                   IrpContext->DeviceExt,
                                                   SystemBuffer,
                                                   &BufferLength);
            break;

        case FileAllInformation:
            Status = VfatGetAllInformation(IrpContext->FileObject,
                                           FCB,
                                           IrpContext->DeviceExt,
                                           SystemBuffer,
                                           &BufferLength);
            break;

        case FileEaInformation:
            Status = VfatGetEaInformation(IrpContext->FileObject,
                                          FCB,
                                          IrpContext->DeviceExt,
                                          SystemBuffer,
                                          &BufferLength);
            break;

        case FileAlternateNameInformation:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case FileAttributeTagInformation:
            Status = VfatGetAttributeTagInformation(FCB,
                                                    SystemBuffer,
                                                    &BufferLength);
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
    }

    if (!BooleanFlagOn(FCB->Flags, FCB_IS_PAGE_FILE))
    {
        ExReleaseResourceLite(&FCB->MainResource);
    }

    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
        IrpContext->Irp->IoStatus.Information =
            IrpContext->Stack->Parameters.QueryFile.Length - BufferLength;
    else
        IrpContext->Irp->IoStatus.Information = 0;

    return Status;
}

/*
 * FUNCTION: Retrieve the specified file information
 */
NTSTATUS
VfatSetInformation(
    PVFAT_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PVFATFCB FCB;
    NTSTATUS Status = STATUS_SUCCESS;
    PVOID SystemBuffer;
    BOOLEAN LockDir;

    /* PRECONDITION */
    ASSERT(IrpContext);

    DPRINT("VfatSetInformation(IrpContext %p)\n", IrpContext);

    /* INITIALIZATION */
    FileInformationClass =
        IrpContext->Stack->Parameters.SetFile.FileInformationClass;
    FCB = (PVFATFCB) IrpContext->FileObject->FsContext;
    SystemBuffer = IrpContext->Irp->AssociatedIrp.SystemBuffer;

    DPRINT("VfatSetInformation is called for '%s'\n",
           FileInformationClass >= FileMaximumInformation - 1 ? "????" : FileInformationClassNames[ FileInformationClass]);

    DPRINT("FileInformationClass %d\n", FileInformationClass);
    DPRINT("SystemBuffer %p\n", SystemBuffer);

    if (FCB == NULL)
    {
        DPRINT1("IRP_MJ_SET_INFORMATION without FCB!\n");
        IrpContext->Irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    /* Special: We should call MmCanFileBeTruncated here to determine if changing
       the file size would be allowed.  If not, we bail with the right error.
       We must do this before acquiring the lock. */
    if (FileInformationClass == FileEndOfFileInformation)
    {
        DPRINT("Check for the ability to set file size\n");
        if (!MmCanFileBeTruncated(IrpContext->FileObject->SectionObjectPointer,
                                  (PLARGE_INTEGER)SystemBuffer))
        {
            DPRINT("Couldn't set file size!\n");
            IrpContext->Irp->IoStatus.Information = 0;
            return STATUS_USER_MAPPED_FILE;
        }
        DPRINT("Can set file size\n");
    }

    LockDir = FALSE;
    if (FileInformationClass == FileRenameInformation || FileInformationClass == FileAllocationInformation ||
        FileInformationClass == FileEndOfFileInformation || FileInformationClass == FileBasicInformation)
    {
        LockDir = TRUE;
    }

    if (LockDir)
    {
        if (!ExAcquireResourceExclusiveLite(&((PDEVICE_EXTENSION)IrpContext->DeviceObject->DeviceExtension)->DirResource,
                                            BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
        {
            return VfatMarkIrpContextForQueue(IrpContext);
        }
    }

    if (!BooleanFlagOn(FCB->Flags, FCB_IS_PAGE_FILE))
    {
        if (!ExAcquireResourceExclusiveLite(&FCB->MainResource,
                                            BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
        {
            if (LockDir)
            {
                ExReleaseResourceLite(&((PDEVICE_EXTENSION)IrpContext->DeviceObject->DeviceExtension)->DirResource);
            }

            return VfatMarkIrpContextForQueue(IrpContext);
        }
    }

    switch (FileInformationClass)
    {
        case FilePositionInformation:
            Status = VfatSetPositionInformation(IrpContext->FileObject,
                                                SystemBuffer);
            break;

        case FileDispositionInformation:
            Status = VfatSetDispositionInformation(IrpContext->FileObject,
                                                   FCB,
                                                   IrpContext->DeviceExt,
                                                   SystemBuffer);
            break;

        case FileAllocationInformation:
        case FileEndOfFileInformation:
            Status = VfatSetAllocationSizeInformation(IrpContext->FileObject,
                                                      FCB,
                                                      IrpContext->DeviceExt,
                                                      (PLARGE_INTEGER)SystemBuffer);
            break;

        case FileBasicInformation:
            Status = VfatSetBasicInformation(IrpContext->FileObject,
                                             FCB,
                                             IrpContext->DeviceExt,
                                             SystemBuffer);
            break;

        case FileRenameInformation:
            Status = VfatSetRenameInformation(IrpContext->FileObject,
                                              FCB,
                                              IrpContext->DeviceExt,
                                              SystemBuffer,
                                              IrpContext->Stack->Parameters.SetFile.FileObject);
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
    }

    if (!BooleanFlagOn(FCB->Flags, FCB_IS_PAGE_FILE))
    {
        ExReleaseResourceLite(&FCB->MainResource);
    }

    if (LockDir)
    {
        ExReleaseResourceLite(&((PDEVICE_EXTENSION)IrpContext->DeviceObject->DeviceExtension)->DirResource);
    }

    IrpContext->Irp->IoStatus.Information = 0;
    return Status;
}

/* EOF */
