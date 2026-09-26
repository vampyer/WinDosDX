/*
 * exFAT write tests
 *
 * They run in a scratch directory after the read tests, on a volume the
 * test image enables writing for. Every check reads back through a fresh
 * open, so it sees what is on the volume, not what a buffer still holds.
 * The files are left in place for the offline checker and Windows chkdsk,
 * except the ones a test deletes on purpose.
 */

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS

#include <windef.h>
#include <winbase.h>
#include <stdio.h>
#include <wchar.h>

#include "exfat-tests.h"

#define SEG_ZERO    0
#define SEG_PATTERN 1

/* Expected content: zeros, or the pattern at absolute file offsets. */
typedef struct _SEGMENT
{
    ULONGLONG Start;
    ULONGLONG End;
    int Kind;
    BYTE Seed;
} SEGMENT;

static
BYTE
ExpectedByte(const SEGMENT *Segments, int Count, ULONGLONG Offset)
{
    int i;

    for (i = 0; i < Count; i++)
    {
        if (Offset >= Segments[i].Start && Offset < Segments[i].End)
            return Segments[i].Kind == SEG_ZERO ? 0 : PatternByte(Offset, Segments[i].Seed);
    }
    return 0xEE;
}

/* Read a whole file back and check its size and every byte. */
static
BOOL
VerifyFile(PCWSTR Relative, ULONGLONG Size, const SEGMENT *Segments, int Count, const char *Name)
{
    WCHAR Path[MAX_PATH];
    LARGE_INTEGER FileSize;
    ULONGLONG Offset = 0;
    DWORD Read;
    DWORD i;
    PBYTE Buffer;
    HANDLE File;
    BOOL Ok = FALSE;

    Buffer = VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!Buffer)
        return FALSE;

    ExfatPath(Path, Relative);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL %s verify-open gle=%lu", Name, GetLastError());
        goto Exit;
    }
    if (!GetFileSizeEx(File, &FileSize) || (ULONGLONG)FileSize.QuadPart != Size)
    {
        Emit("EXFATREG FAIL %s size=%I64d expected=%I64u", Name, FileSize.QuadPart, Size);
        goto Close;
    }
    while (Offset < Size)
    {
        if (!ReadFile(File, Buffer, CHUNK_SIZE, &Read, NULL) || Read == 0)
        {
            Emit("EXFATREG FAIL %s read at %I64u gle=%lu", Name, Offset, GetLastError());
            goto Close;
        }
        for (i = 0; i < Read; i++)
        {
            if (Buffer[i] != ExpectedByte(Segments, Count, Offset + i))
            {
                Emit("EXFATREG FAIL %s byte %I64u is %02x, expected %02x", Name, Offset + i,
                     Buffer[i], ExpectedByte(Segments, Count, Offset + i));
                goto Close;
            }
        }
        Offset += Read;
    }
    Ok = TRUE;

Close:
    CloseHandle(File);
Exit:
    VirtualFree(Buffer, 0, MEM_RELEASE);
    return Ok;
}

/* Write the pattern for [Offset, Offset + Length) in Chunk-sized pieces. */
static
BOOL
WritePattern(HANDLE File, ULONGLONG Offset, ULONGLONG Length, BYTE Seed, DWORD Chunk)
{
    static BYTE Buffer[CHUNK_SIZE];
    LARGE_INTEGER Position;
    ULONGLONG Done = 0;
    DWORD Piece;
    DWORD Written;
    DWORD i;

    Position.QuadPart = (LONGLONG)Offset;
    if (!SetFilePointerEx(File, Position, NULL, FILE_BEGIN))
        return FALSE;
    while (Done < Length)
    {
        Piece = (DWORD)min((ULONGLONG)min(Chunk, CHUNK_SIZE), Length - Done);
        for (i = 0; i < Piece; i++)
            Buffer[i] = PatternByte(Offset + Done + i, Seed);
        if (!WriteFile(File, Buffer, Piece, &Written, NULL) || Written != Piece)
            return FALSE;
        Done += Piece;
    }
    return TRUE;
}

static
HANDLE
OpenForWrite(PCWSTR Relative, DWORD Disposition, DWORD Flags)
{
    WCHAR Path[MAX_PATH];

    ExfatPath(Path, Relative);
    return CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, Disposition, Flags, NULL);
}

static
BOOL
SetSize(HANDLE File, ULONGLONG Size)
{
    LARGE_INTEGER Position;

    Position.QuadPart = (LONGLONG)Size;
    return SetFilePointerEx(File, Position, NULL, FILE_BEGIN) && SetEndOfFile(File);
}

