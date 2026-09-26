/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        windos.exe entry point
 * PROGRAMMERS:    WinDosDX Team
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

/* DOS names are 8-bit and 8.3: the OEM code page, and the short name when the
   file system has one. */
static BOOL
ToDos(LPCWSTR Text, char *Out, int Size)
{
    return WideCharToMultiByte(CP_OEMCP, 0, Text, -1, Out, Size, NULL, NULL) > 0;
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
    WCHAR ShortPath[MAX_PATH];
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

        if (GetShortPathNameW(FullPath, ShortPath, ARRAYSIZE(ShortPath)))
        {
            LPWSTR ShortName = wcsrchr(ShortPath, L'\\');
            ToDos(ShortName ? ShortName + 1 : ShortPath, Name, sizeof(Name));
        }
        else
        {
            ToDos(FilePart, Name, sizeof(Name));
        }
        if (!ToDos(Arguments, DosArguments, sizeof(DosArguments)))
            DosArguments[0] = '\0';

        /* FullPath becomes the folder; a root folder keeps its "X:\". */
        if (FilePart - FullPath == 3)
            FilePart[0] = L'\0';
        else
            FilePart[-1] = L'\0';
        ToDos(FullPath, ProgramDrive, sizeof(ProgramDrive));
        WD_DosSetProgram(ProgramDrive, Name);

        wsprintfA(InitialCommand, "%s%s%s\nEXIT", Name, DosArguments[0] ? " " : "", DosArguments);
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
