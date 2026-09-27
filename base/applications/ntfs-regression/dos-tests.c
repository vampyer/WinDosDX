/*
 * DOS checks, run after the exFAT suites on the test disk
 *
 * The test programs are written onto the disk from the bytes below, then run
 * two ways: started like any Windows program (CreateProcess on a .COM, which
 * kernel32 hands to windos.exe, or to NTVDM when windos is absent or turned
 * off) and through windos.exe directly. Results go out as DOSREG lines and
 * do not change the overall result.
 */

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS

#include <windef.h>
#include <winbase.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "exfat-tests.h"

/*
 * VERTEST.COM: INT 21h AH=30h, then prints "WINDOS DOS M.NN" and writes the
 * same line to RESULT.TXT in the current directory.
 */
static const BYTE VerTestCom[] =
{
    0xB4, 0x30, 0xCD, 0x21, 0x88, 0xC3, 0x88, 0xE0, 0xD4, 0x0A, 0x05, 0x30,
    0x30, 0xA2, 0x4E, 0x01, 0x88, 0x26, 0x4D, 0x01, 0x80, 0xC3, 0x30, 0x88,
    0x1E, 0x4B, 0x01, 0xB4, 0x3C, 0x31, 0xC9, 0xBA, 0x52, 0x01, 0xCD, 0x21,
    0x89, 0xC3, 0xB4, 0x40, 0xB9, 0x11, 0x00, 0xBA, 0x40, 0x01, 0xCD, 0x21,
    0xB4, 0x3E, 0xCD, 0x21, 0xB4, 0x09, 0xBA, 0x40, 0x01, 0xCD, 0x21, 0xB8,
    0x00, 0x4C, 0xCD, 0x21, 0x57, 0x49, 0x4E, 0x44, 0x4F, 0x53, 0x20, 0x44,
    0x4F, 0x53, 0x20, 0x3F, 0x2E, 0x3F, 0x3F, 0x0D, 0x0A, 0x24, 0x52, 0x45,
    0x53, 0x55, 0x4C, 0x54, 0x2E, 0x54, 0x58, 0x54, 0x00,
};

/*
 * FPUTEST.COM: FINIT, FLD1, FADD ST,ST, FISTP, then writes "FPU 2" to
 * RESULT.TXT; without math coprocessor emulation it writes "FPU 0".
 */
static const BYTE FpuTestCom[] =
{
    0x9B, 0xDB, 0xE3, 0xD9, 0xE8, 0xD8, 0xC0, 0xDF, 0x1E, 0x32, 0x01, 0x9B,
    0xA0, 0x32, 0x01, 0x04, 0x30, 0xA2, 0x38, 0x01, 0xB4, 0x3C, 0x31, 0xC9,
    0xBA, 0x3C, 0x01, 0xCD, 0x21, 0x89, 0xC3, 0xB4, 0x40, 0xB9, 0x07, 0x00,
    0xBA, 0x34, 0x01, 0xCD, 0x21, 0xB4, 0x3E, 0xCD, 0x21, 0xB8, 0x00, 0x4C,
    0xCD, 0x21, 0x00, 0x00, 0x46, 0x50, 0x55, 0x20, 0x3F, 0x0D, 0x0A, 0x24,
    0x52, 0x45, 0x53, 0x55, 0x4C, 0x54, 0x2E, 0x54, 0x58, 0x54, 0x00,
};

/* EXIT42.COM: mov ax,4C2Ah ; int 21h */
static const BYTE Exit42Com[] = { 0xB8, 0x2A, 0x4C, 0xCD, 0x21 };

static const char ExpectedVersion[] = "WINDOS DOS 6.22\r\n";

static
BOOL
WriteBytes(PCWSTR Path, const BYTE *Data, DWORD Length)
{
    HANDLE File;
    DWORD Written = 0;
    BOOL Ok;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Data, Length, &Written, NULL) && Written == Length;
    CloseHandle(File);
    return Ok;
}

/* Reads a small text file into Buffer; returns FALSE if it is missing. */
static
BOOL
ReadSmallFile(PCWSTR Path, char *Buffer, DWORD Size)
{
    HANDLE File;
    DWORD Read = 0;

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(File, Buffer, Size - 1, &Read, NULL);
    CloseHandle(File);
    Buffer[Read] = '\0';
    return TRUE;
}

