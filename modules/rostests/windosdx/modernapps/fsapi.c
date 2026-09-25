/*
 * PROJECT:     WinDosDX modern application tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The Win32 file calls the Visual C++ std::filesystem runtime
 *              makes (msvcp140 __std_fs_*), one at a time, with error codes.
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>

static int failures;

static void report(const char *what, BOOL ok)
{
    DWORD err = ok ? 0 : GetLastError();
    printf("  %-44s %s", what, ok ? "ok" : "FAIL");
    if (!ok)
    {
        printf(" (error %lu)", err);
        failures++;
    }
    printf("\n");
}

int main(void)
{
    WCHAR dir[MAX_PATH], file[MAX_PATH], renamed[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
    FILE_ATTRIBUTE_TAG_INFO tag;
    FILE_DISPOSITION_INFO disp = { TRUE };
    FILE_END_OF_FILE_INFO eof;
    HANDLE h;
    DWORD written;
    BYTE renameBuf[sizeof(FILE_RENAME_INFO) + MAX_PATH * sizeof(WCHAR)];
    FILE_RENAME_INFO *ren = (FILE_RENAME_INFO *)renameBuf;

    printf("fsapi\n");
    GetTempPathW(MAX_PATH, dir);
    lstrcatW(dir, L"wdx_fsapi");
    lstrcpyW(file, dir);
    lstrcatW(file, L"\\a.txt");
    lstrcpyW(renamed, dir);
    lstrcatW(renamed, L"\\b.txt");
    printf("  dir: %ls\n", dir);

    /* Directory: stat of the parent, create, open with backup semantics */
    report("GetFileAttributesExW(temp dir parent)", GetFileAttributesExW(L"C:\\", GetFileExInfoStandard, &fad));
    RemoveDirectoryW(dir);
    report("CreateDirectoryW", CreateDirectoryW(dir, NULL));
    report("GetFileAttributesExW(new dir)", GetFileAttributesExW(dir, GetFileExInfoStandard, &fad));
    h = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    report("CreateFileW(dir, BACKUP_SEMANTICS|REPARSE)", h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE)
    {
        report("GetFileInformationByHandleEx(dir, Basic)",
               GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic)));
        report("GetFileInformationByHandleEx(dir, AttributeTag)",
               GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)));
        CloseHandle(h);
    }

    /* File: create, write, query, resize, rename, delete by disposition */
    h = CreateFileW(file, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    report("CreateFileW(file, CREATE_ALWAYS)", h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE)
    {
        report("WriteFile", WriteFile(h, "hello", 5, &written, NULL) && written == 5);
        report("GetFileInformationByHandleEx(file, Standard)",
               GetFileInformationByHandleEx(h, FileStandardInfo, &standard, sizeof(standard)) &&
               standard.EndOfFile.QuadPart == 5);
        eof.EndOfFile.QuadPart = 3;
        report("SetFileInformationByHandle(EndOfFile)",
               SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof(eof)));
        ZeroMemory(renameBuf, sizeof(renameBuf));
        ren->ReplaceIfExists = TRUE;
        lstrcpyW(ren->FileName, renamed);
        ren->FileNameLength = lstrlenW(renamed) * sizeof(WCHAR);
        report("SetFileInformationByHandle(Rename)",
               SetFileInformationByHandle(h, FileRenameInfo, renameBuf, sizeof(renameBuf)));
        report("SetFileInformationByHandle(Disposition)",
               SetFileInformationByHandle(h, FileDispositionInfo, &disp, sizeof(disp)));
        CloseHandle(h);
    }
    report("file gone after close",
           GetFileAttributesW(renamed) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES);
    report("RemoveDirectoryW", RemoveDirectoryW(dir));

    printf("%s fsapi\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
