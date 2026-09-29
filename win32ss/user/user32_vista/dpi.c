/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DPI functions for user32 and user32_vista.
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <cbialo2@outlook.com>
 *              WinDosDX: awareness contexts and the Windows 10 *ForDpi functions
 *
 * WinDosDX draws everything at one system DPI and never rescales windows.
 * A program may still declare itself DPI aware (once per process, as on
 * Windows) and ask for metrics at another DPI; the answers here are what
 * Windows gives on a single-monitor system at the system DPI.
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winerror.h>

HDC APIENTRY
NtUserGetDC(HWND hWnd);

#define NDEBUG
#include <debug.h>

/* The process awareness; set at most once, like on Windows. */
static LONG ProcessAwareness = -1;      /* -1: not set, DPI_AWARENESS_UNAWARE */

/* A thread's awareness override (SetThreadDpiAwarenessContext), in TLS. */
static DWORD ThreadAwarenessTls = TLS_OUT_OF_INDEXES;

UINT
WINAPI
GetDpiForSystem(VOID)
{
    HDC hDC;
    UINT Dpi;
    hDC = NtUserGetDC(NULL);
    Dpi = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(NULL, hDC);
    return Dpi;
}

UINT
WINAPI
GetDpiForWindow(
    _In_ HWND hWnd)
{
    if (!IsWindow(hWnd))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    return GetDpiForSystem();
}

static DPI_AWARENESS
CurrentProcessAwareness(VOID)
{
    LONG Awareness = ProcessAwareness;
    return Awareness < 0 ? DPI_AWARENESS_UNAWARE : (DPI_AWARENESS)Awareness;
}

DPI_AWARENESS
WINAPI
GetAwarenessFromDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT Context)
{
    switch ((LONG_PTR)Context)
    {
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_UNAWARE:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED:
            return DPI_AWARENESS_UNAWARE;
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_SYSTEM_AWARE:
            return DPI_AWARENESS_SYSTEM_AWARE;
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2:
            return DPI_AWARENESS_PER_MONITOR_AWARE;
        default:
            return DPI_AWARENESS_INVALID;
    }
}

static DPI_AWARENESS_CONTEXT
ContextFromAwareness(DPI_AWARENESS Awareness)
{
    switch (Awareness)
    {
        case DPI_AWARENESS_SYSTEM_AWARE:
            return DPI_AWARENESS_CONTEXT_SYSTEM_AWARE;
        case DPI_AWARENESS_PER_MONITOR_AWARE:
            return DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;
        default:
            return DPI_AWARENESS_CONTEXT_UNAWARE;
    }
}

BOOL
WINAPI
IsValidDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT Context)
{
    return GetAwarenessFromDpiAwarenessContext(Context) != DPI_AWARENESS_INVALID;
}

BOOL
WINAPI
AreDpiAwarenessContextsEqual(
    _In_ DPI_AWARENESS_CONTEXT Context1,
    _In_ DPI_AWARENESS_CONTEXT Context2)
{
    DPI_AWARENESS A = GetAwarenessFromDpiAwarenessContext(Context1);
    return A != DPI_AWARENESS_INVALID && A == GetAwarenessFromDpiAwarenessContext(Context2);
}

UINT
WINAPI
GetDpiFromDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT Context)
{
    switch (GetAwarenessFromDpiAwarenessContext(Context))
    {
        case DPI_AWARENESS_UNAWARE:
            return USER_DEFAULT_SCREEN_DPI;
        case DPI_AWARENESS_SYSTEM_AWARE:
            return GetDpiForSystem();
        default:
            return 0;   /* per monitor: no single DPI */
    }
}

BOOL
WINAPI
SetProcessDpiAwarenessInternal(
    _In_ DPI_AWARENESS awareness)
{
    if (awareness < DPI_AWARENESS_UNAWARE || awareness > DPI_AWARENESS_PER_MONITOR_AWARE)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (InterlockedCompareExchange(&ProcessAwareness, awareness, -1) != -1)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return TRUE;
}

