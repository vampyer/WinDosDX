/*
 * PROJECT:     WinDosDX Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     SetFileInformationByHandle (Vista+)
 * COPYRIGHT:   Based on Wine kernelbase/file.c (LGPL-2.1-or-later)
 *              Copyright 2026 WinDosDX Team & Contributors
 */

#include "k32_vista.h"

#include <ndk/rtlfuncs.h>
#include <ndk/iofuncs.h>

#define NDEBUG
#include <debug.h>

/*
 * The Visual C++ runtime's std::filesystem calls this to delete files
 * (FileDispositionInfoEx, then FileDispositionInfo) and to rename them
 * (FileRenameInfo). It used to be a spec stub, which raised an
 * "unimplemented function" exception inside every such program.
 */
BOOL
WINAPI
SetFileInformationByHandle(HANDLE hFile,
                           FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
                           LPVOID lpFileInformation,
                           DWORD dwBufferSize)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;

    switch (FileInformationClass)
    {
        case FileBasicInfo:
            Status = NtSetInformationFile(hFile, &IoStatus, lpFileInformation, dwBufferSize,
                                          FileBasicInformation);
            break;

        case FileEndOfFileInfo:
            Status = NtSetInformationFile(hFile, &IoStatus, lpFileInformation, dwBufferSize,
                                          FileEndOfFileInformation);
            break;

        case FileAllocationInfo:
            Status = NtSetInformationFile(hFile, &IoStatus, lpFileInformation, dwBufferSize,
                                          FileAllocationInformation);
            break;

        case FileDispositionInfo:
            Status = NtSetInformationFile(hFile, &IoStatus, lpFileInformation, dwBufferSize,
                                          FileDispositionInformation);
            break;

        case FileDispositionInfoEx:
            /* Callers fall back to FileDispositionInfo when a file system
             * rejects this class (ERROR_INVALID_PARAMETER). */
            Status = NtSetInformationFile(hFile, &IoStatus, lpFileInformation, dwBufferSize,
                                          FileDispositionInformationEx);
            break;

        case FileIoPriorityHintInfo:
            Status = NtSetInformationFile(hFile, &IoStatus, lpFileInformation, dwBufferSize,
                                          FileIoPriorityHintInformation);
            break;

        case FileRenameInfo:
        {
            /* The Win32 structure carries a DOS path; the native call wants an NT path. */
            PFILE_RENAME_INFO Win32Info = lpFileInformation;
            PFILE_RENAME_INFORMATION NtInfo;
            UNICODE_STRING NtName;
            ULONG NtSize;

            if (dwBufferSize < sizeof(FILE_RENAME_INFO) ||
                !RtlDosPathNameToNtPathName_U(Win32Info->FileName, &NtName, NULL, NULL))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }

            NtSize = FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + NtName.Length + sizeof(WCHAR);
            NtInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, NtSize);
            if (!NtInfo)
            {
                RtlFreeUnicodeString(&NtName);
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return FALSE;
            }

            NtInfo->ReplaceIfExists = Win32Info->ReplaceIfExists;
            NtInfo->RootDirectory = Win32Info->RootDirectory;
            NtInfo->FileNameLength = NtName.Length;
            RtlCopyMemory(NtInfo->FileName, NtName.Buffer, NtName.Length);
            NtInfo->FileName[NtName.Length / sizeof(WCHAR)] = UNICODE_NULL;

            Status = NtSetInformationFile(hFile, &IoStatus, NtInfo, NtSize, FileRenameInformation);

            RtlFreeHeap(RtlGetProcessHeap(), 0, NtInfo);
            RtlFreeUnicodeString(&NtName);
            break;
        }

        case FileNameInfo:
        case FileStreamInfo:
        case FileIdBothDirectoryInfo:
        case FileIdBothDirectoryRestartInfo:
        case FileFullDirectoryInfo:
        case FileFullDirectoryRestartInfo:
        case FileStorageInfo:
        case FileAlignmentInfo:
        case FileIdInfo:
        case FileIdExtdDirectoryInfo:
        case FileIdExtdDirectoryRestartInfo:
            DPRINT1("SetFileInformationByHandle(%p, %u): class not implemented\n", hFile, FileInformationClass);
            SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
            return FALSE;

        default:
            /* Read-only classes (standard, compression, attribute tag, ...) */
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }

    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }
    return TRUE;
}