static
BOOL
Report(BOOL Ok, const char *Name)
{
    if (Ok)
        Emit("EXFATREG PASS %s", Name);
    return Ok;
}

static
BOOL
FreeBytes(PULONGLONG Free)
{
    ULARGE_INTEGER Available;

    if (!GetDiskFreeSpaceExW(ExfatRoot, &Available, NULL, NULL))
        return FALSE;
    *Free = Available.QuadPart;
    return TRUE;
}

#define CACHED_SIZE 3145851ULL

static
BOOL
TestCachedWrite(void)
{
    SEGMENT Seg[] = { { 0, CACHED_SIZE, SEG_PATTERN, 21 } };
    HANDLE File = OpenForWrite(L"exfat-reg\\cached.bin", CREATE_NEW, 0);
    BOOL Ok;

    if (File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL cached-write create gle=%lu", GetLastError());
        return FALSE;
    }
    /* An odd chunk size makes writes straddle clusters and pages. */
    Ok = WritePattern(File, 0, CACHED_SIZE, 21, 12345);
    if (!Ok)
        Emit("EXFATREG FAIL cached-write write gle=%lu", GetLastError());
    CloseHandle(File);
    return Report(Ok && VerifyFile(L"exfat-reg\\cached.bin", CACHED_SIZE, Seg, 1, "cached-write"),
                  "cached-write");
}

