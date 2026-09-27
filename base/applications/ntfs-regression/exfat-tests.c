/*
 * exFAT regression payload
 *
 * Runs when the test disk's volume is exFAT. The read tests check the
 * content a Windows-formatted template was filled with (see
 * output-VS-i386-ntfsreg/exfat-template/make-exfat-template.ps1): every byte
 * of every file, through cached, uncached and memory-mapped reads, plus
 * directory listings, name matching and volume information.
 *
 * Binary files hold byte[i] = (i * 31 + seed) mod 256.
 */

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS

#include <windef.h>
#include <winbase.h>
#include <stdio.h>
#include <wchar.h>

#include "exfat-tests.h"

WCHAR ExfatRoot[4] = L"C:\\";

VOID
ExfatPath(PWSTR Buffer, PCWSTR Relative)
{
    _snwprintf(Buffer, MAX_PATH, L"%s%s", ExfatRoot, Relative);
    Buffer[MAX_PATH - 1] = L'\0';
}

/* %S cannot print characters outside the code page, and a line that fails
   to format is dropped; names go out with non-ASCII as \uXXXX. */
static
const char *
AsciiName(PCWSTR Name, char *Buffer, size_t Size)
{
    size_t Used = 0;

    for (; *Name && Used + 7 < Size; Name++)
    {
        if (*Name >= 0x20 && *Name < 0x7F)
            Buffer[Used++] = (char)*Name;
        else
            Used += _snprintf(Buffer + Used, Size - Used, "\\u%04X", *Name);
    }
    Buffer[Used] = '\0';
    return Buffer;
}

BYTE
PatternByte(ULONGLONG Offset, BYTE Seed)
{
    return (BYTE)((Offset * 31 + Seed) & 0xFF);
}

/* Returns the offset of the first byte that differs, or -1 when all match. */
static
LONGLONG
CheckPattern(const BYTE *Buffer, DWORD Length, ULONGLONG Offset, BYTE Seed)
{
    DWORD i;

    for (i = 0; i < Length; i++)
    {
        if (Buffer[i] != PatternByte(Offset + i, Seed))
            return (LONGLONG)(Offset + i);
    }
    return -1;
}

BOOL ExfatIsTemplate = FALSE;
BOOL ExfatIsFat = FALSE;

BOOL
ExfatFindVolume(void)
{
    WCHAR FileSystem[32];
    WCHAR Label[MAX_PATH];
    WCHAR Root[4] = L"C:\\";
    WCHAR Letter;

    for (Letter = L'C'; Letter <= L'Z'; Letter++)
    {
        Root[0] = Letter;
        /* exFAT, or a FAT test disk (make-fat-image.py labels it FATREG):
           the write tests only use Win32 file calls, so they cover FAT too. */
        if (GetVolumeInformationW(Root, Label, ARRAYSIZE(Label), NULL, NULL, NULL,
                                  FileSystem, ARRAYSIZE(FileSystem)) &&
            (_wcsicmp(FileSystem, L"exFAT") == 0 ||
             ((_wcsicmp(FileSystem, L"FAT") == 0 || _wcsicmp(FileSystem, L"FAT32") == 0) &&
              _wcsicmp(Label, L"FATREG") == 0)))
        {
            ExfatRoot[0] = Letter;
            ExfatIsTemplate = _wcsicmp(Label, L"EXFATREG") == 0;
            ExfatIsFat = _wcsicmp(FileSystem, L"exFAT") != 0;
            Emit("EXFATREG INFO volume %c: fs=%S label=%S", (char)Letter, FileSystem, Label);
            return TRUE;
        }
    }
    return FALSE;
}

/* Read a whole file through the cache and compare it with Expected. */
BOOL
CheckTextFile(PCWSTR Relative, const char *Expected, const char *Name)
{
    WCHAR Path[MAX_PATH];
    char Buffer[256];
    DWORD Read = 0;
    DWORD Length = (DWORD)strlen(Expected);
    LARGE_INTEGER Size;
    HANDLE File;
    BOOL Ok;

    ExfatPath(Path, Relative);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL %s open gle=%lu", Name, GetLastError());
        return FALSE;
    }

    Ok = GetFileSizeEx(File, &Size) && Size.QuadPart == Length &&
         ReadFile(File, Buffer, sizeof(Buffer), &Read, NULL) &&
         Read == Length && memcmp(Buffer, Expected, Length) == 0;
    if (!Ok)
    {
        Emit("EXFATREG FAIL %s size=%I64d read=%lu gle=%lu", Name, Size.QuadPart, Read, GetLastError());
    }
    CloseHandle(File);
    if (Ok)
        Emit("EXFATREG PASS %s", Name);
    return Ok;
}

