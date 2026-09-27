/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        windos.exe entry point
 * PROGRAMMERS:    WinDosDX Team
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "windos_core/include/windos_platform.h"
#include "windos_core/include/windos_dos.h"

/*
 * windos.exe is a thin shell around the DOSBox-derived core. It hands control
 * to the host (windos_core/src/windos_host.c), which loads windos.ini, brings
 * up the platform backend, and runs the machine loop.
 *
 *   windos.exe                      DOS prompt; C: is %USERPROFILE%\DOS unless
 *                                   windos.ini names one
 *   windos.exe D:\GAMES\X\X.EXE a   run X.EXE with argument "a", its folder as
 *                                   C:, and close when it ends (how Windows
 *                                   starts DOS programs; see kernel32)
 *   windos.exe DIR /W               any other text is a DOS command
 */

/* Text the DOS program sees (its arguments) is in the OEM code page. */
static BOOL
ToDos(LPCWSTR Text, char *Out, int Size)
{
    return WideCharToMultiByte(CP_OEMCP, 0, Text, -1, Out, Size, NULL, NULL) > 0;
}

/* Windows paths the DOS machine opens on the host use the ANSI code page. */
static BOOL
ToHost(LPCWSTR Text, char *Out, int Size)
{
    return WideCharToMultiByte(CP_ACP, 0, Text, -1, Out, Size, NULL, NULL) > 0;
}

/* Splits the first (possibly quoted) token off CmdLine. */
static LPCWSTR
FirstToken(LPCWSTR CmdLine, WCHAR *Token, DWORD Size)
{
    DWORD Used = 0;
    BOOL Quoted = (*CmdLine == L'"');

    if (Quoted)
        CmdLine++;
    while (*CmdLine && (Quoted ? *CmdLine != L'"' : (*CmdLine != L' ' && *CmdLine != L'\t')))
    {
        if (Used + 1 < Size)
            Token[Used++] = *CmdLine;
        CmdLine++;
    }
    if (Quoted && *CmdLine == L'"')
        CmdLine++;
    Token[Used] = L'\0';
    while (*CmdLine == L' ' || *CmdLine == L'\t')
        CmdLine++;
    return CmdLine;
}

/* Whether files can be created in Folder (not a CD or a read-only share). */
static BOOL
IsWritableFolder(LPCWSTR Folder)
{
    WCHAR Probe[MAX_PATH];
    HANDLE File;

    if (_snwprintf(Probe, ARRAYSIZE(Probe), L"%s%s~wdwrite.tmp", Folder,
                   Folder[wcslen(Folder) - 1] == L'\\' ? L"" : L"\\") < 0)
        return FALSE;
    File = CreateFileW(Probe, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(File);
        return TRUE;
    }
    return GetLastError() == ERROR_FILE_EXISTS;
}

/*
 * Copies the folder tree From to To. Files already in To are kept, so what a
 * program saved earlier in the session survives a second start. Copies of
 * files from a CD come read-only; that is cleared, so the program can
 * update its own files.
 */
