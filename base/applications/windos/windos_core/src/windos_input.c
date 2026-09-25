/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        Input backend (keyboard + mouse -> portable events)
 * PROGRAMMERS:    WinDosDX Team
 *
 * Translates Win32 window messages into WD_Event records the DOS core
 * consumes. A small ring buffer decouples the message pump (window thread)
 * from the machine loop.
 */

#include "windos_internal.h"

#define WD_EVENT_QUEUE_SIZE 256

static WD_Event  g_queue[WD_EVENT_QUEUE_SIZE];
static int       g_q_head, g_q_tail;
static int       g_mouse_mode;     /* 0 = relative, 1 = absolute */
static int       g_mouse_x, g_mouse_y;
static int       g_have_position;
static int       g_focused;
static int       g_buttons[WD_MAX_MOUSE_BUTTONS];
static int       g_mouse_captured;

/* Append an event to the ring buffer, dropping the oldest on overflow. */
static void
WD_InputPush(const WD_Event *ev)
{
    int next = (g_q_head + 1) % WD_EVENT_QUEUE_SIZE;
    if (next == g_q_tail)
    {
        /* Full: drop the oldest event to make room. */
        g_q_tail = (g_q_tail + 1) % WD_EVENT_QUEUE_SIZE;
    }
    g_queue[g_q_head] = *ev;
    g_q_head = next;
}

int WD_InputInit(void)
{
    g_q_head = g_q_tail = 0;
    g_mouse_mode = 0;
    g_have_position = 0;
    g_focused = 0;
    ZeroMemory(g_buttons, sizeof(g_buttons));
    return 0;
}

void WD_InputShutdown(void)
{
    HWND hwnd = WD_GetVideoWindow();
    if (hwnd && g_mouse_captured)
    {
        ReleaseCapture();
        g_mouse_captured = 0;
    }
    g_q_head = g_q_tail = 0;
}

void WD_InputSetMouseMode(int mode)
{
    g_mouse_mode = mode ? 1 : 0;
    /* Release capture when going to relative mode. */
    if (g_mouse_mode == 0 && g_mouse_captured)
    {
        HWND hwnd = WD_GetVideoWindow();
        if (hwnd) ReleaseCapture();
        g_mouse_captured = 0;
    }
}

int WD_InputGetMouseMode(void)
{
    return g_mouse_mode;
}

void WD_InputSetFocus(int focused)
{
    g_focused = focused;
    if (!focused)
        WD_SoundPause(1);
    else
        WD_SoundPause(0);
}

void WD_InputSetLastPosition(int x, int y, int have_position)
{
    g_mouse_x = x;
    g_mouse_y = y;
    g_have_position = have_position;
}

int WD_InputGetPosition(int *x, int *y)
{
    if (x) *x = g_mouse_x;
    if (y) *y = g_mouse_y;
    return g_have_position;
}

void WD_InputOnWindowDestroy(void)
{
    g_mouse_captured = 0;
    g_have_position = 0;
}

/*
 * Translate a Win32 virtual key + scan into a DOS-friendly scancode. We use a
 * compact set-1-style table for the keys the core cares about; anything not
 * listed yields the raw scan code masked to 8 bits (with E0 prefix for
 * extended keys via the `extended` flag).
 */
static u32
WD_InputVkToScancode(WPARAM vk, LPARAM lparam, int *out_extended)
{
    int extended = 0;
    u32 scan;

    scan = (u32)((lparam >> 16) & 0xFF);

    /* Some ReactOS input paths deliver a valid virtual key but leave the
     * hardware scan-code field at zero.  Do not feed the raw VK value to the
     * BIOS in that case: DOSBox expects set-1 make codes, not ASCII/Windows
     * virtual-key values. */
    if (scan == 0)
    {
        if (vk >= 'A' && vk <= 'Z')
            scan = (u32)(0x1e + (vk - 'A'));
        else if (vk >= '0' && vk <= '9')
            scan = (u32)((vk == '0') ? 0x0b : 0x02 + (vk - '1'));
        else if (vk >= VK_F1 && vk <= VK_F12)
            scan = (u32)(0x3b + (vk - VK_F1));
        else
            switch (vk)
            {
            case VK_OEM_MINUS:  scan = 0x0c; break;
            case VK_OEM_PLUS:   scan = 0x0e; break;
            case VK_OEM_4:      scan = 0x0f; break; /* [ */
            case VK_OEM_6:      scan = 0x11; break; /* ] */
            case VK_OEM_1:      scan = 0x13; break; /* ; */
            case VK_OEM_7:      scan = 0x14; break; /* ' */
            case VK_OEM_5:      scan = 0x15; break; /* \ */
            case VK_OEM_COMMA:  scan = 0x33; break;
            case VK_OEM_PERIOD: scan = 0x34; break;
            case VK_OEM_2:      scan = 0x35; break; /* / */
            case VK_OEM_3:      scan = 0x16; break; /* ` */
            default: break;
            }
    }

    switch (vk)
    {
        case VK_LEFT:  extended = 1; scan = 0x4B; break;
        case VK_RIGHT: extended = 1; scan = 0x4D; break;
        case VK_UP:    extended = 1; scan = 0x48; break;
        case VK_DOWN:  extended = 1; scan = 0x50; break;
        case VK_HOME:  extended = 1; scan = 0x47; break;
        case VK_END:   extended = 1; scan = 0x4F; break;
        case VK_PRIOR: extended = 1; scan = 0x49; break;
        case VK_NEXT:  extended = 1; scan = 0x51; break;
        case VK_INSERT:extended = 1; scan = 0x52; break;
        case VK_DELETE:extended = 1; scan = 0x53; break;
        case VK_RETURN: scan = 0x1C; break;
        case VK_BACK:   scan = 0x0E; break;
        case VK_TAB:    scan = 0x0F; break;
        case VK_ESCAPE: scan = 0x01; break;
        case VK_SPACE:  scan = 0x39; break;
        case VK_LWIN:
        case VK_RWIN:   extended = 1; scan = 0x5B; break;
        default:
            if (scan == 0)
                scan = (u32)(vk & 0xFF);
            break;
    }

    if (out_extended)
        *out_extended = extended;
    return scan;
}