/* Prints Text with CR and LF spelled out, for the log. */
static
const char *
Visible(const char *Text, char *Buffer, size_t Size)
{
    size_t Used = 0;

    for (; *Text && Used + 5 < Size; Text++)
    {
        if (*Text == '\r')      { Buffer[Used++] = '\\'; Buffer[Used++] = 'r'; }
        else if (*Text == '\n') { Buffer[Used++] = '\\'; Buffer[Used++] = 'n'; }
        else if (*Text >= 0x20 && *Text < 0x7F) Buffer[Used++] = *Text;
        else Buffer[Used++] = '?';
    }
    Buffer[Used] = '\0';
    return Buffer;
}

/*
 * Starts CommandLine in Directory and waits up to Seconds for it to finish,
 * or for Directory\RESULT.TXT to appear when WaitForResult is set (windos.exe
 * stays open after its DOS program ends, so it is then terminated).
 */
static
BOOL
RunAndWait(PWSTR CommandLine, PCWSTR Directory, DWORD Seconds, BOOL WaitForResult,
           PDWORD ExitCode, const char *Name)
{
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Process;
    WCHAR Result[MAX_PATH];
    DWORD Waited;
    BOOL Done = FALSE;

    _snwprintf(Result, MAX_PATH, L"%s\\RESULT.TXT", Directory);
    Result[MAX_PATH - 1] = L'\0';

    if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL,
                        Directory, &Startup, &Process))
    {
        Emit("DOSREG FAIL %s start gle=%lu", Name, GetLastError());
        return FALSE;
    }
    /* The harness takes a screenshot of the guest after this line: 20 s
       later, or 4 s for the long-name launches, which end quickly. */
    Emit("DOSREG WAIT %s%s", Name, strstr(Name, "longname") ? " 4" : "");

    for (Waited = 0; Waited < Seconds * 10 && !Done; Waited++)
    {
        if (WaitForSingleObject(Process.hProcess, 100) == WAIT_OBJECT_0)
            Done = TRUE;
        else if (WaitForResult && GetFileAttributesW(Result) != INVALID_FILE_ATTRIBUTES)
            Done = TRUE;
    }

    if (WaitForSingleObject(Process.hProcess, 0) == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(Process.hProcess, ExitCode);
    }
    else
    {
        if (!Done)
            Emit("DOSREG INFO %s still running after %lu s; stopping it", Name, Seconds);
        /* Let the result file be closed before the program is stopped. */
        Sleep(1000);
        TerminateProcess(Process.hProcess, 0xDEAD);
        WaitForSingleObject(Process.hProcess, 5000);
        *ExitCode = 0xDEAD;
    }
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    return Done;
}

