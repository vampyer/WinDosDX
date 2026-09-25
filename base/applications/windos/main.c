/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        windos.exe entry point
 * PROGRAMMERS:    WinDosDX Team
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "windos_core/include/windos_platform.h"

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
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (WD_MachineInit(NULL) != 0)
        return 1;

    WD_MachineRun();
    WD_MachineShutdown();
    return 0;
}