static
BOOL
TestUncachedWrite(void)
{
    SEGMENT Seg[] = { { 0, 262144, SEG_PATTERN, 23 } };
    PBYTE Buffer;
    DWORD Written;
    DWORD i;
    ULONG Offset;
    HANDLE File;
    BOOL Ok = TRUE;

    Buffer = VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    File = OpenForWrite(L"exfat-reg\\uncached.bin", CREATE_NEW,
                        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
    if (!Buffer || File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL uncached-write create gle=%lu", GetLastError());
        if (Buffer)
            VirtualFree(Buffer, 0, MEM_RELEASE);
        if (File != INVALID_HANDLE_VALUE)
            CloseHandle(File);
        return FALSE;
    }
    for (Offset = 0; Offset < 262144 && Ok; Offset += CHUNK_SIZE)
    {
        for (i = 0; i < CHUNK_SIZE; i++)
            Buffer[i] = PatternByte(Offset + i, 23);
        Ok = WriteFile(File, Buffer, CHUNK_SIZE, &Written, NULL) && Written == CHUNK_SIZE;
    }
    if (!Ok)
        Emit("EXFATREG FAIL uncached-write write gle=%lu", GetLastError());
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
    return Report(Ok && VerifyFile(L"exfat-reg\\uncached.bin", 262144, Seg, 1, "uncached-write"),
                  "uncached-write");
}

static
BOOL
TestOverwriteMiddle(void)
{
    SEGMENT Seg[] =
    {
        { 0, 1000003, SEG_PATTERN, 21 },
        { 1000003, 1070003, SEG_PATTERN, 25 },
        { 1070003, CACHED_SIZE, SEG_PATTERN, 21 },
    };
    HANDLE File = OpenForWrite(L"exfat-reg\\cached.bin", OPEN_EXISTING, 0);
    BOOL Ok;

    if (File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL overwrite-middle open gle=%lu", GetLastError());
        return FALSE;
    }
    Ok = WritePattern(File, 1000003, 70000, 25, 7777);
    CloseHandle(File);
    return Report(Ok && VerifyFile(L"exfat-reg\\cached.bin", CACHED_SIZE, Seg, 3, "overwrite-middle"),
                  "overwrite-middle");
}

static
BOOL
TestGapExtend(void)
{
    SEGMENT Seg[] =
    {
        { 0, 1000, SEG_PATTERN, 27 },
        { 1000, 5000000, SEG_ZERO, 0 },
        { 5000000, 5000100, SEG_PATTERN, 27 },
    };
    HANDLE File = OpenForWrite(L"exfat-reg\\gap.bin", CREATE_NEW, 0);
    BOOL Ok;

    if (File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL gap-extend create gle=%lu", GetLastError());
        return FALSE;
    }
    /* Writing past the end must leave zeros in the gap, not old disk data. */
    Ok = WritePattern(File, 0, 1000, 27, 1000) &&
         WritePattern(File, 5000000, 100, 27, 100);
    CloseHandle(File);
    return Report(Ok && VerifyFile(L"exfat-reg\\gap.bin", 5000100, Seg, 3, "gap-extend"), "gap-extend");
}

static
BOOL
TestSetEndOfFile(void)
{
    SEGMENT Extended[] = { { 0, 10000, SEG_PATTERN, 29 }, { 10000, 3000000, SEG_ZERO, 0 } };
    SEGMENT Truncated[] = { { 0, 5000, SEG_PATTERN, 29 } };
    SEGMENT Regrown[] = { { 0, 5000, SEG_PATTERN, 29 }, { 5000, 20000, SEG_ZERO, 0 } };
    HANDLE File;
    BOOL Ok;

    File = OpenForWrite(L"exfat-reg\\eof.bin", CREATE_NEW, 0);
    Ok = File != INVALID_HANDLE_VALUE && WritePattern(File, 0, 10000, 29, 4096) && SetSize(File, 3000000);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    Ok = Ok && VerifyFile(L"exfat-reg\\eof.bin", 3000000, Extended, 2, "seteof-extend");

    File = OpenForWrite(L"exfat-reg\\eof.bin", OPEN_EXISTING, 0);
    Ok = Ok && File != INVALID_HANDLE_VALUE && SetSize(File, 5000);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    Ok = Ok && VerifyFile(L"exfat-reg\\eof.bin", 5000, Truncated, 1, "seteof-truncate");

    /* Growing again must not bring the truncated data back. */
    File = OpenForWrite(L"exfat-reg\\eof.bin", OPEN_EXISTING, 0);
    Ok = Ok && File != INVALID_HANDLE_VALUE && SetSize(File, 20000);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    Ok = Ok && VerifyFile(L"exfat-reg\\eof.bin", 20000, Regrown, 2, "seteof-regrow");

    if (!Ok)
        Emit("EXFATREG FAIL set-end-of-file gle=%lu", GetLastError());
    return Report(Ok, "set-end-of-file");
}

/* Grow files Windows wrote: a contiguous ("no FAT chain") and a chained one. */
static
BOOL
TestAppendWindowsFiles(void)
{
    SEGMENT Contiguous[] = { { 0, 2097152, SEG_PATTERN, 7 } };
    SEGMENT Chained[] = { { 0, 1572864, SEG_PATTERN, 11 } };
    HANDLE File;
    BOOL Ok;

    File = OpenForWrite(L"pattern-1m.bin", OPEN_EXISTING, 0);
    Ok = File != INVALID_HANDLE_VALUE && WritePattern(File, 1048576, 1048576, 7, CHUNK_SIZE);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    Ok = Ok && VerifyFile(L"pattern-1m.bin", 2097152, Contiguous, 1, "append-contiguous");

    File = OpenForWrite(L"frag-a.bin", OPEN_EXISTING, 0);
    Ok = Ok && File != INVALID_HANDLE_VALUE && WritePattern(File, 1048576, 524288, 11, 9999);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    Ok = Ok && VerifyFile(L"frag-a.bin", 1572864, Chained, 1, "append-chained");

    if (!Ok)
        Emit("EXFATREG FAIL append-windows-files gle=%lu", GetLastError());
    return Report(Ok, "append-windows-files");
}

#define GROW_COUNT 300

static
BOOL
TestDirectoryGrowth(void)
{
    WCHAR Path[MAX_PATH];
    WCHAR Relative[128];
    char Content[32];
    DWORD Files = 0;
    DWORD Dots = 0;
    DWORD Written;
    DWORD i;
    HANDLE File;
    BOOL Ok;

    ExfatPath(Path, L"exfat-reg\\grow");
    Ok = CreateDirectoryW(Path, NULL);
    if (!Ok)
        Emit("EXFATREG FAIL directory-growth mkdir gle=%lu", GetLastError());

    /* Long names take five entries each: the directory grows by many clusters. */
    for (i = 0; i < GROW_COUNT && Ok; i++)
    {
        _snwprintf(Relative, ARRAYSIZE(Relative),
                   L"exfat-reg\\grow\\item-%03lu with a longer name to use more entries.txt", i);
        File = OpenForWrite(Relative, CREATE_NEW, 0);
        if (File == INVALID_HANDLE_VALUE)
        {
            Emit("EXFATREG FAIL directory-growth create %lu gle=%lu", i, GetLastError());
            Ok = FALSE;
            break;
        }
        _snprintf(Content, sizeof(Content), "item %03lu\r\n", i);
        Ok = WriteFile(File, Content, (DWORD)strlen(Content), &Written, NULL);
        CloseHandle(File);
    }

    if (Ok && (!CountMatches(L"exfat-reg\\grow\\*", &Files, &Dots) || Files != GROW_COUNT || Dots != 2))
    {
        Emit("EXFATREG FAIL directory-growth listing files=%lu dots=%lu", Files, Dots);
        Ok = FALSE;
    }
    for (i = 0; i < GROW_COUNT && Ok; i += 149)
    {
        _snwprintf(Relative, ARRAYSIZE(Relative),
                   L"exfat-reg\\grow\\item-%03lu with a longer name to use more entries.txt", i);
        _snprintf(Content, sizeof(Content), "item %03lu\r\n", i);
        Ok = CheckTextFile(Relative, Content, "directory-growth-content");
    }
    return Report(Ok, "directory-growth");
}

static
BOOL
TestRenameAndMove(void)
{
    SEGMENT Seg[] = { { 0, 262144, SEG_PATTERN, 23 } };
    WCHAR From[MAX_PATH];
    WCHAR To[MAX_PATH];
    BOOL Ok;

    /* A longer name needs more entries than the old set has. */
    ExfatPath(From, L"exfat-reg\\uncached.bin");
    ExfatPath(To, L"exfat-reg\\renamed with a much longer name than before.bin");
    Ok = MoveFileW(From, To) &&
         VerifyFile(L"exfat-reg\\renamed with a much longer name than before.bin", 262144, Seg, 1, "rename");

    /* Into another directory */
    ExfatPath(From, L"exfat-reg\\renamed with a much longer name than before.bin");
    ExfatPath(To, L"exfat-reg\\grow\\moved.bin");
    Ok = Ok && MoveFileW(From, To) &&
         VerifyFile(L"exfat-reg\\grow\\moved.bin", 262144, Seg, 1, "move");

    /* A directory with contents */
    ExfatPath(From, L"exfat-reg\\grow");
    ExfatPath(To, L"exfat-reg\\grown");
    Ok = Ok && MoveFileW(From, To) &&
         VerifyFile(L"exfat-reg\\grown\\moved.bin", 262144, Seg, 1, "rename-directory") &&
         CheckTextFile(L"exfat-reg\\grown\\item-150 with a longer name to use more entries.txt",
                       "item 150\r\n", "rename-directory-content");

    if (!Ok)
        Emit("EXFATREG FAIL rename-and-move gle=%lu", GetLastError());
    return Report(Ok, "rename-and-move");
}

static
BOOL
TestDelete(void)
{
    WIN32_FIND_DATAW Data;
    WCHAR Path[MAX_PATH];
    WCHAR Pattern[MAX_PATH];
    WCHAR FilePath[MAX_PATH];
    ULONGLONG FreeBefore = 0;
    ULONGLONG FreeAfter = 0;
    HANDLE Find;
    BOOL Ok;

    ExfatPath(Path, L"exfat-reg\\grown");
    Ok = FreeBytes(&FreeBefore);

    /* A directory with files in it is not removed. */
    if (Ok && (RemoveDirectoryW(Path) || GetLastError() != ERROR_DIR_NOT_EMPTY))
    {
        Emit("EXFATREG FAIL delete non-empty directory gle=%lu", GetLastError());
        Ok = FALSE;
    }

    ExfatPath(Pattern, L"exfat-reg\\grown\\*");
    Find = FindFirstFileW(Pattern, &Data);
    if (Find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (Data.cFileName[0] == L'.')
                continue;
            _snwprintf(FilePath, ARRAYSIZE(FilePath), L"%s\\%s", Path, Data.cFileName);
            FilePath[MAX_PATH - 1] = L'\0';
            if (!DeleteFileW(FilePath))
            {
                Emit("EXFATREG FAIL delete file gle=%lu", GetLastError());
                Ok = FALSE;
            }
        } while (FindNextFileW(Find, &Data));
        FindClose(Find);
    }

    Ok = Ok && RemoveDirectoryW(Path) && GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES &&
         FreeBytes(&FreeAfter) && FreeAfter > FreeBefore;
    if (!Ok)
        Emit("EXFATREG FAIL delete gle=%lu before=%I64u after=%I64u", GetLastError(), FreeBefore, FreeAfter);
    return Report(Ok, "delete");
}