BOOL
WINAPI
GetProcessDpiAwarenessInternal(
    _In_  HANDLE process,
    _Out_ DPI_AWARENESS *awareness)
{
    if (!awareness)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* Other processes are reported as unaware: their setting is not visible here. */
    if (!process || process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        *awareness = CurrentProcessAwareness();
    else
        *awareness = DPI_AWARENESS_UNAWARE;
    return TRUE;
}

BOOL
WINAPI
SetProcessDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT context)
{
    DPI_AWARENESS Awareness = GetAwarenessFromDpiAwarenessContext(context);

    if (Awareness == DPI_AWARENESS_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return SetProcessDpiAwarenessInternal(Awareness);
}

BOOL
WINAPI
IsProcessDPIAware(VOID)
{
    return CurrentProcessAwareness() != DPI_AWARENESS_UNAWARE;
}

BOOL
WINAPI
SetProcessDPIAware(VOID)
{
    /* Already aware (or declared otherwise) is not an error for this one. */
    InterlockedCompareExchange(&ProcessAwareness, DPI_AWARENESS_SYSTEM_AWARE, -1);
    return TRUE;
}

UINT
WINAPI
GetSystemDpiForProcess(
    _In_ HANDLE hProcess)
{
    UNREFERENCED_PARAMETER(hProcess);
    return GetDpiForSystem();
}

/* Thread contexts: a per-thread override of the process awareness. */

DPI_AWARENESS_CONTEXT
WINAPI
GetThreadDpiAwarenessContext(VOID)
{
    PVOID Value = ThreadAwarenessTls != TLS_OUT_OF_INDEXES ? TlsGetValue(ThreadAwarenessTls) : NULL;

    if (Value)
        return (DPI_AWARENESS_CONTEXT)Value;
    return ContextFromAwareness(CurrentProcessAwareness());
}

DPI_AWARENESS_CONTEXT
WINAPI
SetThreadDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT Context)
{
    DPI_AWARENESS_CONTEXT Previous;

    if (!IsValidDpiAwarenessContext(Context))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (ThreadAwarenessTls == TLS_OUT_OF_INDEXES)
    {
        DWORD Slot = TlsAlloc();
        if (Slot == TLS_OUT_OF_INDEXES)
            return NULL;
        if (InterlockedCompareExchange((LONG *)&ThreadAwarenessTls, (LONG)Slot, (LONG)TLS_OUT_OF_INDEXES) !=
            (LONG)TLS_OUT_OF_INDEXES)
        {
            TlsFree(Slot);
        }
    }
    Previous = GetThreadDpiAwarenessContext();
    TlsSetValue(ThreadAwarenessTls, (PVOID)Context);
    return Previous;
}

/* Exported as GetWindowDpiAwarenessContext: every window has the awareness
 * of the process that made it. */
DPI_AWARENESS_CONTEXT
WINAPI
WdxGetWindowDpiAwarenessContext(
    _In_ HWND hWnd)
{
    DWORD ProcessId = 0;

    if (!GetWindowThreadProcessId(hWnd, &ProcessId))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return NULL;
    }
    if (ProcessId == GetCurrentProcessId())
        return ContextFromAwareness(CurrentProcessAwareness());
    return DPI_AWARENESS_CONTEXT_UNAWARE;
}

/* Exported as GetDpiAwarenessContextForProcess. */
DPI_AWARENESS_CONTEXT
WINAPI
WdxGetDpiAwarenessContextForProcess(
    _In_ HANDLE hProcess)
{
    DPI_AWARENESS Awareness;

    if (!GetProcessDpiAwarenessInternal(hProcess, &Awareness))
        return NULL;
    return ContextFromAwareness(Awareness);
}

BOOL
WINAPI
GetDpiForMonitorInternal(
    _In_  HMONITOR monitor,
    _In_  UINT type,
    _Out_ UINT *x,
    _Out_ UINT *y)
{
    UNREFERENCED_PARAMETER(monitor);

    if (type > 2 || !x || !y)   /* effective, angular, raw */
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *x = *y = GetDpiForSystem();
    return TRUE;
}

