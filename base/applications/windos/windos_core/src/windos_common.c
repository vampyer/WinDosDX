/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        Common platform state for the WinDosDX backend
 * PROGRAMMERS:    WinDosDX Team
 */

#include "windos_internal.h"

static WD_GlobalState g_wd_global = {0, 0, WD_DOS_STATE_STOPPED};

WD_GlobalState *WD_GetGlobal(void)
{
    return &g_wd_global;
}

int WD_PlatformReady(void)
{
    return g_wd_global.ready;
}

WD_DosState WD_DosGetState(void)
{
    return g_wd_global.state;
}

void WD_PlatformRequestQuit(void)
{
    if (g_wd_global.state == WD_DOS_STATE_RUNNING ||
        g_wd_global.state == WD_DOS_STATE_STARTING)
    {
        g_wd_global.state = WD_DOS_STATE_STOPPING;
    }
    g_wd_global.quit_requested = 1;
    /* Nudge the message loop so a blocking pump returns promptly. */
    {
        HWND hwnd = WD_GetVideoWindow();
        if (hwnd)
            PostMessage(hwnd, WM_CLOSE, 0, 0);
    }
}