static
BOOL
TestAttributesAndTimesWrite(void)
{
    SYSTEMTIME Creation = { 2020, 1, 0, 2, 3, 4, 5, 60 };
    SYSTEMTIME Modified = { 2021, 6, 0, 15, 12, 34, 56, 780 };
    FILETIME SetCreation, SetModified;
    FILETIME GotCreation = { 0 }, GotAccess = { 0 }, GotModified = { 0 };
    WCHAR Path[MAX_PATH];
    DWORD Attributes = 0;
    HANDLE File;
    BOOL Ok;

    SystemTimeToFileTime(&Creation, &SetCreation);
    SystemTimeToFileTime(&Modified, &SetModified);

    File = OpenForWrite(L"exfat-reg\\times.txt", CREATE_NEW, 0);
    Ok = File != INVALID_HANDLE_VALUE && SetFileTime(File, &SetCreation, NULL, &SetModified);
    if (!Ok)
        Emit("EXFATREG FAIL attributes-times-write set-times gle=%lu", GetLastError());
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    ExfatPath(Path, L"exfat-reg\\times.txt");
    if (Ok && !SetFileAttributesW(Path, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN))
    {
        Emit("EXFATREG FAIL attributes-times-write set-attributes gle=%lu", GetLastError());
        Ok = FALSE;
    }

    /* Read them back from a fresh open: exFAT keeps 10 ms for these two. */
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (Ok && (File == INVALID_HANDLE_VALUE ||
               !GetFileTime(File, &GotCreation, &GotAccess, &GotModified) ||
               CompareFileTime(&GotCreation, &SetCreation) != 0 ||
               CompareFileTime(&GotModified, &SetModified) != 0))
    {
        Emit("EXFATREG FAIL attributes-times-write times created=%08lx%08lx/%08lx%08lx "
             "modified=%08lx%08lx/%08lx%08lx gle=%lu",
             GotCreation.dwHighDateTime, GotCreation.dwLowDateTime,
             SetCreation.dwHighDateTime, SetCreation.dwLowDateTime,
             GotModified.dwHighDateTime, GotModified.dwLowDateTime,
             SetModified.dwHighDateTime, SetModified.dwLowDateTime, GetLastError());
        Ok = FALSE;
    }
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    Attributes = GetFileAttributesW(Path);
    if (Ok && (Attributes == INVALID_FILE_ATTRIBUTES ||
               (Attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)) !=
               (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)))
    {
        Emit("EXFATREG FAIL attributes-times-write attributes=%08lx", Attributes);
        Ok = FALSE;
    }

    /* A read-only file refuses to open for writing. */
    File = OpenForWrite(L"exfat-reg\\times.txt", OPEN_EXISTING, 0);
    if (File != INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL attributes-times-write read-only file opened for writing");
        CloseHandle(File);
        Ok = FALSE;
    }

    return Report(Ok, "attributes-times-write");
}