/* Metrics at another DPI: sizes scale with the DPI, counts and flags do not. */
static BOOL
IsSizeMetric(int Index)
{
    switch (Index)
    {
        case SM_CXVSCROLL: case SM_CYHSCROLL: case SM_CYCAPTION:
        case SM_CXBORDER: case SM_CYBORDER: case SM_CXDLGFRAME: case SM_CYDLGFRAME:
        case SM_CYVTHUMB: case SM_CXHTHUMB: case SM_CXICON: case SM_CYICON:
        case SM_CXCURSOR: case SM_CYCURSOR: case SM_CYMENU: case SM_CYVSCROLL:
        case SM_CXHSCROLL: case SM_CXMIN: case SM_CYMIN: case SM_CXSIZE: case SM_CYSIZE:
        case SM_CXFRAME: case SM_CYFRAME: case SM_CXMINTRACK: case SM_CYMINTRACK:
        case SM_CXICONSPACING: case SM_CYICONSPACING: case SM_CXEDGE: case SM_CYEDGE:
        case SM_CXSMICON: case SM_CYSMICON: case SM_CYSMCAPTION: case SM_CXSMSIZE:
        case SM_CYSMSIZE: case SM_CXMENUSIZE: case SM_CYMENUSIZE: case SM_CXMENUCHECK:
        case SM_CYMENUCHECK: case SM_CXFOCUSBORDER: case SM_CYFOCUSBORDER:
        case 92: /* SM_CXPADDEDBORDER (Vista) */
            return TRUE;
        default:
            return FALSE;
    }
}

int
WINAPI
GetSystemMetricsForDpi(
    _In_ int nIndex,
    _In_ UINT dpi)
{
    int Value = GetSystemMetrics(nIndex);
    UINT System = GetDpiForSystem();

    if (!dpi || dpi == System || !IsSizeMetric(nIndex))
        return Value;
    return MulDiv(Value, dpi, System);
}

BOOL
WINAPI
AdjustWindowRectExForDpi(
    _Inout_ LPRECT lpRect,
    _In_ DWORD dwStyle,
    _In_ BOOL bMenu,
    _In_ DWORD dwExStyle,
    _In_ UINT dpi)
{
    UNREFERENCED_PARAMETER(dpi);
    return AdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle);
}

BOOL
WINAPI
SystemParametersInfoForDpi(
    _In_ UINT uiAction,
    _In_ UINT uiParam,
    _Inout_ PVOID pvParam,
    _In_ UINT fWinIni,
    _In_ UINT dpi)
{
    UNREFERENCED_PARAMETER(dpi);

    /* Windows accepts only these; each returns sizes at the system DPI here. */
    switch (uiAction)
    {
        case SPI_GETICONTITLELOGFONT:
        case SPI_GETICONMETRICS:
        case SPI_GETNONCLIENTMETRICS:
            return SystemParametersInfoW(uiAction, uiParam, pvParam, fWinIni);
        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }
}

BOOL
WINAPI
EnableNonClientDpiScaling(
    _In_ HWND hwnd)
{
    /* Non-client areas are drawn at the one system DPI already. */
    UNREFERENCED_PARAMETER(hwnd);
    return TRUE;
}

/* Logical and physical coordinates are the same: there is no scaling. */

BOOL
WINAPI
LogicalToPhysicalPoint(
    _In_ HWND hwnd,
    _Inout_ POINT *point)
{
    UNREFERENCED_PARAMETER(hwnd);
    return point != NULL;
}

BOOL
WINAPI
PhysicalToLogicalPoint(
    _In_ HWND hwnd,
    _Inout_ POINT *point)
{
    UNREFERENCED_PARAMETER(hwnd);
    return point != NULL;
}

BOOL
WINAPI
LogicalToPhysicalPointForPerMonitorDPI(
    _In_opt_ HWND hwnd,
    _Inout_ POINT *point)
{
    UNREFERENCED_PARAMETER(hwnd);
    return point != NULL;
}

BOOL
WINAPI
PhysicalToLogicalPointForPerMonitorDPI(
    _In_opt_ HWND hwnd,
    _Inout_ POINT *point)
{
    UNREFERENCED_PARAMETER(hwnd);
    return point != NULL;
}