/* Read a pattern file in chunks, cached or uncached. */
static
BOOL
CheckPatternFile(PCWSTR Relative, ULONGLONG ExpectedSize, BYTE Seed, BOOL NoBuffering, const char *Name)
{
    WCHAR Path[MAX_PATH];
    LARGE_INTEGER Size;
    ULONGLONG Offset = 0;
    LONGLONG Bad;
    DWORD Read;
    PBYTE Buffer;
    HANDLE File;
    BOOL Ok = FALSE;

    /* Uncached reads need a sector-aligned buffer; VirtualAlloc gives a page. */
    Buffer = VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!Buffer)
    {
        Emit("EXFATREG FAIL %s alloc", Name);
        return FALSE;
    }

    ExfatPath(Path, Relative);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       NoBuffering ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL %s open gle=%lu", Name, GetLastError());
        goto Exit;
    }

    if (!GetFileSizeEx(File, &Size) || (ULONGLONG)Size.QuadPart != ExpectedSize)
    {
        Emit("EXFATREG FAIL %s size=%I64d", Name, Size.QuadPart);
        goto Close;
    }

    while (Offset < ExpectedSize)
    {
        if (!ReadFile(File, Buffer, CHUNK_SIZE, &Read, NULL) || Read == 0)
        {
            Emit("EXFATREG FAIL %s read at %I64u gle=%lu", Name, Offset, GetLastError());
            goto Close;
        }
        Bad = CheckPattern(Buffer, Read, Offset, Seed);
        if (Bad >= 0)
        {
            Emit("EXFATREG FAIL %s mismatch at %I64d", Name, Bad);
            goto Close;
        }
        Offset += Read;
    }

    /* Nothing past the end */
    if (!ReadFile(File, Buffer, CHUNK_SIZE, &Read, NULL) || Read != 0)
    {
        Emit("EXFATREG FAIL %s read past end returned %lu", Name, Read);
        goto Close;
    }

    Ok = TRUE;
    Emit("EXFATREG PASS %s", Name);

Close:
    CloseHandle(File);
Exit:
    VirtualFree(Buffer, 0, MEM_RELEASE);
    return Ok;
}

/* Map a pattern file and compare it: paging reads through the section. */
static
BOOL
CheckMappedFile(PCWSTR Relative, ULONGLONG ExpectedSize, BYTE Seed, const char *Name)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    HANDLE Mapping = NULL;
    PBYTE View = NULL;
    LONGLONG Bad;
    BOOL Ok = FALSE;

    ExfatPath(Path, Relative);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL %s open gle=%lu", Name, GetLastError());
        return FALSE;
    }

    Mapping = CreateFileMappingW(File, NULL, PAGE_READONLY, 0, 0, NULL);
    if (Mapping)
        View = MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, 0);
    if (!View)
    {
        Emit("EXFATREG FAIL %s map gle=%lu", Name, GetLastError());
        goto Exit;
    }

    Bad = CheckPattern(View, (DWORD)ExpectedSize, 0, Seed);
    if (Bad >= 0)
    {
        Emit("EXFATREG FAIL %s mismatch at %I64d", Name, Bad);
        goto Exit;
    }

    Ok = TRUE;
    Emit("EXFATREG PASS %s", Name);

Exit:
    if (View)
        UnmapViewOfFile(View);
    if (Mapping)
        CloseHandle(Mapping);
    CloseHandle(File);
    return Ok;
}

