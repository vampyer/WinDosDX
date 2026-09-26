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
 * FILE:             drivers/filesystem/ntfs/dirctl.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Hervé Poussineau (hpoussin@reactos.org)
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Retrieve the standard file information
 */
static
NTSTATUS
NtfsGetStandardInformation(PNTFS_FCB Fcb,
                           PDEVICE_OBJECT DeviceObject,
                           PFILE_STANDARD_INFORMATION StandardInfo,
                           PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    DPRINT("NtfsGetStandardInformation(%p, %p, %p, %p)\n", Fcb, DeviceObject, StandardInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_STANDARD_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    /* PRECONDITION */
    ASSERT(StandardInfo != NULL);
    ASSERT(Fcb != NULL);

    RtlZeroMemory(StandardInfo,
                  sizeof(FILE_STANDARD_INFORMATION));

    StandardInfo->AllocationSize = Fcb->RFCB.AllocationSize;
    StandardInfo->EndOfFile = Fcb->RFCB.FileSize;
    StandardInfo->NumberOfLinks = Fcb->LinkCount;
    StandardInfo->DeletePending = BooleanFlagOn(Fcb->Flags, FCB_DELETE_PENDING);
    StandardInfo->Directory = NtfsFCBIsDirectory(Fcb);

    *BufferLength -= sizeof(FILE_STANDARD_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetPositionInformation(PFILE_OBJECT FileObject,
                           PFILE_POSITION_INFORMATION PositionInfo,
                           PULONG BufferLength)
{
    DPRINT1("NtfsGetPositionInformation(%p, %p, %p)\n", FileObject, PositionInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_POSITION_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    PositionInfo->CurrentByteOffset.QuadPart = FileObject->CurrentByteOffset.QuadPart;

    DPRINT("Getting position %I64x\n",
           PositionInfo->CurrentByteOffset.QuadPart);

    *BufferLength -= sizeof(FILE_POSITION_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetBasicInformation(PFILE_OBJECT FileObject,
                        PNTFS_FCB Fcb,
                        PDEVICE_OBJECT DeviceObject,
                        PFILE_BASIC_INFORMATION BasicInfo,
                        PULONG BufferLength)
{
    PFILENAME_ATTRIBUTE FileName = &Fcb->Entry;

    DPRINT("NtfsGetBasicInformation(%p, %p, %p, %p, %p)\n", FileObject, Fcb, DeviceObject, BasicInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(BasicInfo, sizeof(FILE_BASIC_INFORMATION));

    BasicInfo->CreationTime.QuadPart = FileName->CreationTime;
    BasicInfo->LastAccessTime.QuadPart = FileName->LastAccessTime;
    BasicInfo->LastWriteTime.QuadPart = FileName->LastWriteTime;
    BasicInfo->ChangeTime.QuadPart = FileName->ChangeTime;

    NtfsFileFlagsToAttributes(FileName->FileAttributes, &BasicInfo->FileAttributes);

    *BufferLength -= sizeof(FILE_BASIC_INFORMATION);

    return STATUS_SUCCESS;
}


/*
 * FUNCTION: Retrieve the file name information
 */
static
NTSTATUS
NtfsGetNameInformation(PFILE_OBJECT FileObject,
                       PNTFS_FCB Fcb,
                       PDEVICE_OBJECT DeviceObject,
                       PFILE_NAME_INFORMATION NameInfo,
                       PULONG BufferLength)
{
    ULONG BytesToCopy;

    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DeviceObject);

    DPRINT("NtfsGetNameInformation(%p, %p, %p, %p, %p)\n", FileObject, Fcb, DeviceObject, NameInfo, BufferLength);

    ASSERT(NameInfo != NULL);
    ASSERT(Fcb != NULL);

    /* If buffer can't hold at least the file name length, bail out */
    if (*BufferLength < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
        return STATUS_BUFFER_TOO_SMALL;

    /* Save file name length, and as much file len, as buffer length allows */
    NameInfo->FileNameLength = wcslen(Fcb->PathName) * sizeof(WCHAR);

    /* Calculate amount of bytes to copy not to overflow the buffer */
    BytesToCopy = min(NameInfo->FileNameLength,
                      *BufferLength - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]));

    /* Fill in the bytes */
    RtlCopyMemory(NameInfo->FileName, Fcb->PathName, BytesToCopy);

    /* Check if we could write more but are not able to */
    if (*BufferLength < NameInfo->FileNameLength + (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
    {
        /* Return number of bytes written */
        *BufferLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + BytesToCopy;
        return STATUS_BUFFER_OVERFLOW;
    }

    /* We filled up as many bytes, as needed */
    *BufferLength -= (FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + NameInfo->FileNameLength);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetInternalInformation(PNTFS_FCB Fcb,
                           PFILE_INTERNAL_INFORMATION InternalInfo,
                           PULONG BufferLength)
{
    DPRINT1("NtfsGetInternalInformation(%p, %p, %p)\n", Fcb, InternalInfo, BufferLength);

    ASSERT(InternalInfo);
    ASSERT(Fcb);

    if (*BufferLength < sizeof(FILE_INTERNAL_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    InternalInfo->IndexNumber.QuadPart = Fcb->MFTIndex;

    *BufferLength -= sizeof(FILE_INTERNAL_INFORMATION);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsGetNetworkOpenInformation(PNTFS_FCB Fcb,
                              PDEVICE_EXTENSION DeviceExt,
                              PFILE_NETWORK_OPEN_INFORMATION NetworkInfo,
                              PULONG BufferLength)
{
    PFILENAME_ATTRIBUTE FileName = &Fcb->Entry;

    DPRINT("NtfsGetNetworkOpenInformation(%p, %p, %p, %p)\n", Fcb, DeviceExt, NetworkInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_NETWORK_OPEN_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    NetworkInfo->CreationTime.QuadPart = FileName->CreationTime;
    NetworkInfo->LastAccessTime.QuadPart = FileName->LastAccessTime;
    NetworkInfo->LastWriteTime.QuadPart = FileName->LastWriteTime;
    NetworkInfo->ChangeTime.QuadPart = FileName->ChangeTime;

    NetworkInfo->EndOfFile = Fcb->RFCB.FileSize;
    NetworkInfo->AllocationSize = Fcb->RFCB.AllocationSize;

    NtfsFileFlagsToAttributes(FileName->FileAttributes, &NetworkInfo->FileAttributes);

    *BufferLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsGetStreamInformation(PNTFS_FCB Fcb,
                         PDEVICE_EXTENSION DeviceExt,
                         PFILE_STREAM_INFORMATION StreamInfo,
                         PULONG BufferLength)
{
    ULONG CurrentSize;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    NTSTATUS Status, BrowseStatus;
    PFILE_RECORD_HEADER FileRecord;
    PFILE_STREAM_INFORMATION CurrentInfo = StreamInfo, Previous = NULL;

    if (*BufferLength < sizeof(FILE_STREAM_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Not enough memory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Can't find record!\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    BrowseStatus = FindFirstAttribute(&Context, DeviceExt, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(BrowseStatus))
    {
        if (Attribute->Type == AttributeData)
        {
            CurrentSize = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) + Attribute->NameLength * sizeof(WCHAR) + wcslen(L"::$DATA") * sizeof(WCHAR);

            if (CurrentSize > *BufferLength)
            {
                Status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            CurrentInfo->NextEntryOffset = 0;
            CurrentInfo->StreamNameLength = (Attribute->NameLength + wcslen(L"::$DATA")) * sizeof(WCHAR);
            CurrentInfo->StreamSize.QuadPart = AttributeDataLength(Attribute);
            CurrentInfo->StreamAllocationSize.QuadPart = AttributeAllocatedLength(Attribute);
            CurrentInfo->StreamName[0] = L':';
            RtlMoveMemory(&CurrentInfo->StreamName[1], (PWCHAR)((ULONG_PTR)Attribute + Attribute->NameOffset), CurrentInfo->StreamNameLength);
            RtlMoveMemory(&CurrentInfo->StreamName[Attribute->NameLength + 1], L":$DATA", sizeof(L":$DATA") - sizeof(UNICODE_NULL));

            if (Previous != NULL)
            {
                Previous->NextEntryOffset = (ULONG_PTR)CurrentInfo - (ULONG_PTR)Previous;
            }
            Previous = CurrentInfo;
            CurrentInfo = (PFILE_STREAM_INFORMATION)((ULONG_PTR)CurrentInfo + CurrentSize);
            *BufferLength -= CurrentSize;
        }

        BrowseStatus = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

// Convert enum value to friendly name
const PCSTR
GetInfoClassName(FILE_INFORMATION_CLASS infoClass)
{
    const PCSTR fileInfoClassNames[] = { "???????",
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
        "FileIoCompletionNotificationInformation",
        "FileIoStatusBlockRangeInformation",
        "FileIoPriorityHintInformation",
        "FileSfioReserveInformation",
        "FileSfioVolumeInformation",
        "FileHardLinkInformation",
        "FileProcessIdsUsingFileInformation",
        "FileNormalizedNameInformation",
        "FileNetworkPhysicalNameInformation",
        "FileIdGlobalTxDirectoryInformation",
        "FileIsRemoteDeviceInformation",
        "FileAttributeCacheInformation",
        "FileNumaNodeInformation",
        "FileStandardLinkInformation",
        "FileRemoteProtocolInformation",
        "FileReplaceCompletionInformation",
        "FileMaximumInformation",
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
        "FileIoCompletionNotificationInformation",
        "FileIoStatusBlockRangeInformation",
        "FileIoPriorityHintInformation",
        "FileSfioReserveInformation",
        "FileSfioVolumeInformation",
        "FileHardLinkInformation",
        "FileProcessIdsUsingFileInformation",
        "FileNormalizedNameInformation",
        "FileNetworkPhysicalNameInformation",
        "FileIdGlobalTxDirectoryInformation",
        "FileIsRemoteDeviceInformation",
        "FileAttributeCacheInformation",
        "FileNumaNodeInformation",
        "FileStandardLinkInformation",
        "FileRemoteProtocolInformation",
        "FileReplaceCompletionInformation",
        "FileMaximumInformation" };
    return fileInfoClassNames[infoClass];
}

/*
 * FUNCTION: Retrieve the specified file information
 */
NTSTATUS
NtfsQueryInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT1("NtfsQueryInformation(%p)\n", IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceObject = IrpContext->DeviceObject;
    FileInformationClass = Stack->Parameters.QueryFile.FileInformationClass;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = Stack->Parameters.QueryFile.Length;

    if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    switch (FileInformationClass)
    {
        case FileStandardInformation:
            Status = NtfsGetStandardInformation(Fcb,
                                                DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FilePositionInformation:
            Status = NtfsGetPositionInformation(FileObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileBasicInformation:
            Status = NtfsGetBasicInformation(FileObject,
                                             Fcb,
                                             DeviceObject,
                                             SystemBuffer,
                                             &BufferLength);
            break;

        case FileNameInformation:
            Status = NtfsGetNameInformation(FileObject,
                                            Fcb,
                                            DeviceObject,
                                            SystemBuffer,
                                            &BufferLength);
            break;

        case FileInternalInformation:
            Status = NtfsGetInternalInformation(Fcb,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileNetworkOpenInformation:
            Status = NtfsGetNetworkOpenInformation(Fcb,
                                                   DeviceObject->DeviceExtension,
                                                   SystemBuffer,
                                                   &BufferLength);
            break;

        case FileStreamInformation:
            Status = NtfsGetStreamInformation(Fcb,
                                              DeviceObject->DeviceExtension,
                                              SystemBuffer,
                                              &BufferLength);
            break;

        case FileAttributeTagInformation:
            if (BufferLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
            }
            else
            {
                PFILE_ATTRIBUTE_TAG_INFORMATION TagInfo = SystemBuffer;

                TagInfo->FileAttributes = Fcb->Entry.FileAttributes & 0xFFFF;
                if (NtfsFCBIsDirectory(Fcb))
                    TagInfo->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
                if (TagInfo->FileAttributes == 0)
                    TagInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;
                TagInfo->ReparseTag = NtfsFCBIsReparsePoint(Fcb) ? Fcb->Entry.Extended.ReparseTag : 0;
                BufferLength -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
            }
            break;

        case FileAlternateNameInformation:
        case FileAllInformation:
            DPRINT1("Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        default:
            DPRINT1("Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_INVALID_PARAMETER;
    }

    ExReleaseResourceLite(&Fcb->MainResource);

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information =
            Stack->Parameters.QueryFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}

/**
* @name NtfsSetEndOfFile
* @implemented
*
* Sets the end of file (file size) for a given file.
*
* @param Fcb
* Pointer to an NTFS_FCB which describes the target file. Fcb->MainResource should have been
* acquired with ExAcquireResourceSharedLite().
*
* @param FileObject
* Pointer to a FILE_OBJECT describing the target file.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param IrpFlags
* ULONG describing the flags of the original IRP request (Irp->Flags).
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application opened the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param NewFileSize
* Pointer to a LARGE_INTEGER which indicates the new end of file (file size).
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_USER_MAPPED_FILE if trying to truncate a file but MmCanFileBeTruncated() returned false,
* STATUS_OBJECT_NAME_NOT_FOUND if there was no $DATA attribute associated with the target file,
* STATUS_INVALID_PARAMETER if there was no $FILENAME attribute associated with the target file,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_ACCESS_DENIED if target file is a volume or if paging is involved.
*
* @remarks As this function sets the size of a file at the file-level
* (and not at the attribute level) it's not recommended to use this
* function alongside functions that operate on the data attribute directly.
*
*/
NTSTATUS
NtfsSetEndOfFile(PNTFS_FCB Fcb,
                 PFILE_OBJECT FileObject,
                 PDEVICE_EXTENSION DeviceExt,
                 ULONG IrpFlags,
                 BOOLEAN CaseSensitive,
                 PLARGE_INTEGER NewFileSize)
{
    LARGE_INTEGER CurrentFileSize;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    PNTFS_ATTR_CONTEXT AttributeListContext = NULL;
    ULONG AttributeOffset;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONGLONG AllocationSize;
    PFILENAME_ATTRIBUTE FileNameAttribute;
    ULONGLONG ParentMFTId;
    UNICODE_STRING FileName;

    if (NtfsFCBIsCompressed(Fcb) || NtfsFCBIsEncrypted(Fcb) ||
        NtfsFCBIsSparse(Fcb))
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    // Allocate non-paged memory for the file record
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Couldn't allocate memory for file record!");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // read the file record
    DPRINT("Reading file record...\n");
    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        // We couldn't get the file's record. Free the memory and return the error
        DPRINT1("Can't find record for %wS!\n", Fcb->ObjectName);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    DPRINT("Found record for %wS\n", Fcb->ObjectName);

    Status = FindAttribute(DeviceExt, FileRecord, AttributeAttributeList,
                           L"", 0, &AttributeListContext, NULL);
    if (NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(AttributeListContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_NOT_IMPLEMENTED;
    }

    CurrentFileSize.QuadPart = NtfsGetFileSize(DeviceExt, FileRecord, L"", 0, NULL);

    // Are we trying to decrease the file size?
    if (NewFileSize->QuadPart < CurrentFileSize.QuadPart)
    {
        // Is the file mapped?
        if (!MmCanFileBeTruncated(FileObject->SectionObjectPointer,
                                  NewFileSize))
        {
            DPRINT1("Couldn't decrease file size!\n");
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_USER_MAPPED_FILE;
        }
    }

    // Find the attribute with the data stream for our file
    DPRINT("Finding Data Attribute...\n");
    Status = FindAttribute(DeviceExt,
                           FileRecord,
                           AttributeData,
                           Fcb->Stream,
                           wcslen(Fcb->Stream),
                           &DataContext,
                           &AttributeOffset);

    // Did we fail to find the attribute?
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No '%S' data stream associated with file!\n", Fcb->Stream);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // Get the size of the data attribute
    CurrentFileSize.QuadPart = AttributeDataLength(DataContext->pRecord);

    // Are we enlarging the attribute?
    if (NewFileSize->QuadPart > CurrentFileSize.QuadPart)
    {
        // is increasing the stream size not allowed?
        if ((Fcb->Flags & FCB_IS_VOLUME) ||
            (IrpFlags & IRP_PAGING_IO))
        {
            // TODO - just fail for now
            ReleaseAttributeContext(DataContext);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_ACCESS_DENIED;
        }
    }

    Status = NtfsPrepareForMetadataUpdate(DeviceExt);
    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // set the attribute data length
    Status = SetAttributeDataLength(FileObject, Fcb, DataContext, AttributeOffset, FileRecord, NewFileSize);
    if (!NT_SUCCESS(Status))
    {
        NtfsMarkJournalFailure(DeviceExt, 0x0301);
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // now we need to update this file's size in every directory index entry that references it
    // TODO: expand to work with every filename / hardlink stored in the file record.
    FileNameAttribute = GetBestFileNameFromRecord(Fcb->Vcb, FileRecord);
    if (FileNameAttribute == NULL)
    {
        DPRINT1("Unable to find FileName attribute associated with file!\n");
        NtfsMarkJournalFailure(DeviceExt, 0x0302);
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_INVALID_PARAMETER;
    }

    ParentMFTId = FileNameAttribute->DirectoryFileReferenceNumber & NTFS_MFT_MASK;

    FileName.Buffer = FileNameAttribute->Name;
    FileName.Length = FileNameAttribute->NameLength * sizeof(WCHAR);
    FileName.MaximumLength = FileName.Length;

    AllocationSize = AttributeAllocatedLength(DataContext->pRecord);

    Status = UpdateFileNameRecord(Fcb->Vcb,
                                  ParentMFTId,
                                  &FileName,
                                  FALSE,
                                  NewFileSize->QuadPart,
                                  AllocationSize,
                                  CaseSensitive);
    if (!NT_SUCCESS(Status))
    {
        NtfsMarkJournalFailure(DeviceExt, 0x0303);
    }

    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/*
 * Clears the volume bitmap bits of every cluster that the non-resident
 * attributes of Record use.
 */
static
NTSTATUS
NtfsFreeRecordClusters(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER Record)
{
    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    ULONGLONG BitmapDataSize;
    PUCHAR BitmapData = NULL;
    RTL_BITMAP Bitmap;
    BOOLEAN Changed = FALSE;
    ULONG Written;
    NTSTATUS Status;

    BitmapRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!BitmapRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, NTFS_FILE_BITMAP, BitmapRecord);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, BitmapRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DataContext = NULL;
        goto Cleanup;
    }

    BitmapDataSize = AttributeDataLength(DataContext->pRecord);
    if (BitmapDataSize * 8 < Vcb->NtfsInfo.ClusterCount || BitmapDataSize > MAXULONG)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    BitmapData = ExAllocatePoolWithTag(NonPagedPool,
                                       ROUND_UP(BitmapDataSize, Vcb->NtfsInfo.BytesPerSector),
                                       TAG_NTFS);
    if (!BitmapData)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    if (ReadAttribute(Vcb, DataContext, 0, (PCHAR)BitmapData, (ULONG)BitmapDataSize) != BitmapDataSize)
    {
        Status = STATUS_UNEXPECTED_IO_ERROR;
        goto Cleanup;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, (ULONG)Vcb->NtfsInfo.ClusterCount);

    Status = FindFirstAttribute(&Context, Vcb, Record, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->IsNonResident)
        {
            PUCHAR DataRun = (PUCHAR)Attribute + Attribute->NonResident.MappingPairsOffset;
            PUCHAR RunEnd = (PUCHAR)Attribute + Attribute->Length;
            LONGLONG Lcn = 0;

            while (DataRun < RunEnd && *DataRun != 0)
            {
                LONGLONG RunOffset;
                ULONGLONG RunLength;

                DataRun = DecodeRun(DataRun, &RunOffset, &RunLength);
                if (RunOffset == -1)
                    continue;   /* sparse */

                Lcn += RunOffset;
                if (Lcn >= 0 && (ULONGLONG)Lcn + RunLength <= Vcb->NtfsInfo.ClusterCount)
                {
                    RtlClearBits(&Bitmap, (ULONG)Lcn, (ULONG)RunLength);
                    Changed = TRUE;
                }
                else
                {
                    DPRINT1("Ignoring out-of-range run %I64d+%I64u in record %lu\n", Lcn, RunLength, Record->MFTRecordNumber);
                }
            }
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }
    FindCloseAttribute(&Context);

    Status = STATUS_SUCCESS;
    if (Changed)
    {
        /* NULL: the $Bitmap attribute belongs to record 6, not to Record. */
        Status = WriteAttribute(Vcb, DataContext, 0, BitmapData, (ULONG)BitmapDataSize, &Written, NULL);
        if (NT_SUCCESS(Status) && Written != BitmapDataSize)
            Status = STATUS_END_OF_FILE;
    }

Cleanup:
    if (BitmapData)
        ExFreePoolWithTag(BitmapData, TAG_NTFS);
    if (DataContext)
        ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);
    return Status;
}

/**
* @name NtfsDeleteFileRecord
*
* Deletes file MftIndex: removes its names from every directory that lists
* it, frees its file record and then the clusters its attributes used.
*
* @return
* STATUS_DIRECTORY_NOT_EMPTY for a directory that still has entries.
* STATUS_NOT_IMPLEMENTED for files with an attribute list.
*
* @remarks
* The caller holds DirResource. Nothing is changed when this fails before
* the first name is removed; failures after that mark the volume dirty.
*/
NTSTATUS
NtfsDeleteFileRecord(PDEVICE_EXTENSION Vcb,
                     ULONGLONG MftIndex)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT ListContext;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    ULONGLONG Parents[8];
    ULONG ParentCount = 0;
    ULONG i;
    NTSTATUS Status;

    if (MftIndex < NTFS_FILE_FIRST_USER_FILE)
        return STATUS_CANNOT_DELETE;

    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, MftIndex, Record);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (!(Record->Flags & FRH_IN_USE))
    {
        Status = STATUS_SUCCESS;
        goto Cleanup;
    }

    if (NT_SUCCESS(FindAttribute(Vcb, Record, AttributeAttributeList, L"", 0, &ListContext, NULL)))
    {
        ReleaseAttributeContext(ListContext);
        Status = STATUS_NOT_IMPLEMENTED;
        goto Cleanup;
    }

    if (Record->Flags & FRH_DIRECTORY)
    {
        BOOLEAN Empty;

        Status = NtfsIndexIsEmpty(Vcb, MftIndex, &Empty);
        if (NT_SUCCESS(Status) && !Empty)
            Status = STATUS_DIRECTORY_NOT_EMPTY;
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }

    /* Every directory that lists one of the file's names. */
    Status = FindFirstAttribute(&Context, Vcb, Record, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == AttributeFileName && !Attribute->IsNonResident)
        {
            PFILENAME_ATTRIBUTE Name = (PFILENAME_ATTRIBUTE)((PUCHAR)Attribute + Attribute->Resident.ValueOffset);
            ULONGLONG Parent = Name->DirectoryFileReferenceNumber & NTFS_MFT_MASK;

            for (i = 0; i < ParentCount && Parents[i] != Parent; i++);
            if (i == ParentCount && ParentCount < ARRAYSIZE(Parents))
                Parents[ParentCount++] = Parent;
        }
        Status = FindNextAttribute(&Context, &Attribute);
    }
    FindCloseAttribute(&Context);

    if (ParentCount == 0)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    Status = NtfsPrepareForMetadataUpdate(Vcb);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* 1. Unlink: once no directory lists the file, it is gone. */
    for (i = 0; i < ParentCount; i++)
    {
        Status = NtfsIndexUpdate(Vcb, Parents[i], TRUE, MftIndex, NULL, 0, FALSE, NULL);
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            Status = STATUS_SUCCESS;
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Removing file %I64u from directory %I64u failed (0x%08lx)\n", MftIndex, Parents[i], Status);
            if (i > 0)
                NtfsMarkJournalFailure(Vcb, 0x0701);
            goto Cleanup;
        }
    }

    /* 2. Release the file record (this marks its own failures). */
    Status = RemoveNewMftEntry(Vcb, MftIndex);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Releasing file record %I64u failed (0x%08lx)\n", MftIndex, Status);
        goto Cleanup;
    }

    /* 3. Free the clusters; if this fails they only leak. */
    Status = NtfsFreeRecordClusters(Vcb, Record);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Freeing the clusters of file %I64u failed (0x%08lx)\n", MftIndex, Status);
        NtfsMarkJournalFailure(Vcb, 0x0702);
    }

Cleanup:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    return Status;
}

static
NTSTATUS
NtfsSetDispositionInformation(PFILE_OBJECT FileObject,
                              PDEVICE_EXTENSION DeviceExt,
                              PNTFS_FCB Fcb,
                              PFILE_DISPOSITION_INFORMATION DispositionInfo)
{
    DPRINT("NtfsSetDispositionInformation(%p, %p, %p, %u)\n", FileObject, DeviceExt, Fcb, DispositionInfo->DeleteFile);

    if (!DispositionInfo->DeleteFile)
    {
        Fcb->Flags &= ~FCB_DELETE_PENDING;
        FileObject->DeletePending = FALSE;
        return STATUS_SUCCESS;
    }

    if (Fcb->Flags & FCB_DELETE_PENDING)
    {
        FileObject->DeletePending = TRUE;
        return STATUS_SUCCESS;
    }

    if ((Fcb->Flags & FCB_IS_VOLUME) ||
        Fcb->MFTIndex < NTFS_FILE_FIRST_USER_FILE ||
        NtfsFCBIsRoot(Fcb) ||
        (Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_READ_ONLY))
    {
        return STATUS_CANNOT_DELETE;
    }

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_ACCESS_DENIED;
    if (DeviceExt->Flags & VCB_VOLUME_DIRTY)
        return STATUS_VOLUME_DIRTY;

    if (NtfsFCBIsDirectory(Fcb))
    {
        BOOLEAN Empty;
        NTSTATUS Status = NtfsIndexIsEmpty(DeviceExt, Fcb->MFTIndex, &Empty);

        if (!NT_SUCCESS(Status))
            return Status;
        if (!Empty)
            return STATUS_DIRECTORY_NOT_EMPTY;
    }
    else if (!MmFlushImageSection(FileObject->SectionObjectPointer, MmFlushForDelete))
    {
        return STATUS_CANNOT_DELETE;
    }

    /* The file goes away when its last handle is cleaned up. */
    Fcb->Flags |= FCB_DELETE_PENDING;
    FileObject->DeletePending = TRUE;
    return STATUS_SUCCESS;
}

/* Replaces all $FILE_NAME attributes of Record with one holding NewName. */
static
NTSTATUS
NtfsReplaceFileNames(PDEVICE_EXTENSION DeviceExt,
                     PFILE_RECORD_HEADER Record,
                     PFILENAME_ATTRIBUTE NewName,
                     ULONG NewNameLength)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG AttributeLength = ALIGN_UP_BY(ResidentHeaderLength + NewNameLength, ATTR_RECORD_ALIGNMENT);
    ULONG Offset = Record->AttributeOffset;
    ULONG InsertAt = 0;
    ULONG Removed = 0;
    PNTFS_ATTR_RECORD Attribute;

    /* Measure first, so a record that can't hold the new name is left untouched. */
    for (;;)
    {
        Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Offset);
        if (Attribute->Type == AttributeEnd)
            break;
        if (Attribute->Length == 0 || Offset + Attribute->Length > Record->BytesInUse)
            return STATUS_FILE_CORRUPT_ERROR;
        if (Attribute->Type == AttributeFileName)
            Removed += Attribute->Length;
        Offset += Attribute->Length;
    }

    if (Record->BytesInUse - Removed + AttributeLength > DeviceExt->NtfsInfo.BytesPerFileRecord)
        return STATUS_NOT_IMPLEMENTED;

    Offset = Record->AttributeOffset;
    for (;;)
    {
        Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Offset);
        if (Attribute->Type == AttributeEnd)
            break;

        if (Attribute->Type == AttributeFileName)
        {
            ULONG Length = Attribute->Length;

            if (!InsertAt)
                InsertAt = Offset;
            RtlMoveMemory(Attribute, (PUCHAR)Attribute + Length, Record->BytesInUse - Offset - Length);
            Record->BytesInUse -= Length;
            continue;
        }

        /* Attributes are sorted by type. */
        if (Attribute->Type > AttributeFileName && !InsertAt)
            InsertAt = Offset;
        Offset += Attribute->Length;
    }
    if (!InsertAt)
        InsertAt = Offset;

    RtlMoveMemory((PUCHAR)Record + InsertAt + AttributeLength,
                  (PUCHAR)Record + InsertAt,
                  Record->BytesInUse - InsertAt);
    Record->BytesInUse += AttributeLength;

    Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Record + InsertAt);
    RtlZeroMemory(Attribute, AttributeLength);
    Attribute->Type = AttributeFileName;
    Attribute->Length = AttributeLength;
    Attribute->NameOffset = (USHORT)ResidentHeaderLength;
    Attribute->Instance = Record->NextAttributeNumber++;
    Attribute->Resident.ValueLength = NewNameLength;
    Attribute->Resident.ValueOffset = (USHORT)ResidentHeaderLength;
    Attribute->Resident.Flags = RA_INDEXED;
    RtlCopyMemory((PUCHAR)Attribute + ResidentHeaderLength, NewName, NewNameLength);

    Record->LinkCount = 1;
    return STATUS_SUCCESS;
}

/**
* @name NtfsSetRenameInformation
*
* Renames or moves a file or directory within the volume. The target
* directory comes from TargetFileObject (opened by the I/O manager with
* SL_OPEN_TARGET_DIRECTORY, its name reduced to the new name), or is the
* file's own directory when the new name is a plain name.
*/
static
NTSTATUS
NtfsSetRenameInformation(PDEVICE_EXTENSION DeviceExt,
                         PNTFS_FCB Fcb,
                         PFILE_RENAME_INFORMATION RenameInfo,
                         PFILE_OBJECT TargetFileObject,
                         BOOLEAN CaseSensitive)
{
    PFILE_RECORD_HEADER Record = NULL;
    PFILENAME_ATTRIBUTE OldName, NewName = NULL;
    ULONG NewNameLength;
    UNICODE_STRING NewComponent;
    WCHAR TargetDirPath[MAX_PATH];
    WCHAR NewPath[MAX_PATH];
    WCHAR OldPath[MAX_PATH];
    ULONGLONG TargetDirMft, OldParentMft, TargetDirReference;
    ULONGLONG FileReference;
    ULONGLONG ExistingMft;
    ULONG FirstEntry = 0;
    ULONG i;
    LARGE_INTEGER SystemTime;
    NTSTATUS Status;

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_ACCESS_DENIED;
    if (DeviceExt->Flags & VCB_VOLUME_DIRTY)
        return STATUS_VOLUME_DIRTY;

    if ((Fcb->Flags & (FCB_IS_VOLUME | FCB_DELETE_PENDING | FCB_IS_DELETED)) ||
        Fcb->MFTIndex < NTFS_FILE_FIRST_USER_FILE ||
        NtfsFCBIsRoot(Fcb))
    {
        return STATUS_ACCESS_DENIED;
    }

    Record = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, Record);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    OldName = GetBestFileNameFromRecord(DeviceExt, Record);
    if (!OldName)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }
    OldParentMft = OldName->DirectoryFileReferenceNumber & NTFS_MFT_MASK;

    /* Where the file goes, and under which name. */
    if (TargetFileObject)
    {
        PNTFS_FCB TargetDirFcb = TargetFileObject->FsContext;

        if (!TargetDirFcb || !NtfsFCBIsDirectory(TargetDirFcb))
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }
        TargetDirMft = TargetDirFcb->MFTIndex;
        wcscpy(TargetDirPath, TargetDirFcb->PathName);
        NewComponent = TargetFileObject->FileName;
    }
    else
    {
        PWCHAR LastSlash;

        if (RenameInfo->RootDirectory)
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }
        TargetDirMft = OldParentMft;
        wcscpy(TargetDirPath, Fcb->PathName);
        LastSlash = wcsrchr(TargetDirPath, L'\\');
        if (!LastSlash)
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }
        LastSlash[(LastSlash == TargetDirPath) ? 1 : 0] = UNICODE_NULL;
        NewComponent.Buffer = RenameInfo->FileName;
        NewComponent.Length = NewComponent.MaximumLength = (USHORT)RenameInfo->FileNameLength;
    }

    if (NewComponent.Length == 0 || NewComponent.Length > 255 * sizeof(WCHAR))
    {
        Status = STATUS_OBJECT_NAME_INVALID;
        goto Cleanup;
    }
    for (i = 0; i < NewComponent.Length / sizeof(WCHAR); i++)
    {
        WCHAR c = NewComponent.Buffer[i];

        if (c == L'\\' || c == L'/' || c == L':' || c == UNICODE_NULL)
        {
            Status = STATUS_OBJECT_NAME_INVALID;
            goto Cleanup;
        }
    }

    if (wcslen(TargetDirPath) + 1 + NewComponent.Length / sizeof(WCHAR) >= MAX_PATH)
    {
        Status = STATUS_OBJECT_NAME_INVALID;
        goto Cleanup;
    }
    wcscpy(NewPath, TargetDirPath);
    if (wcscmp(NewPath, L"\\") != 0)
        wcscat(NewPath, L"\\");
    i = (ULONG)wcslen(NewPath);
    RtlCopyMemory(&NewPath[i], NewComponent.Buffer, NewComponent.Length);
    NewPath[i + NewComponent.Length / sizeof(WCHAR)] = UNICODE_NULL;

    /* A directory can't move below itself. */
    if (NtfsFCBIsDirectory(Fcb))
    {
        SIZE_T Length = wcslen(Fcb->PathName);

        if (TargetDirMft == Fcb->MFTIndex ||
            (_wcsnicmp(TargetDirPath, Fcb->PathName, Length) == 0 && TargetDirPath[Length] == L'\\'))
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }
    }

    /* Replace an existing target, if allowed and possible. */
    Status = NtfsIndexLookup(DeviceExt, TargetDirMft, &NewComponent, &FirstEntry, FALSE, CaseSensitive, &ExistingMft);
    if (NT_SUCCESS(Status) && ExistingMft != Fcb->MFTIndex)
    {
        PFILE_RECORD_HEADER TargetRecord;
        PNTFS_FCB TargetFcb;

        if (!RenameInfo->ReplaceIfExists)
        {
            Status = STATUS_OBJECT_NAME_COLLISION;
            goto Cleanup;
        }

        TargetRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        if (!TargetRecord)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        Status = ReadFileRecord(DeviceExt, ExistingMft, TargetRecord);
        if (NT_SUCCESS(Status) && (TargetRecord->Flags & FRH_DIRECTORY))
            Status = STATUS_ACCESS_DENIED;
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, TargetRecord);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        TargetFcb = NtfsGrabFCBFromTable(DeviceExt, NewPath);
        if (TargetFcb)
        {
            if (TargetFcb->OpenHandleCount != 0 || TargetFcb->MFTIndex != ExistingMft)
            {
                NtfsReleaseFCB(DeviceExt, TargetFcb);
                Status = STATUS_ACCESS_DENIED;
                goto Cleanup;
            }

            /* No handles, only cached data: drop it, the file is about to go. */
            CcPurgeCacheSection(&TargetFcb->SectionObjectPointers, NULL, 0, FALSE);
            NtfsRemoveFCBFromTable(DeviceExt, TargetFcb);
            TargetFcb->Flags |= FCB_IS_DELETED;
            NtfsReleaseFCB(DeviceExt, TargetFcb);
        }

        Status = NtfsDeleteFileRecord(DeviceExt, ExistingMft);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }
    else if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_PATH_NOT_FOUND)
    {
        goto Cleanup;
    }

    /* The new $FILE_NAME: same times and sizes, new parent and name. */
    if (TargetDirMft == OldParentMft)
    {
        TargetDirReference = OldName->DirectoryFileReferenceNumber;
    }
    else
    {
        PFILE_RECORD_HEADER DirRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);

        if (!DirRecord)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        Status = ReadFileRecord(DeviceExt, TargetDirMft, DirRecord);
        TargetDirReference = TargetDirMft | ((ULONGLONG)DirRecord->SequenceNumber << 48);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DirRecord);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }

    NewNameLength = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + NewComponent.Length;
    NewName = ExAllocatePoolWithTag(NonPagedPool, NewNameLength, TAG_NTFS);
    if (!NewName)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlCopyMemory(NewName, OldName, FIELD_OFFSET(FILENAME_ATTRIBUTE, NameLength));
    KeQuerySystemTime(&SystemTime);
    NewName->DirectoryFileReferenceNumber = TargetDirReference;
    NewName->ChangeTime = SystemTime.QuadPart;
    NewName->NameLength = (UCHAR)(NewComponent.Length / sizeof(WCHAR));
    /* Same choice as AddFileName() */
    NewName->NameType = CaseSensitive ? NTFS_FILE_NAME_POSIX : NTFS_FILE_NAME_WIN32_AND_DOS;
    RtlCopyMemory(NewName->Name, NewComponent.Buffer, NewComponent.Length);

    FileReference = Fcb->MFTIndex | ((ULONGLONG)Record->SequenceNumber << 48);

    Status = NtfsPrepareForMetadataUpdate(DeviceExt);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* The new name is listed before the old one goes: a crash leaves both, never neither. */
    if (TargetDirMft == OldParentMft)
    {
        Status = NtfsIndexUpdate(DeviceExt, TargetDirMft, TRUE, Fcb->MFTIndex,
                                 NewName, FileReference, CaseSensitive, NULL);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }
    else
    {
        Status = NtfsIndexUpdate(DeviceExt, TargetDirMft, FALSE, 0,
                                 NewName, FileReference, CaseSensitive, NULL);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        Status = NtfsIndexUpdate(DeviceExt, OldParentMft, TRUE, Fcb->MFTIndex,
                                 NULL, 0, FALSE, NULL);
        if (!NT_SUCCESS(Status))
        {
            NtfsMarkJournalFailure(DeviceExt, 0x0801);
            goto Cleanup;
        }
    }

    Status = NtfsReplaceFileNames(DeviceExt, Record, NewName, NewNameLength);
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, Record);
    if (!NT_SUCCESS(Status))
    {
        NtfsMarkJournalFailure(DeviceExt, 0x0802);
        goto Cleanup;
    }

    Fcb->Entry.DirectoryFileReferenceNumber = TargetDirReference;
    Fcb->Entry.ChangeTime = NewName->ChangeTime;
    Fcb->Entry.NameType = NewName->NameType;
    Fcb->LinkCount = 1;

    wcscpy(OldPath, Fcb->PathName);
    Status = NtfsRenameFCBPaths(DeviceExt, OldPath, NewPath);
    if (!NT_SUCCESS(Status))
    {
        /* The rename is on disk; only cached paths are stale. */
        DPRINT1("Renamed %S to %S, but cached paths couldn't follow (0x%08lx)\n", OldPath, NewPath, Status);
        Status = STATUS_SUCCESS;
    }

Cleanup:
    if (NewName)
        ExFreePoolWithTag(NewName, TAG_NTFS);
    if (Record)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Record);
    return Status;
}

