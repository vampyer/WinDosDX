
#include "k32_vista.h"

#include <ndk/rtlfuncs.h>
#include <ndk/iofuncs.h>

#define NDEBUG
#include <debug.h>

#undef FIXME
#define FIXME DPRINT1

/* Taken from Wine kernel32/file.c */

/***********************************************************************
 *	GetFileInformationByHandleEx   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetFileInformationByHandleEx(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS class,
    LPVOID info, DWORD size)
{
    NTSTATUS status;
    IO_STATUS_BLOCK io;

    switch (class)
    {
    case FileRemoteProtocolInfo:
    case FileStorageInfo:
    case FileDispositionInfoEx:
    case FileRenameInfoEx:
    case FileCaseSensitiveInfo:
    case FileNormalizedNameInfo:
        FIXME("%p, %u, %p, %lu\n", handle, class, info, size);
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;

    case FileStreamInfo:
        status = NtQueryInformationFile(handle, &io, info, size, FileStreamInformation);
        break;

    case FileCompressionInfo:
        status = NtQueryInformationFile(handle, &io, info, size, FileCompressionInformation);
        break;

    case FileAlignmentInfo:
        status = NtQueryInformationFile(handle, &io, info, size, FileAlignmentInformation);
        break;

    case FileAttributeTagInfo:
        status = NtQueryInformationFile(handle, &io, info, size, FileAttributeTagInformation);
#ifdef __REACTOS__
        /* Not every file system driver implements this class (e.g. CDFS).
         * Without reparse point support the answer is the attributes and a
         * zero tag, so derive it from FileBasicInformation; the Visual C++
         * std::filesystem runtime depends on this query succeeding. */
        if ((status == STATUS_INVALID_PARAMETER || status == STATUS_INVALID_INFO_CLASS ||
             status == STATUS_NOT_IMPLEMENTED) && size >= sizeof(FILE_ATTRIBUTE_TAG_INFO))
        {
            FILE_BASIC_INFORMATION basic;
            status = NtQueryInformationFile(handle, &io, &basic, sizeof(basic), FileBasicInformation);
            if (NT_SUCCESS(status))
            {
                ((FILE_ATTRIBUTE_TAG_INFO *)info)->FileAttributes = basic.FileAttributes;
                ((FILE_ATTRIBUTE_TAG_INFO *)info)->ReparseTag = 0;
            }
        }
#endif
        break;

    case FileBasicInfo:
        status = NtQueryInformationFile(handle, &io, info, size, FileBasicInformation);
        break;

    case FileStandardInfo:
        status = NtQueryInformationFile(handle, &io, info, size, FileStandardInformation);
        break;

    case FileNameInfo:
        status = NtQueryInformationFile(handle, &io, info, size, FileNameInformation);
        break;

    case FileIdInfo:
        status = NtQueryInformationFile(handle, &io, info, size, FileIdInformation);
        break;

    case FileIdBothDirectoryRestartInfo:
    case FileIdBothDirectoryInfo:
        status = NtQueryDirectoryFile(handle, NULL, NULL, NULL, &io, info, size,
            FileIdBothDirectoryInformation, FALSE, NULL,
            (class == FileIdBothDirectoryRestartInfo));
        break;

    case FileFullDirectoryInfo:
    case FileFullDirectoryRestartInfo:
        status = NtQueryDirectoryFile(handle, NULL, NULL, NULL, &io, info, size,
            FileFullDirectoryInformation, FALSE, NULL,
            (class == FileFullDirectoryRestartInfo));
        break;

    case FileIdExtdDirectoryInfo:
    case FileIdExtdDirectoryRestartInfo:
        status = NtQueryDirectoryFile(handle, NULL, NULL, NULL, &io, info, size,
            FileIdExtdDirectoryInformation, FALSE, NULL,
            (class == FileIdExtdDirectoryRestartInfo));
        break;

    case FileRenameInfo:
    case FileDispositionInfo:
    case FileAllocationInfo:
    case FileIoPriorityHintInfo:
    case FileEndOfFileInfo:
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

#ifdef __REACTOS__
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return FALSE;
    }

    return TRUE;
#else
    return set_ntstatus(status);
#endif
}