static
BOOL
MakeDirectory(PCWSTR Relative, PWSTR Path)
{
    ExfatPath(Path, Relative);
    return CreateDirectoryW(Path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static
VOID
CheckResult(PCWSTR Directory, const char *Name, const char *Expected)
{
    WCHAR Path[MAX_PATH];
    char Text[64];
    char Shown[128];

    _snwprintf(Path, MAX_PATH, L"%s\\RESULT.TXT", Directory);
    Path[MAX_PATH - 1] = L'\0';
    if (!ReadSmallFile(Path, Text, sizeof(Text)))
        Emit("DOSREG FAIL %s no RESULT.TXT", Name);
    else if (strcmp(Text, Expected) != 0)
        Emit("DOSREG FAIL %s reported \"%s\"", Name, Visible(Text, Shown, sizeof(Shown)));
    else
        Emit("DOSREG PASS %s", Name);
}

static
VOID
CheckVersionResult(PCWSTR Directory, const char *Name)
{
    CheckResult(Directory, Name, ExpectedVersion);
}

/* Emits each line of a text file as "DOSREG LOG <tag>: <line>". */
static
VOID
EmitFileLines(PCWSTR Path, const char *Tag)
{
    char Text[4096];
    char *Line, *Next;

    if (!ReadSmallFile(Path, Text, sizeof(Text)))
    {
        Emit("DOSREG LOG %s: (no file)", Tag);
        return;
    }
    for (Line = Text; Line && *Line; Line = Next)
    {
        Next = strpbrk(Line, "\r\n");
        if (Next)
        {
            *Next++ = '\0';
            while (*Next == '\r' || *Next == '\n') Next++;
        }
        if (*Line)
            Emit("DOSREG LOG %s: %s", Tag, Line);
    }
}

/* windos.log: %APPDATA%\WinDosDX, or %TEMP%\WinDosDX without APPDATA. */
static
VOID
EmitWindosLog(void)
{
    WCHAR Path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", Path, MAX_PATH - 30);

    if (n == 0 || n >= MAX_PATH - 30)
    {
        n = GetTempPathW(MAX_PATH - 30, Path);
        if (n > 0 && Path[n - 1] == L'\\')
            Path[n - 1] = L'\0';
    }
    wcscat(Path, L"\\WinDosDX\\windos.log");
    EmitFileLines(Path, "windos.log");
}

/*
 * What the DOS machine sees in Directory: its DIR listing (the names DOS
 * gives the files) and windos.log, for when a program is not found.
 */
static
VOID
DumpDosView(PCWSTR Directory)
{
    WCHAR Windos[MAX_PATH];
    WCHAR CommandLine[MAX_PATH * 2];
    WCHAR Path[MAX_PATH];
    DWORD ExitCode;

    static const char ListDir[] = "DIR > DIRLIST.TXT\r\n";

    /* A batch file in Directory, so windos.exe makes Directory C:. */
    GetWindowsDirectoryW(Windos, MAX_PATH);
    wcscat(Windos, L"\\windos.exe");
    _snwprintf(Path, MAX_PATH, L"%s\\LISTDIR.BAT", Directory);
    WriteBytes(Path, (const BYTE *)ListDir, sizeof(ListDir) - 1);
    _snwprintf(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" \"%s\"", Windos, Path);
    CommandLine[ARRAYSIZE(CommandLine) - 1] = L'\0';
    RunAndWait(CommandLine, Directory, 30, FALSE, &ExitCode, "dos-dir-listing");
    _snwprintf(Path, MAX_PATH, L"%s\\DIRLIST.TXT", Directory);
    EmitFileLines(Path, "dir");

    EmitWindosLog();
}

static
VOID
TestLaunch(void)
{
    WCHAR Directory[MAX_PATH];
    WCHAR Program[MAX_PATH];
    WCHAR CommandLine[MAX_PATH];
    DWORD ExitCode = 0;

    if (!MakeDirectory(L"dosreg-launch", Directory))
    {
        Emit("DOSREG FAIL launch mkdir gle=%lu", GetLastError());
        return;
    }

    /* The DOS version NTVDM reports. */
    _snwprintf(Program, MAX_PATH, L"%s\\VERTEST.COM", Directory);
    if (!WriteBytes(Program, VerTestCom, sizeof(VerTestCom)))
    {
        Emit("DOSREG FAIL launch-version write gle=%lu", GetLastError());
        return;
    }
    _snwprintf(CommandLine, MAX_PATH, L"\"%s\"", Program);
    if (RunAndWait(CommandLine, Directory, 60, FALSE, &ExitCode, "launch-version"))
    {
        if (ExitCode != 0)
            Emit("DOSREG FAIL launch-version exit code %lu", ExitCode);
        CheckVersionResult(Directory, "launch-version");
    }
    else
    {
        Emit("DOSREG FAIL launch-version did not finish");
    }

    /* A long, non-8.3 name: exFAT has no short names, so the DOS machine
       has to find the name it gives the file itself. */
    _snwprintf(Program, MAX_PATH, L"%s\\Long Name Test.com", Directory);
    {
        WCHAR Result[MAX_PATH];
        _snwprintf(Result, MAX_PATH, L"%s\\RESULT.TXT", Directory);
        DeleteFileW(Result);
    }
    if (!WriteBytes(Program, VerTestCom, sizeof(VerTestCom)))
    {
        Emit("DOSREG FAIL launch-longname write gle=%lu", GetLastError());
    }
    else
    {
        _snwprintf(CommandLine, MAX_PATH, L"\"%s\"", Program);
        if (RunAndWait(CommandLine, Directory, 60, FALSE, &ExitCode, "launch-longname"))
        {
            WCHAR Result[MAX_PATH];
            _snwprintf(Result, MAX_PATH, L"%s\\RESULT.TXT", Directory);
            if (GetFileAttributesW(Result) == INVALID_FILE_ATTRIBUTES)
                DumpDosView(Directory);
            CheckVersionResult(Directory, "launch-longname");
        }
        else
        {
            Emit("DOSREG FAIL launch-longname did not finish");
        }
    }

    /* A DOS program's exit code reaches the Win32 parent. */
    _snwprintf(Program, MAX_PATH, L"%s\\EXIT42.COM", Directory);
    if (!WriteBytes(Program, Exit42Com, sizeof(Exit42Com)))
    {
        Emit("DOSREG FAIL launch-exitcode write gle=%lu", GetLastError());
        return;
    }
    _snwprintf(CommandLine, MAX_PATH, L"\"%s\"", Program);
    if (!RunAndWait(CommandLine, Directory, 60, FALSE, &ExitCode, "launch-exitcode"))
        Emit("DOSREG FAIL launch-exitcode did not finish");
    else if (ExitCode != 42)
        Emit("DOSREG FAIL launch-exitcode got %lu, expected 42", ExitCode);
    else
        Emit("DOSREG PASS launch-exitcode");
}

static
VOID
TestWindos(void)
{
    WCHAR Windos[MAX_PATH];
    WCHAR Directory[MAX_PATH];
    WCHAR Program[MAX_PATH];
    WCHAR CommandLine[MAX_PATH * 2];
    DWORD ExitCode = 0;

    /* windos.exe is only on CDs built with WINDOSDX_BUILD_DOS_HOST=ON. */
    GetWindowsDirectoryW(Windos, MAX_PATH);
    wcscat(Windos, L"\\windos.exe");
    if (GetFileAttributesW(Windos) == INVALID_FILE_ATTRIBUTES)
    {
        Emit("DOSREG INFO windos.exe is not on this CD; skipping the DOSBox machine");
        return;
    }

    if (!MakeDirectory(L"dosreg-windos", Directory))
    {
        Emit("DOSREG FAIL windos mkdir gle=%lu", GetLastError());
        return;
    }
    _snwprintf(Program, MAX_PATH, L"%s\\VERTEST.COM", Directory);
    if (!WriteBytes(Program, VerTestCom, sizeof(VerTestCom)))
    {
        Emit("DOSREG FAIL windos-version write gle=%lu", GetLastError());
        return;
    }

    /* Without a windos.ini, the current directory becomes DOS drive C:. */
    _snwprintf(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" VERTEST.COM", Windos);
    CommandLine[ARRAYSIZE(CommandLine) - 1] = L'\0';
    if (!RunAndWait(CommandLine, Directory, 120, TRUE, &ExitCode, "windos-version"))
    {
        Emit("DOSREG FAIL windos-version no result (exit code %lu)", ExitCode);
        return;
    }
    CheckVersionResult(Directory, "windos-version");

    /* The long name given to windos.exe directly, without kernel32. */
    _snwprintf(Program, MAX_PATH, L"%s\\Long Name Test.com", Directory);
    if (!WriteBytes(Program, VerTestCom, sizeof(VerTestCom)))
    {
        Emit("DOSREG FAIL windos-longname write gle=%lu", GetLastError());
        return;
    }
    {
        WCHAR Result[MAX_PATH];
        _snwprintf(Result, MAX_PATH, L"%s\\RESULT.TXT", Directory);
        DeleteFileW(Result);
    }
    _snwprintf(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" \"%s\"", Windos, Program);
    CommandLine[ARRAYSIZE(CommandLine) - 1] = L'\0';
    if (!RunAndWait(CommandLine, Directory, 120, TRUE, &ExitCode, "windos-longname"))
    {
        Emit("DOSREG FAIL windos-longname no result (exit code %lu)", ExitCode);
        return;
    }
    {
        WCHAR Result[MAX_PATH];
        _snwprintf(Result, MAX_PATH, L"%s\\RESULT.TXT", Directory);
        if (GetFileAttributesW(Result) == INVALID_FILE_ATTRIBUTES)
            DumpDosView(Directory);
    }
    CheckVersionResult(Directory, "windos-longname");

    /* Math coprocessor emulation: 1 + 1 on the FPU. */
    _snwprintf(Program, MAX_PATH, L"%s\\FPUTEST.COM", Directory);
    {
        WCHAR Result[MAX_PATH];
        _snwprintf(Result, MAX_PATH, L"%s\\RESULT.TXT", Directory);
        DeleteFileW(Result);
    }
    if (!WriteBytes(Program, FpuTestCom, sizeof(FpuTestCom)))
    {
        Emit("DOSREG FAIL windos-fpu write gle=%lu", GetLastError());
        return;
    }
    _snwprintf(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" \"%s\"", Windos, Program);
    CommandLine[ARRAYSIZE(CommandLine) - 1] = L'\0';
    if (!RunAndWait(CommandLine, Directory, 120, TRUE, &ExitCode, "windos-fpu"))
    {
        Emit("DOSREG FAIL windos-fpu no result (exit code %lu)", ExitCode);
        return;
    }
    CheckResult(Directory, "windos-fpu", "FPU 2\r\n");
}

VOID
DosRunTests(void)
{
    WCHAR AppData[MAX_PATH];

    Emit("DOSREG BEGIN");
    /* This session has no user profile: give windos.exe (which inherits the
       environment) a writable APPDATA on the test disk for its log. */
    if (!GetEnvironmentVariableW(L"APPDATA", AppData, MAX_PATH))
    {
        ExfatPath(AppData, L"dosreg-appdata");
        CreateDirectoryW(AppData, NULL);
        SetEnvironmentVariableW(L"APPDATA", AppData);
    }
    TestLaunch();
    TestWindos();
    Emit("DOSREG END");
}