static
BOOL
TestMappedWrite(void)
{
    SEGMENT Seg[] = { { 0, 1048576, SEG_PATTERN, 31 } };
    HANDLE File;
    HANDLE Mapping = NULL;
    PBYTE View = NULL;
    DWORD i;
    BOOL Ok = FALSE;

    File = OpenForWrite(L"exfat-reg\\mapped.bin", CREATE_NEW, 0);
    if (File == INVALID_HANDLE_VALUE)
    {
        Emit("EXFATREG FAIL mapped-write create gle=%lu", GetLastError());
        return FALSE;
    }
    Mapping = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, 1048576, NULL);
    if (Mapping)
        View = MapViewOfFile(Mapping, FILE_MAP_WRITE, 0, 0, 0);
    if (View)
    {
        for (i = 0; i < 1048576; i++)
            View[i] = PatternByte(i, 31);
        Ok = FlushViewOfFile(View, 0);
        UnmapViewOfFile(View);
    }
    if (!Ok)
        Emit("EXFATREG FAIL mapped-write map gle=%lu", GetLastError());
    if (Mapping)
        CloseHandle(Mapping);
    CloseHandle(File);
    return Report(Ok && VerifyFile(L"exfat-reg\\mapped.bin", 1048576, Seg, 1, "mapped-write"), "mapped-write");
}