/*
 * Returns nonzero if the message was consumed as input.
 */
int WD_InputHandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            WD_Event ev;
            int ext = 0;
            ZeroMemory(&ev, sizeof(ev));
            ev.type = WD_EVENT_KEY_DOWN;
            ev.scancode = WD_InputVkToScancode(wparam, lparam, &ext);
            ev.extended = ext;
            WD_InputPush(&ev);
            return 1;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            WD_Event ev;
            int ext = 0;
            ZeroMemory(&ev, sizeof(ev));
            ev.type = WD_EVENT_KEY_UP;
            ev.scancode = WD_InputVkToScancode(wparam, lparam, &ext);
            ev.extended = ext;
            WD_InputPush(&ev);
            return 1;
        }

        case WM_MOUSEMOVE:
        {
            WD_Event ev;
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            if (g_mouse_mode == 0)
            {
                /* Relative mode: report the delta since the last position. */
                if (g_have_position)
                {
                    ev.x = x - g_mouse_x;
                    ev.y = y - g_mouse_y;
                }
                else
                {
                    ev.x = 0;
                    ev.y = 0;
                }
                /* Keep the real cursor on the client centre while captured. */
                g_mouse_x = x;
                g_mouse_y = y;
            }
            else
            {
                ev.x = x;
                ev.y = y;
            }
            g_have_position = 1;
            ev.type = WD_EVENT_MOUSE_MOVE;
            WD_InputPush(&ev);
            return 1;
        }

        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        {
            WD_Event ev;
            int btn = (msg == WM_LBUTTONDOWN) ? 0 :
                      (msg == WM_RBUTTONDOWN) ? 1 : 2;
            ZeroMemory(&ev, sizeof(ev));
            ev.type = WD_EVENT_MOUSE_DOWN;
            ev.button = btn;
            if (btn < WD_MAX_MOUSE_BUTTONS) g_buttons[btn] = 1;
            WD_InputPush(&ev);
            if (!g_mouse_captured) { SetCapture(hwnd); g_mouse_captured = 1; }
            return 1;
        }

        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        {
            WD_Event ev;
            int btn = (msg == WM_LBUTTONUP) ? 0 :
                      (msg == WM_RBUTTONUP) ? 1 : 2;
            ZeroMemory(&ev, sizeof(ev));
            ev.type = WD_EVENT_MOUSE_UP;
            ev.button = btn;
            if (btn < WD_MAX_MOUSE_BUTTONS) g_buttons[btn] = 0;
            WD_InputPush(&ev);
            if (g_buttons[0] == 0 && g_buttons[1] == 0 && g_buttons[2] == 0)
            {
                if (g_mouse_captured) { ReleaseCapture(); g_mouse_captured = 0; }
            }
            return 1;
        }

        case WM_MOUSEWHEEL:
        {
            WD_Event ev;
            ZeroMemory(&ev, sizeof(ev));
            ev.type = WD_EVENT_MOUSE_WHEEL;
            ev.wheel = (GET_WHEEL_DELTA_WPARAM(wparam) > 0)
                     ? WD_MOUSE_WHEEL_UP : WD_MOUSE_WHEEL_DOWN;
            WD_InputPush(&ev);
            return 1;
        }

        case WM_SETFOCUS:
            WD_InputSetFocus(1);
            return 0;
        case WM_KILLFOCUS:
            WD_InputSetFocus(0);
            return 0;

        case WM_CAPTURECHANGED:
            g_mouse_captured = 0;
            return 0;
    }

    return 0;
}

/*
 * Drain the Win32 message queue and append any input events to our ring.
 * Returns total events available (ring + newly pumped).
 */
int WD_InputPump(WD_Event *events, int max_events)
{
    MSG msg;
    HWND hwnd = WD_GetVideoWindow();

    /* Pump pending window messages into the ring. */
    if (hwnd)
    {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                WD_PlatformRequestQuit();
                break;
            }
            if (hwnd)
                TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    /* Copy ring -> caller. */
    {
        int n = 0;
        while (g_q_tail != g_q_head && n < max_events)
        {
            events[n++] = g_queue[g_q_tail];
            g_q_tail = (g_q_tail + 1) % WD_EVENT_QUEUE_SIZE;
        }
        return n;
    }
}
