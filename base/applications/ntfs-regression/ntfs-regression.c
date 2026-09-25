/*
 * NTFS image regression payload
 *
 * This is intentionally a small Win32 console application. The host-side
 * harness starts it through the ReactOS Run dialog and watches COM1 for
 * machine-readable markers.
 */

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS

#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winioctl.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#include <fmifs/fmifs.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#define DATA_SIZE (64 * 1024)
#define CRASH_ITERATIONS 4096

static BYTE Data[DATA_SIZE];
static HANDLE SerialHandle = INVALID_HANDLE_VALUE;
static DWORD FormatError;
static WCHAR NtfsRoot[4] = L"C:\\";
static WCHAR NtfsTestDir[MAX_PATH];

static
BOOLEAN
NTAPI
FormatExCallback(CALLBACKCOMMAND Command, ULONG SubAction, PVOID ActionInfo)
{
    UNREFERENCED_PARAMETER(SubAction);
    UNREFERENCED_PARAMETER(ActionInfo);

    if (Command == DONE && ActionInfo != NULL)
        FormatError = *(DWORD *)ActionInfo;

    return TRUE;
}

static
VOID
Emit(const char *Format, ...)
{
    char Line[512];
    va_list Arguments;
    int Length;
    DWORD Written;

    va_start(Arguments, Format);
    Length = _vsnprintf(Line, sizeof(Line) - 2, Format, Arguments);
    va_end(Arguments);

    if (Length < 0)
        return;

    if (Length > (int)sizeof(Line) - 2)
        Length = (int)sizeof(Line) - 2;

    Line[Length++] = '\r';
    Line[Length++] = '\n';

    if (SerialHandle != INVALID_HANDLE_VALUE)
        WriteFile(SerialHandle, Line, Length, &Written, NULL);
}

static
BOOL
WriteAll(HANDLE File, const BYTE *Buffer, DWORD Length)
{
    DWORD Offset = 0;

    while (Offset < Length)
    {
        DWORD Written = 0;
        if (!WriteFile(File, Buffer + Offset, Length - Offset, &Written, NULL) ||
            Written == 0)
        {
            return FALSE;
        }
        Offset += Written;
    }

    return TRUE;
}

static
BOOL
ReadAll(HANDLE File, BYTE *Buffer, DWORD Length)
{
    DWORD Offset = 0;

    while (Offset < Length)
    {
        DWORD Read = 0;
        if (!ReadFile(File, Buffer + Offset, Length - Offset, &Read, NULL) ||
            Read == 0)
        {
            return FALSE;
        }
        Offset += Read;
    }

    return TRUE;
}

static
BOOL
WriteAt(HANDLE File, ULONGLONG Offset, const BYTE *Buffer, DWORD Length)
{
    LARGE_INTEGER Position;

    Position.QuadPart = (LONGLONG)Offset;
    if (!SetFilePointerEx(File, Position, NULL, FILE_BEGIN))
        return FALSE;

    return WriteAll(File, Buffer, Length);
}

static
BOOL
ReadAt(HANDLE File, ULONGLONG Offset, BYTE *Buffer, DWORD Length)
{
    LARGE_INTEGER Position;

    Position.QuadPart = (LONGLONG)Offset;
    if (!SetFilePointerEx(File, Position, NULL, FILE_BEGIN))
        return FALSE;

    return ReadAll(File, Buffer, Length);
}

static
VOID
FillPattern(BYTE *Buffer, DWORD Length, BYTE Seed)
{
    DWORD i;

    for (i = 0; i < Length; i++)
        Buffer[i] = (BYTE)(Seed + (i * 37));
}

static
VOID
BuildPath(PWSTR Path, PCWSTR Root, PCWSTR Name)
{
    wcscpy(Path, Root);
    if (Path[wcslen(Path) - 1] != L'\\')
        wcscat(Path, L"\\");
    wcscat(Path, Name);
}