/**
* @name NtfsSetInformation
* @implemented
*
* Sets the specified file information.
*
* @param IrpContext
* Points to an NTFS_IRP_CONTEXT which describes the set operation
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_NOT_IMPLEMENTED if trying to set an unimplemented information class,
* STATUS_USER_MAPPED_FILE if trying to truncate a file but MmCanFileBeTruncated() returned false,
* STATUS_OBJECT_NAME_NOT_FOUND if there was no $DATA attribute associated with the target file,
* STATUS_INVALID_PARAMETER if there was no $FILENAME attribute associated with the target file,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_ACCESS_DENIED if target file is a volume or if paging is involved.
*
* @remarks Called by NtfsDispatch() in response to an IRP_MJ_SET_INFORMATION request.
* Only the FileEndOfFileInformation InformationClass is fully implemented. FileAllocationInformation
* is a hack and not a true implementation, but it's enough to make SetEndOfFile() work.
* All other information classes are TODO.
*
*/
NTSTATUS
NtfsSetInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PIO_STACK_LOCATION Stack;
    PDEVICE_EXTENSION DeviceExt;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

    DPRINT("NtfsSetInformation(%p)\n", IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceObject = IrpContext->DeviceObject;
    DeviceExt = DeviceObject->DeviceExtension;
    FileInformationClass = Stack->Parameters.QueryFile.FileInformationClass;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = Stack->Parameters.QueryFile.Length;

    /* Serialize metadata changes with volume flush/checkpoint. */
    ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, TRUE);
    if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        ExReleaseResourceLite(&DeviceExt->DirResource);
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    switch (FileInformationClass)
    {
        PFILE_END_OF_FILE_INFORMATION EndOfFileInfo;

        /* TODO: Allocation size is not actually the same as file end for NTFS,
           however, few applications are likely to make the distinction. */
        case FileAllocationInformation:
            DPRINT1("FIXME: Using hacky method of setting FileAllocationInformation.\n");
        case FileEndOfFileInformation:
            EndOfFileInfo = (PFILE_END_OF_FILE_INFORMATION)SystemBuffer;
            Status = NtfsSetEndOfFile(Fcb,
                                      FileObject,
                                      DeviceExt,
                                      Irp->Flags,
                                      BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                      &EndOfFileInfo->EndOfFile);
            break;

        case FileDispositionInformation:
            Status = NtfsSetDispositionInformation(FileObject,
                                                   DeviceExt,
                                                   Fcb,
                                                   SystemBuffer);
            break;

        case FileRenameInformation:
            Status = NtfsSetRenameInformation(DeviceExt,
                                              Fcb,
                                              SystemBuffer,
                                              Stack->Parameters.SetFile.FileObject,
                                              BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE));
            break;

        // TODO: all other information classes

        default:
            DPRINT1("FIXME: Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_NOT_IMPLEMENTED;
    }

    ExReleaseResourceLite(&Fcb->MainResource);
    ExReleaseResourceLite(&DeviceExt->DirResource);

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information =
        Stack->Parameters.QueryFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}

/* EOF */
