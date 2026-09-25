/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        Shared internals for the WinDosDX platform backend
 * PROGRAMMERS:    WinDosDX Team
 *
 * windos_internal.h - declarations shared between the backend .c files.
 * These are NOT part of the public contract; the core only sees
 * windos_platform.h.
 */

#ifndef _WINDOS_INTERNAL_H_
#define _WINDOS_INTERNAL_H_

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../include/windos_platform.h"
#include "../include/windos_dos.h"

#define WD_MAX_MOUSE_BUTTONS 3

/* Global backend state, defined in windos_common.c. */
typedef struct
{
    int         ready;
    int         quit_requested;
    WD_DosState state;
} WD_GlobalState;

WD_GlobalState *WD_GetGlobal(void);

/* --- video-owned state the input backend reads (defined in windos_video.c) --- */
HWND WD_GetVideoWindow(void);

/* Called by the video backend when the window is destroyed. */
void WD_InputOnWindowDestroy(void);

/* --- input-owned state the video backend reads (defined in windos_input.c) --- */
int  WD_InputHandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
void WD_InputSetLastPosition(int x, int y, int have_position);
int  WD_InputGetPosition(int *x, int *y);

#endif /* _WINDOS_INTERNAL_H_ */
