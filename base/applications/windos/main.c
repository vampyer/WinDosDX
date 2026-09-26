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
 * up the platform backend, and runs the machine loop. Passing NULL lets the
 * host use its own defaults (windos.ini next to the executable).
 */
int
WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
         LPWSTR lpCmdLine, int nCmdShow)
{
    /* DOS names are 8-bit: the command line becomes the first command the
       emulated DOS runs, in the OEM code page, as "windos GAME.EXE" expects. */
    static char InitialCommand[256];

    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    while (*lpCmdLine == L' ' || *lpCmdLine == L'\t')
        lpCmdLine++;
    if (*lpCmdLine == L'\0' ||
        !WideCharToMultiByte(CP_OEMCP, 0, lpCmdLine, -1, InitialCommand,
                             sizeof(InitialCommand), NULL, NULL))
    {
        InitialCommand[0] = '\0';
    }

    if (WD_DosStart(NULL, InitialCommand[0] ? InitialCommand : NULL) != 0)
        return 1;

    WD_MachineRun();
    WD_MachineShutdown();
    return 0;
}