/* Count the names matching Pattern; '.' and '..' are counted separately. */
BOOL
CountMatches(PCWSTR Relative, PDWORD Files, PDWORD Dots)
{
    WIN32_FIND_DATAW Data;
    WCHAR Path[MAX_PATH];
    HANDLE Find;

    *Files = 0;
    *Dots = 0;
    ExfatPath(Path, Relative);
    Find = FindFirstFileW(Path, &Data);
    if (Find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;

    do
    {
        if (wcscmp(Data.cFileName, L".") == 0 || wcscmp(Data.cFileName, L"..") == 0)
            (*Dots)++;
        else
            (*Files)++;
    } while (FindNextFileW(Find, &Data));

    FindClose(Find);
    return GetLastError() == ERROR_NO_MORE_FILES;
}

static
BOOL
TestRootListing(void)
{
    static const PCWSTR Expected[] =
    {
        L"System Volume Information", L"hello.txt", L"pattern-1m.bin", L"Folder",
        /* A hex escape takes every following hex digit, so each one ends
           its literal: \x00EF followed by "c" must not become \x00EFc. */
        L"Long file name with \x00FC" L"n\x00EF" L"c\x00F6" L"d\x00E9" L" characters \x2713" L".txt",
        L"many", L"frag-a.bin", L"frag-b.bin"
    };
    WIN32_FIND_DATAW Data;
    WCHAR Path[MAX_PATH];
    BOOL Seen[ARRAYSIZE(Expected)] = { 0 };
    DWORD Count = 0;
    DWORD i;
    HANDLE Find;
    BOOL Ok = TRUE;

    ExfatPath(Path, L"*");
    Find = FindFirstFileW(Path, &Data);
    if (Find == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL root-listing gle=%lu", GetLastError());
        return FALSE;
    }
    do
    {
        Count++;
        for (i = 0; i < ARRAYSIZE(Expected); i++)
        {
            if (wcscmp(Data.cFileName, Expected[i]) == 0)
                break;
        }
        if (i == ARRAYSIZE(Expected))
        {
            char Ascii[512];
            Emit("EXFATREG FAIL root-listing unexpected name %s",
                 AsciiName(Data.cFileName, Ascii, sizeof(Ascii)));
            Ok = FALSE;
        }
        else
        {
            Seen[i] = TRUE;
        }
    } while (FindNextFileW(Find, &Data));
    FindClose(Find);

    for (i = 0; i < ARRAYSIZE(Expected); i++)
    {
        if (!Seen[i])
        {
            char Ascii[512];
            Emit("EXFATREG FAIL root-listing missing %s", AsciiName(Expected[i], Ascii, sizeof(Ascii)));
            Ok = FALSE;
        }
    }
    if (Ok && Count != ARRAYSIZE(Expected))
    {
        Emit("EXFATREG FAIL root-listing count=%lu", Count);
        Ok = FALSE;
    }
    if (Ok)
        Emit("EXFATREG PASS root-listing");
    return Ok;
}

static
BOOL
TestManyDirectory(void)
{
    WCHAR Relative[64];
    char Expected[32];
    DWORD Files;
    DWORD Dots;
    DWORD Index;
    BOOL Ok = TRUE;

    /* 200 files in a directory spread over five fragments */
    if (!CountMatches(L"many\\*", &Files, &Dots) || Files != 200 || Dots != 2)
    {
        Emit("EXFATREG FAIL many-listing files=%lu dots=%lu gle=%lu", Files, Dots, GetLastError());
        Ok = FALSE;
    }
    else
    {
        Emit("EXFATREG PASS many-listing");
    }

    /* Wildcards: file-100 .. file-199 */
    if (!CountMatches(L"many\\file-1*.txt", &Files, &Dots) || Files != 100 || Dots != 0)
    {
        Emit("EXFATREG FAIL many-wildcard files=%lu dots=%lu", Files, Dots);
        Ok = FALSE;
    }
    else
    {
        Emit("EXFATREG PASS many-wildcard");
    }

    /* An exact name, in another case */
    if (!CountMatches(L"many\\FILE-042.TXT", &Files, &Dots) || Files != 1)
    {
        Emit("EXFATREG FAIL many-exact files=%lu", Files);
        Ok = FALSE;
    }
    else
    {
        Emit("EXFATREG PASS many-exact");
    }

    /* Contents from the first, middle and last fragments */
    for (Index = 0; Index < 200; Index += 99)
    {
        _snwprintf(Relative, ARRAYSIZE(Relative), L"many\\file-%03lu.txt", Index);
        _snprintf(Expected, sizeof(Expected), "file %03lu\r\n", Index);
        Ok = CheckTextFile(Relative, Expected, "many-content") && Ok;
    }
    return Ok;
}

static
BOOL
TestAttributesAndTimes(void)
{
    WCHAR Path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA Data;
    SYSTEMTIME Time;
    BOOL Ok = TRUE;

    ExfatPath(Path, L"Folder");
    if (!GetFileAttributesExW(Path, GetFileExInfoStandard, &Data) ||
        !(Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        Emit("EXFATREG FAIL attributes-dir attr=%08lx gle=%lu", Data.dwFileAttributes, GetLastError());
        Ok = FALSE;
    }

    ExfatPath(Path, L"hello.txt");
    if (!GetFileAttributesExW(Path, GetFileExInfoStandard, &Data) ||
        (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        Data.nFileSizeLow != 26 || Data.nFileSizeHigh != 0)
    {
        Emit("EXFATREG FAIL attributes-file attr=%08lx size=%lu", Data.dwFileAttributes, Data.nFileSizeLow);
        Ok = FALSE;
    }
    else if (!FileTimeToSystemTime(&Data.ftLastWriteTime, &Time) ||
             Time.wYear < 2020 || Time.wYear > 2100)
    {
        Emit("EXFATREG FAIL times year=%u", Time.wYear);
        Ok = FALSE;
    }

    if (Ok)
        Emit("EXFATREG PASS attributes-times");
    return Ok;
}

static
BOOL
TestVolumeInformation(void)
{
    WCHAR FileSystem[32];
    WCHAR Label[MAX_PATH];
    ULARGE_INTEGER Free;
    ULARGE_INTEGER Total;
    BOOL Ok;

    Ok = GetVolumeInformationW(ExfatRoot, Label, ARRAYSIZE(Label), NULL, NULL, NULL,
                               FileSystem, ARRAYSIZE(FileSystem)) &&
         wcscmp(FileSystem, L"exFAT") == 0 && wcscmp(Label, L"EXFATREG") == 0 &&
         GetDiskFreeSpaceExW(ExfatRoot, &Free, &Total, NULL) &&
         Total.QuadPart == 32208ULL * 4096 && Free.QuadPart < Total.QuadPart;
    if (Ok)
        Emit("EXFATREG PASS volume-information");
    else
        Emit("EXFATREG FAIL volume-information fs=%S label=%S total=%I64u free=%I64u",
             FileSystem, Label, Total.QuadPart, Free.QuadPart);
    return Ok;
}

static
BOOL
TestMissingNames(void)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD FileError;
    DWORD PathError;

    ExfatPath(Path, L"no-such-file.txt");
    File = CreateFileW(Path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    FileError = GetLastError();
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    ExfatPath(Path, L"no-such-dir\\file.txt");
    File = CreateFileW(Path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    PathError = GetLastError();
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    if (FileError == ERROR_FILE_NOT_FOUND && PathError == ERROR_PATH_NOT_FOUND)
    {
        Emit("EXFATREG PASS missing-names");
        return TRUE;
    }
    Emit("EXFATREG FAIL missing-names file=%lu path=%lu", FileError, PathError);
    return FALSE;
}

BOOL
ExfatRunReadTests(void)
{
    BOOL Result = TRUE;

    Result = TestVolumeInformation() && Result;
    Result = TestRootListing() && Result;
    Result = CheckTextFile(L"hello.txt", "Hello from Windows exFAT\r\n", "hello") && Result;
    Result = CheckTextFile(L"HELLO.TXT", "Hello from Windows exFAT\r\n", "case-insensitive") && Result;
    Result = CheckTextFile(L"Folder\\Sub\\deep.txt", "deep\r\n", "nested") && Result;
    Result = CheckTextFile(L"Long file name with \x00FC" L"n\x00EF" L"c\x00F6" L"d\x00E9" L" characters \x2713" L".txt",
                           "unicode\r\n", "long-unicode-name") && Result;
    Result = CheckPatternFile(L"pattern-1m.bin", 1048576, 7, FALSE, "contiguous-cached") && Result;
    Result = CheckPatternFile(L"pattern-1m.bin", 1048576, 7, TRUE, "contiguous-uncached") && Result;
    Result = CheckPatternFile(L"frag-a.bin", 1048576, 11, FALSE, "fragmented-cached") && Result;
    Result = CheckPatternFile(L"frag-b.bin", 1048576, 13, TRUE, "fragmented-uncached") && Result;
    Result = CheckMappedFile(L"frag-b.bin", 1048576, 13, "fragmented-mapped") && Result;
    Result = TestManyDirectory() && Result;
    Result = TestAttributesAndTimes() && Result;
    Result = TestMissingNames() && Result;
    return Result;
}