static
BOOL
TestUnicodeAndCollisions(void)
{
    /* Each hex escape ends its literal, or it would swallow the next letter. */
    static const WCHAR Name[] = L"exfat-reg\\\x00D1" L"ame \x4E2D\x6587 \x00E9" L"t\x00E9" L".txt";
    WCHAR Path[MAX_PATH];
    DWORD Written;
    DWORD Files = 0;
    DWORD Dots = 0;
    HANDLE File;
    BOOL Ok;

    File = OpenForWrite(Name, CREATE_NEW, 0);
    Ok = File != INVALID_HANDLE_VALUE && WriteFile(File, "unicode2\r\n", 10, &Written, NULL);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    Ok = Ok && CountMatches(Name, &Files, &Dots) && Files == 1 &&
         CheckTextFile(Name, "unicode2\r\n", "unicode-create");
    if (!Ok)
        Emit("EXFATREG FAIL unicode create files=%lu gle=%lu", Files, GetLastError());

    /* Names that differ only in case are the same name. */
    File = OpenForWrite(L"exfat-reg\\CACHED.BIN", CREATE_NEW, 0);
    if (File != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS)
    {
        Emit("EXFATREG FAIL collision file gle=%lu", GetLastError());
        if (File != INVALID_HANDLE_VALUE)
            CloseHandle(File);
        Ok = FALSE;
    }
    ExfatPath(Path, L"EXFAT-REG");
    if (CreateDirectoryW(Path, NULL) || GetLastError() != ERROR_ALREADY_EXISTS)
    {
        Emit("EXFATREG FAIL collision directory gle=%lu", GetLastError());
        Ok = FALSE;
    }
    return Report(Ok, "unicode-and-collisions");
}

static
BOOL
TestSpaceAccounting(void)
{
    ULONGLONG Before = 0;
    ULONGLONG Written = 0;
    ULONGLONG Full = 0;
    ULONGLONG After = 0;
    WCHAR Path[MAX_PATH];
    HANDLE File;
    BOOL Ok;

    /* One MB written takes exactly 256 clusters of 4 KB. */
    Ok = FreeBytes(&Before);
    File = OpenForWrite(L"exfat-reg\\space.bin", CREATE_NEW, 0);
    Ok = Ok && File != INVALID_HANDLE_VALUE && WritePattern(File, 0, 1048576, 33, CHUNK_SIZE);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    Ok = Ok && FreeBytes(&Written) && Before - Written == 1048576;
    if (!Ok)
        Emit("EXFATREG FAIL space-accounting used=%I64u gle=%lu", Before - Written, GetLastError());

    File = OpenForWrite(L"exfat-reg\\space.bin", OPEN_EXISTING, 0);
    if (Ok && File != INVALID_HANDLE_VALUE)
    {
        /* Asking for more than is free fails and takes nothing. */
        if (SetSize(File, 1048576 + Written + 4096) || GetLastError() != ERROR_DISK_FULL)
        {
            Emit("EXFATREG FAIL disk-full gle=%lu", GetLastError());
            Ok = FALSE;
        }
        /* Taking all of it works. */
        Ok = Ok && SetSize(File, 1048576 + Written) && FreeBytes(&Full) && Full == 0;
        if (!Ok)
            Emit("EXFATREG FAIL fill-disk free=%I64u gle=%lu", Full, GetLastError());
    }
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    ExfatPath(Path, L"exfat-reg\\space.bin");
    Ok = Ok && DeleteFileW(Path) && FreeBytes(&After) && After == Before;
    if (!Ok)
        Emit("EXFATREG FAIL space-release before=%I64u after=%I64u gle=%lu", Before, After, GetLastError());
    return Report(Ok, "space-accounting");
}

BOOL
ExfatRunWriteTests(void)
{
    WCHAR Path[MAX_PATH];
    BOOL Result = TRUE;

    ExfatPath(Path, L"exfat-reg");
    if (!CreateDirectoryW(Path, NULL))
    {
        Emit("EXFATREG FAIL scratch-directory gle=%lu", GetLastError());
        return FALSE;
    }

    Result = TestCachedWrite() && Result;
    Result = TestUncachedWrite() && Result;
    Result = TestOverwriteMiddle() && Result;
    Result = TestGapExtend() && Result;
    Result = TestSetEndOfFile() && Result;
    Result = TestAppendWindowsFiles() && Result;
    Result = TestDirectoryGrowth() && Result;
    Result = TestRenameAndMove() && Result;
    Result = TestDelete() && Result;
    Result = TestAttributesAndTimesWrite() && Result;
    Result = TestMappedWrite() && Result;
    Result = TestUnicodeAndCollisions() && Result;
    Result = TestSpaceAccounting() && Result;
    return Result;
}