static
HANDLE
OpenVolume(void)
{
    return CreateFileW(L"\\\\.\\C:", GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static
BOOL
QueryVolumeDirty(HANDLE Volume, BOOL *Dirty)
{
    DWORD Flags = 0;
    DWORD Returned = 0;

    if (!DeviceIoControl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0,
                         &Flags, sizeof(Flags), &Returned, NULL) ||
        Returned != sizeof(Flags))
    {
        return FALSE;
    }

    *Dirty = (Flags & VOLUME_IS_DIRTY) != 0;
    return TRUE;
}

static
VOID
ReportVolumeDirtyBeforeCreate(VOID)
{
    HANDLE Volume = OpenVolume();
    DWORD Flags = 0;
    DWORD Returned = 0;
    BOOL Queried = Volume != INVALID_HANDLE_VALUE &&
                   DeviceIoControl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0,
                                   &Flags, sizeof(Flags), &Returned, NULL) &&
                   Returned == sizeof(Flags);

    Emit("NTFSREG INFO pre-create-dirty-query=%u flags=%08lx", Queried, Flags);
    if (Volume != INVALID_HANDLE_VALUE)
        CloseHandle(Volume);
}

static
BOOL
TestFlushCheckpoint(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    HANDLE File = INVALID_HANDLE_VALUE;
    HANDLE Volume = INVALID_HANDLE_VALUE;
    BOOL Dirty;
    BOOL Result = FALSE;

    Volume = OpenVolume();
    if (Volume == INVALID_HANDLE_VALUE)
        goto Exit;

    BuildPath(Path, Root, L"ntfs-reg-flush.bin");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, 4096, 0x35);
    if (!WriteAll(File, Data, 4096) || !QueryVolumeDirty(Volume, &Dirty) || !Dirty)
        goto Exit;

    /* A file flush protects the file cache.  The volume handle is the
       explicit operation that checkpoints the metadata journal. */
    if (!FlushFileBuffers(Volume) || !QueryVolumeDirty(Volume, &Dirty) || Dirty)
        goto Exit;

    Result = TRUE;

Exit:
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    if (Volume != INVALID_HANDLE_VALUE)
        CloseHandle(Volume);
    Emit("NTFSREG %s flush-checkpoint", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
TestVolumeLock(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    HANDLE Volume = INVALID_HANDLE_VALUE;
    HANDLE Other = INVALID_HANDLE_VALUE;
    HANDLE File = INVALID_HANDLE_VALUE;
    BOOL Result = FALSE;

    Volume = OpenVolume();
    if (Volume == INVALID_HANDLE_VALUE)
        goto Exit;
    if (!DeviceIoControl(Volume, FSCTL_LOCK_VOLUME, NULL, 0,
                         NULL, 0, NULL, NULL))
        goto Exit;

    Other = OpenVolume();
    if (Other != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Other);
        goto Exit;
    }

    BuildPath(Path, Root, L"ntfs-reg-locked.bin");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(File);
        DeleteFileW(Path);
        goto Exit;
    }

    if (!DeviceIoControl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0,
                         NULL, 0, NULL, NULL))
        goto Exit;

    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;
    CloseHandle(File);
    File = INVALID_HANDLE_VALUE;
    if (!DeleteFileW(Path) || !FlushFileBuffers(Volume))
        goto Exit;
    Result = TRUE;

Exit:
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    if (Volume != INVALID_HANDLE_VALUE)
        CloseHandle(Volume);
    Emit("NTFSREG %s volume-lock", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
TestDirtyState(PCWSTR Root, BOOL VerifyOnly)
{
    WCHAR Path[MAX_PATH];
    HANDLE Volume = INVALID_HANDLE_VALUE;
    HANDLE File = INVALID_HANDLE_VALUE;
    BOOL Dirty;
    BOOL Result = FALSE;

    Volume = OpenVolume();
    if (Volume == INVALID_HANDLE_VALUE)
        goto Exit;

    if (!VerifyOnly &&
        !DeviceIoControl(Volume, FSCTL_MARK_VOLUME_DIRTY, NULL, 0,
                         NULL, 0, NULL, NULL))
        goto Exit;

    if (!QueryVolumeDirty(Volume, &Dirty) || !Dirty)
        goto Exit;

    BuildPath(Path, Root, L"ntfs-reg-dirty.bin");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(File);
        DeleteFileW(Path);
        goto Exit;
    }

    Result = TRUE;

Exit:
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    if (Volume != INVALID_HANDLE_VALUE)
        CloseHandle(Volume);
    Emit("NTFSREG %s dirty-%s", Result ? "PASS" : "FAIL",
         VerifyOnly ? L"remount" : L"mark");
    return Result;
}