static BOOL
CopyTree(LPCWSTR From, LPCWSTR To)
{
    WCHAR Pattern[MAX_PATH], Source[MAX_PATH], Target[MAX_PATH];
    WIN32_FIND_DATAW Data;
    HANDLE Find;
    BOOL Ok = TRUE;

    if (!CreateDirectoryW(To, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    _snwprintf(Pattern, ARRAYSIZE(Pattern), L"%s\\*", From);
    Pattern[ARRAYSIZE(Pattern) - 1] = L'\0';
    Find = FindFirstFileW(Pattern, &Data);
    if (Find == INVALID_HANDLE_VALUE)
        return FALSE;
    do
    {
        if (!wcscmp(Data.cFileName, L".") || !wcscmp(Data.cFileName, L".."))
            continue;
        _snwprintf(Source, ARRAYSIZE(Source), L"%s\\%s", From, Data.cFileName);
        _snwprintf(Target, ARRAYSIZE(Target), L"%s\\%s", To, Data.cFileName);
        Source[ARRAYSIZE(Source) - 1] = Target[ARRAYSIZE(Target) - 1] = L'\0';
        if (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            Ok = CopyTree(Source, Target) && Ok;
        }
        else if (CopyFileW(Source, Target, TRUE))
        {
            SetFileAttributesW(Target, Data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
        }
        else if (GetLastError() != ERROR_FILE_EXISTS)
        {
            Ok = FALSE;
        }
    } while (FindNextFileW(Find, &Data));
    FindClose(Find);
    return Ok;
}

/*
 * A program on a CD (the live CD's DOSGames folder, say) cannot write its
 * settings or saved games there. Run a copy in %TEMP%\WinDosDX\<folder>
 * instead; it lasts for the session. Folder is replaced by the copy's path.
 */
static VOID
UseWritableCopy(LPWSTR Folder, DWORD Size)
{
    WCHAR Copy[MAX_PATH];
    LPCWSTR Leaf;
    DWORD n;

    if (IsWritableFolder(Folder))
        return;
    n = GetTempPathW(ARRAYSIZE(Copy), Copy);
    if (n == 0 || n >= ARRAYSIZE(Copy) - 40)
        return;
    wcscat(Copy, L"WinDosDX");
    CreateDirectoryW(Copy, NULL);
    Leaf = wcsrchr(Folder, L'\\');
    Leaf = (Leaf && Leaf[1]) ? Leaf + 1 : L"program";
    if (wcslen(Copy) + 1 + wcslen(Leaf) >= ARRAYSIZE(Copy))
        return;
    wcscat(Copy, L"\\");
    wcscat(Copy, Leaf);
    if (CopyTree(Folder, Copy) && wcslen(Copy) < Size)
        wcscpy(Folder, Copy);
}

int
WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
         LPWSTR lpCmdLine, int nCmdShow)
{
    static char InitialCommand[512];
    static char ProgramDrive[MAX_PATH];
    static char DefaultDrive[MAX_PATH];
    WD_MachineConfig *Config = NULL;
    WCHAR Token[MAX_PATH];
    WCHAR FullPath[MAX_PATH];
    WIN32_FIND_DATAW Found;
    HANDLE Find;
    LPCWSTR Arguments;
    LPWSTR FilePart = NULL;
    DWORD Attributes;
    int ExitCode;

    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    while (*lpCmdLine == L' ' || *lpCmdLine == L'\t')
        lpCmdLine++;

    Arguments = FirstToken(lpCmdLine, Token, ARRAYSIZE(Token));
    Attributes = Token[0] ? GetFileAttributesW(Token) : INVALID_FILE_ATTRIBUTES;

    if (Attributes != INVALID_FILE_ATTRIBUTES && !(Attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        GetFullPathNameW(Token, ARRAYSIZE(FullPath), FullPath, &FilePart) && FilePart)
    {
        /* A program file: its folder is C:, it runs, and the window closes
           when it ends. */
        char Name[MAX_PATH];
        char DosArguments[256];

        /* The name as stored on disk; WDRUN finds the DOS name DOSBox gives
           it, which is not the Windows 8.3 alias and exists even where the
           drive has no short names. */
        Find = FindFirstFileW(FullPath, &Found);
        if (Find != INVALID_HANDLE_VALUE)
        {
            FindClose(Find);
            ToHost(Found.cFileName, Name, sizeof(Name));
        }
        else
        {
            ToHost(FilePart, Name, sizeof(Name));
        }
        if (!ToDos(Arguments, DosArguments, sizeof(DosArguments)))
            DosArguments[0] = '\0';

        /* FullPath becomes the folder; a root folder keeps its "X:\". */
        if (FilePart - FullPath == 3)
            FilePart[0] = L'\0';
        else
            FilePart[-1] = L'\0';
        UseWritableCopy(FullPath, ARRAYSIZE(FullPath));
        ToHost(FullPath, ProgramDrive, sizeof(ProgramDrive));
        WD_DosSetProgram(ProgramDrive, Name);

        wsprintfA(InitialCommand, "WDRUN \"%s\"%s%s\nEXIT", Name,
                  DosArguments[0] ? " " : "", DosArguments);
    }
    else
    {
        /* A DOS command, or nothing: the DOS prompt on the user's DOS drive. */
        DWORD n;

        if (lpCmdLine[0] && !ToDos(lpCmdLine, InitialCommand, sizeof(InitialCommand)))
            InitialCommand[0] = '\0';

        n = GetEnvironmentVariableA("USERPROFILE", DefaultDrive, MAX_PATH - 5);
        if (n > 0 && n < MAX_PATH - 5)
        {
            lstrcatA(DefaultDrive, "\\DOS");
            WD_DosSetDefaultDrive(DefaultDrive);
        }
    }

    if (WD_DosStart(Config, InitialCommand[0] ? InitialCommand : NULL) != 0)
        return 1;

    ExitCode = WD_MachineRun();
    WD_MachineShutdown();
    return ExitCode;
}
