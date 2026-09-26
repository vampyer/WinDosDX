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

/*
 * Per-user data file: %APPDATA%\WinDosDX\<file>, creating the folder. The
 * program folder (often the read-only Windows directory) is the fallback
 * when there is no APPDATA.
 */
void WD_DataPath(char *out, size_t max, const char *file)
{
    char dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", dir, MAX_PATH);

    /* Without APPDATA (a session with no user profile), the TEMP folder:
       the program's own folder is often read-only. */
    if (n == 0 || n + 10 >= MAX_PATH)
    {
        n = GetTempPathA(MAX_PATH, dir);
        if (n > 0 && n < MAX_PATH && dir[n - 1] == '\\')
            dir[--n] = '\0';
    }
    if (n > 0 && n + 10 < MAX_PATH)
    {
        lstrcatA(dir, "\\WinDosDX");
        CreateDirectoryA(dir, NULL);
    }
    else
    {
        char *slash;
        n = GetModuleFileNameA(NULL, dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            dir[0] = '\0';
        slash = strrchr(dir, '\\');
        if (slash)
            *slash = '\0';
    }
    _snprintf(out, max, "%s%s%s", dir, dir[0] ? "\\" : "", file);
    out[max - 1] = '\0';
}