static
BOOL
TestAllocationOrdering(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    WCHAR Name[64];
    HANDLE File = INVALID_HANDLE_VALUE;
    HANDLE Volume = INVALID_HANDLE_VALUE;
    DWORD i;
    BOOL Result = FALSE;

    for (i = 0; i < 48; i++)
    {
        swprintf(Name, ARRAYSIZE(Name), L"ntfs-reg-alloc-%03u.bin", i);
        BuildPath(Path, Root, Name);
        File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (File == INVALID_HANDLE_VALUE)
            goto Exit;
        FillPattern(Data, 4096, (BYTE)i);
        if (!WriteAll(File, Data, 4096) || !FlushFileBuffers(File))
        {
            CloseHandle(File);
            goto Exit;
        }
        CloseHandle(File);
    }

    for (i = 0; i < 48; i++)
    {
        swprintf(Name, ARRAYSIZE(Name), L"ntfs-reg-alloc-%03u.bin", i);
        BuildPath(Path, Root, Name);
        File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (File == INVALID_HANDLE_VALUE)
            goto Exit;
        FillPattern(Data, 4096, 0);
        if (!ReadAll(File, Data, 4096))
        {
            CloseHandle(File);
            goto Exit;
        }
        CloseHandle(File);
        if (Data[0] != (BYTE)i || Data[4095] != (BYTE)(i + 4095 * 37))
            goto Exit;
    }

    /* Leave a flushed sentinel so the next boot starts from a clean journal. */
    BuildPath(Path, Root, L"ntfs-reg-alloc-final.bin");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;
    if (!WriteAll(File, Data, 1) || !FlushFileBuffers(File))
    {
        CloseHandle(File);
        goto Exit;
    }
    CloseHandle(File);
    File = INVALID_HANDLE_VALUE;

    Volume = OpenVolume();
    if (Volume == INVALID_HANDLE_VALUE || !FlushFileBuffers(Volume))
        goto Exit;
    Result = TRUE;

Exit:
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    if (Volume != INVALID_HANDLE_VALUE)
        CloseHandle(Volume);
    Emit("NTFSREG %s allocation-ordering", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
TestOverwriteAndFlush(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD i;
    BOOL Result = FALSE;

    BuildPath(Path, Root, L"ntfs-reg-overwrite.bin");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, DATA_SIZE, 0x11);
    if (!WriteAll(File, Data, DATA_SIZE) || !FlushFileBuffers(File))
        goto Exit;

    FillPattern(Data, 8192, 0x7d);
    if (!WriteAt(File, 4096 + 17, Data, 8192) || !FlushFileBuffers(File))
        goto Exit;

    FillPattern(Data, DATA_SIZE, 0);
    if (!ReadAt(File, 0, Data, DATA_SIZE))
        goto Exit;

    for (i = 0; i < DATA_SIZE; i++)
    {
        BYTE Expected;

        if (i >= 4096 + 17 && i < 4096 + 17 + 8192)
            Expected = (BYTE)(0x7d + ((i - (4096 + 17)) * 37));
        else
            Expected = (BYTE)(0x11 + (i * 37));

        if (Data[i] != Expected)
            goto Exit;
    }

    Result = TRUE;

Exit:
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    DeleteFileW(Path);

    Emit("NTFSREG %s overwrite-flush", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
TestExtend(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    LARGE_INTEGER FileSize;
    DWORD i;
    BOOL Result = FALSE;

    BuildPath(Path, Root, L"ntfs-reg-extend.bin");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, 8192, 0x22);
    if (!WriteAll(File, Data, 8192) || !FlushFileBuffers(File))
        goto Exit;

    FillPattern(Data, 20480, 0x63);
    if (!WriteAt(File, 8192, Data, 20480) || !FlushFileBuffers(File))
        goto Exit;

    if (!GetFileSizeEx(File, &FileSize) || FileSize.QuadPart != 28672)
        goto Exit;

    FillPattern(Data, 28672, 0);
    if (!ReadAt(File, 0, Data, 28672))
        goto Exit;

    for (i = 0; i < 8192; i++)
    {
        if (Data[i] != (BYTE)(0x22 + (i * 37)))
            goto Exit;
    }
    for (i = 8192; i < 28672; i++)
    {
        if (Data[i] != (BYTE)(0x63 + ((i - 8192) * 37)))
            goto Exit;
    }

    Result = TRUE;

Exit:
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    DeleteFileW(Path);

    Emit("NTFSREG %s extend", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
TestRenameAndDelete(PCWSTR Root)
{
    WCHAR OldPath[MAX_PATH];
    WCHAR NewPath[MAX_PATH];
    HANDLE File;
    DWORD i;
    BOOL Result = FALSE;

    BuildPath(OldPath, Root, L"ntfs-reg-rename-old.tmp");
    BuildPath(NewPath, Root, L"ntfs-reg-rename-new.tmp");
    DeleteFileW(OldPath);
    DeleteFileW(NewPath);

    File = CreateFileW(OldPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, 16384, 0x44);
    if (!WriteAll(File, Data, 16384) || !FlushFileBuffers(File))
    {
        CloseHandle(File);
        goto Exit;
    }
    CloseHandle(File);

    if (!MoveFileExW(OldPath, NewPath, MOVEFILE_REPLACE_EXISTING))
        goto Exit;

    if (GetFileAttributesW(OldPath) != INVALID_FILE_ATTRIBUTES)
        goto Exit;

    File = CreateFileW(NewPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, 16384, 0);
    if (!ReadAll(File, Data, 16384))
    {
        CloseHandle(File);
        goto Exit;
    }
    CloseHandle(File);

    for (i = 0; i < 16384; i++)
    {
        if (Data[i] != (BYTE)(0x44 + (i * 37)))
            goto Exit;
    }

    if (!DeleteFileW(NewPath) || GetFileAttributesW(NewPath) != INVALID_FILE_ATTRIBUTES)
        goto Exit;

    Result = TRUE;

Exit:
    DeleteFileW(OldPath);
    DeleteFileW(NewPath);
    Emit("NTFSREG %s rename-delete", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
TestRemount(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD i;
    BOOL Result = FALSE;

    BuildPath(Path, Root, L"ntfs-reg-remount.bin");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, 32768, 0xa5);
    if (!WriteAll(File, Data, 32768) || !FlushFileBuffers(File))
    {
        CloseHandle(File);
        goto Exit;
    }
    CloseHandle(File);

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, 32768, 0);
    if (!ReadAll(File, Data, 32768))
    {
        CloseHandle(File);
        goto Exit;
    }
    CloseHandle(File);

    for (i = 0; i < 32768; i++)
    {
        if (Data[i] != (BYTE)(0xa5 + (i * 37)))
            goto Exit;
    }

    Result = TRUE;

Exit:
    Emit("NTFSREG %s remount-write", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
VerifyRemount(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD i;
    BOOL Result = FALSE;

    BuildPath(Path, Root, L"ntfs-reg-remount.bin");
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, 32768, 0);
    if (!ReadAll(File, Data, 32768))
    {
        CloseHandle(File);
        goto Exit;
    }
    CloseHandle(File);

    for (i = 0; i < 32768; i++)
    {
        if (Data[i] != (BYTE)(0xa5 + (i * 37)))
            goto Exit;
    }

    Result = TRUE;

Exit:
    DeleteFileW(Path);
    Emit("NTFSREG %s remount-verify", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
TestCrashRecovery(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD i;
    ULONGLONG Offset;
    BOOL Result = FALSE;

    BuildPath(Path, Root, L"ntfs-reg-crash.bin");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    FillPattern(Data, DATA_SIZE, 0x91);
    if (!WriteAll(File, Data, DATA_SIZE) || !FlushFileBuffers(File))
        goto Exit;

    Emit("NTFSREG CRASH-BEGIN");
    for (i = 0; i < CRASH_ITERATIONS; i++)
    {
        Offset = (ULONGLONG)i * DATA_SIZE;
        FillPattern(Data, DATA_SIZE, (BYTE)(i * 13));
        if (!WriteAt(File, Offset, Data, DATA_SIZE))
            goto Exit;

        if ((i & 7) == 0 && !FlushFileBuffers(File))
            goto Exit;

        if ((i & 31) == 0)
            Emit("NTFSREG CRASH-PROGRESS %u", i);
    }

    Emit("NTFSREG CRASH-COMPLETE");

Exit:
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    return Result;
}

static
BOOL
VerifyCrashRecovery(PCWSTR Root)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    LARGE_INTEGER FileSize;
    DWORD i;
    BOOL Result = FALSE;

    BuildPath(Path, Root, L"ntfs-reg-crash.bin");
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        goto Exit;

    if (!GetFileSizeEx(File, &FileSize) || FileSize.QuadPart < DATA_SIZE)
        goto Exit;

    FillPattern(Data, DATA_SIZE, 0);
    if (!ReadAt(File, 0, Data, DATA_SIZE))
        goto Exit;

    for (i = 0; i < DATA_SIZE; i++)
    {
        if (Data[i] != (BYTE)(0x91 + (i * 37)))
            goto Exit;
    }

    Result = TRUE;

Exit:
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    DeleteFileW(Path);
    Emit("NTFSREG %s crash-recovery", Result ? "PASS" : "FAIL");
    return Result;
}

static
BOOL
EnsureNtfsVolume(void)
{
    WCHAR FileSystem[32];
    WCHAR VolumeLabel[MAX_PATH];
    DWORD SerialNumber;
    DWORD MaximumComponentLength;
    DWORD FileSystemFlags;
    BOOL HaveInfo;
    WCHAR Letter;

    /* Disposable-image ground truth: report every candidate letter and its
       file system so the harness can verify the volume layout in one run. */
    for (Letter = L'C'; Letter <= L'Z'; Letter++)
    {
        WCHAR Root[4];

        Root[0] = Letter;
        Root[1] = L':';
        Root[2] = L'\\';
        Root[3] = L'\0';
        HaveInfo = GetVolumeInformationW(Root,
                                         VolumeLabel,
                                         ARRAYSIZE(VolumeLabel),
                                         &SerialNumber,
                                         &MaximumComponentLength,
                                         &FileSystemFlags,
                                         FileSystem,
                                         ARRAYSIZE(FileSystem));
        if (HaveInfo)
        {
            Emit("NTFSREG INFO volume %c: fs=%S label=%S",
                 (char)Letter, FileSystem, VolumeLabel);
            if (_wcsicmp(FileSystem, L"NTFS") == 0)
            {
                NtfsRoot[0] = Letter;
                BuildPath(NtfsTestDir, NtfsRoot, L"ntfs-reg");
                return TRUE;
            }
        }
    }

    Emit("NTFSREG INFO no-ntfs mask=%08lx", GetLogicalDrives());
    return FALSE;
}

static
BOOL
ReadPhase(PWSTR Phase, DWORD PhaseBytes)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD Read = 0;

    BuildPath(Path, L"C:\\", L"ntfs-reg-phase");
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        Phase[0] = L'\0';
        return TRUE;
    }

    if (!ReadFile(File, Phase, PhaseBytes - sizeof(WCHAR), &Read, NULL))
    {
        CloseHandle(File);
        return FALSE;
    }
    CloseHandle(File);
    Phase[Read / sizeof(WCHAR)] = L'\0';
    return TRUE;
}

static
BOOL
WritePhase(PCWSTR Phase)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD Written = 0;
    DWORD Length = (DWORD)wcslen(Phase) * sizeof(WCHAR);

    BuildPath(Path, L"C:\\", L"ntfs-reg-phase");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    if (!WriteFile(File, Phase, Length, &Written, NULL) || Written != Length ||
        !FlushFileBuffers(File))
    {
        CloseHandle(File);
        return FALSE;
    }
    CloseHandle(File);
    File = CreateFileW(L"\\\\.\\C:", GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE || !FlushFileBuffers(File))
    {
        if (File != INVALID_HANDLE_VALUE)
            CloseHandle(File);
        return FALSE;
    }
    CloseHandle(File);
    return TRUE;
}

static
BOOL
DeletePhase(void)
{
    WCHAR Path[MAX_PATH];

    BuildPath(Path, L"C:\\", L"ntfs-reg-phase");
    return DeleteFileW(Path) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

int
__cdecl
wmain(int argc, WCHAR **argv)
{
    PCWSTR Root;
    PCWSTR Mode;
    WCHAR Phase[32];
    BOOL Result = TRUE;

    /* QEMU's first serial is reserved for kernel/debug output. The regression
     * payload uses COM2 so its protocol markers are not interleaved with it. */
    SerialHandle = CreateFileA("COM2", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (SerialHandle == INVALID_HANDLE_VALUE)
    {
        SerialHandle = CreateFileA("COM1", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    Emit("NTFSREG BEGIN");

    if (argc == 1)
    {
        if (!EnsureNtfsVolume())
        {
            Emit("NTFSREG FAIL format");
            Result = FALSE;
        }
        else if ((ReportVolumeDirtyBeforeCreate(),
                  !CreateDirectoryW(NtfsTestDir, NULL)) &&
                 GetLastError() != ERROR_ALREADY_EXISTS)
        {
            DWORD LastError = GetLastError();
            NTSTATUS LastStatus = RtlGetLastNtStatus();
            HANDLE Volume = OpenVolume();
            DWORD Flags = 0;
            DWORD Returned = 0;
            BOOL DirtyQuery = Volume != INVALID_HANDLE_VALUE &&
                              DeviceIoControl(Volume, FSCTL_IS_VOLUME_DIRTY,
                                              NULL, 0, &Flags, sizeof(Flags),
                                              &Returned, NULL) &&
                              Returned == sizeof(Flags);
            Emit("NTFSREG FAIL test-directory gle=%lu status=%08lx dirty-query=%u flags=%08lx",
                 LastError, LastStatus, DirtyQuery, Flags);
            if (Volume != INVALID_HANDLE_VALUE)
                CloseHandle(Volume);
            Result = FALSE;
        }
        else if (!ReadPhase(Phase, sizeof(Phase)))
        {
            Emit("NTFSREG FAIL phase-read");
            Result = FALSE;
        }
        else if (Phase[0] == L'\0')
        {
            Result = TestOverwriteAndFlush(NtfsTestDir);
            Result = TestExtend(NtfsTestDir) && Result;
            Result = TestRenameAndDelete(NtfsTestDir) && Result;
            Result = TestFlushCheckpoint(NtfsTestDir) && Result;
            Result = TestVolumeLock(NtfsTestDir) && Result;
            Result = TestAllocationOrdering(NtfsTestDir) && Result;
            Result = TestRemount(NtfsTestDir) && Result;
            if (Result && WritePhase(L"verify-remount"))
            {
                Emit("NTFSREG REBOOT-REQUEST");
                if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0))
                    Result = FALSE;
            }
            else
            {
                Result = FALSE;
            }
        }
        else if (wcscmp(Phase, L"verify-remount") == 0)
        {
            Result = VerifyRemount(NtfsTestDir);
            if (Result && WritePhase(L"crash"))
                Result = TestCrashRecovery(NtfsTestDir);
            else
                Result = FALSE;
        }
        else if (wcscmp(Phase, L"crash") == 0)
        {
            Result = VerifyCrashRecovery(NtfsTestDir);
            if (!DeletePhase())
                Result = FALSE;
        }
        else
        {
            Emit("NTFSREG FAIL unknown-phase");
            Result = FALSE;
        }
    }
    else if (argc >= 3)
    {
        Root = argv[1];
        Mode = argv[2];
        if (!CreateDirectoryW(Root, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
        {
            Emit("NTFSREG FAIL test-directory");
            Result = FALSE;
        }
        else if (wcscmp(Mode, L"full") == 0)
        {
            Result = TestOverwriteAndFlush(Root);
            Result = TestExtend(Root) && Result;
            Result = TestRenameAndDelete(Root) && Result;
        }
        else if (wcscmp(Mode, L"flush") == 0)
        {
            Result = TestFlushCheckpoint(Root);
        }
        else if (wcscmp(Mode, L"locks") == 0)
        {
            Result = TestVolumeLock(Root);
        }
        else if (wcscmp(Mode, L"allocation") == 0)
        {
            Result = TestAllocationOrdering(Root);
        }
        else if (wcscmp(Mode, L"dirty") == 0)
        {
            Result = TestDirtyState(Root, FALSE);
        }
        else if (wcscmp(Mode, L"verify-dirty") == 0)
        {
            Result = TestDirtyState(Root, TRUE);
        }
        else if (wcscmp(Mode, L"remount") == 0)
        {
            Result = TestRemount(Root);
        }
        else if (wcscmp(Mode, L"verify-remount") == 0)
        {
            Result = VerifyRemount(Root);
        }
        else if (wcscmp(Mode, L"crash") == 0)
        {
            Result = TestCrashRecovery(Root);
        }
        else if (wcscmp(Mode, L"verify-crash") == 0)
        {
            Result = VerifyCrashRecovery(Root);
        }
        else
        {
            Emit("NTFSREG FAIL unknown-mode");
            Result = FALSE;
        }
    }
    else
    {
        Emit("NTFSREG FAIL usage");
        Result = FALSE;
    }

    Emit("NTFSREG END %s", Result ? "PASS" : "FAIL");
    if (SerialHandle != INVALID_HANDLE_VALUE)
        CloseHandle(SerialHandle);

    return Result ? 0 : 1;
}
