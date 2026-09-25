/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        Video backend (GDI window + DIB blit)
 * PROGRAMMERS:    WinDosDX Team
 *
 * A windowed top-level app window that owns a 32-bit top-down DIB section.
 * The core renders into an XRGB buffer; we blit it with StretchDIBits each
 * frame. Resize is handled by centring the DOS image in the client area
 * (letterbox), which is what DOSBox-style frontends do.
 */

#include "windos_internal.h"

static HWND     g_hwnd;
static HDC      g_dc;
static HBITMAP  g_bitmap;
static HBITMAP  g_oldbitmap;
static BITMAPINFO g_bmi;
static void    *g_bits;
static void    *g_frame;
static u32      g_frame_width, g_frame_height;
static BITMAPINFO g_frame_bmi;
static u32      g_width, g_height;
static int      g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(WINDOWPLACEMENT)};
static HMENU    g_saved_menu;

static const WCHAR *WD_VIDEO_CLASS = L"WinDosDX.WinDosVideo";

/*
 * Paint the last completed emulator frame during WM_PAINT.  The DOSBox
 * renderer calls WD_VideoPresent from its emulation loop, outside a normal
 * Win32 paint cycle.  Keeping the source frame here and invalidating the
 * window makes presentation reliable on ReactOS, where drawing directly to a
 * persistent window DC can otherwise be overwritten by the next background
 * paint.
 */
static void
WD_VideoPaint(HWND hwnd, HDC dc)
{
    RECT client;
    int dst_w, dst_h;
    int x = 0, y = 0;
    int draw_w, draw_h;

    if (!dc)
        return;

    GetClientRect(hwnd, &client);
    dst_w = client.right - client.left;
    dst_h = client.bottom - client.top;
    if (dst_w <= 0 || dst_h <= 0)
        return;

    FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (!g_frame || !g_frame_width || !g_frame_height)
        return;

    if (g_fullscreen)
    {
        double sx = (double)dst_w / g_frame_width;
        double sy = (double)dst_h / g_frame_height;
        double scale = sx < sy ? sx : sy;
        draw_w = (int)(g_frame_width * scale);
        draw_h = (int)(g_frame_height * scale);
        x = (dst_w - draw_w) / 2;
        y = (dst_h - draw_h) / 2;
    }
    else
    {
        draw_w = dst_w < (int)g_frame_width ? dst_w : (int)g_frame_width;
        draw_h = dst_h < (int)g_frame_height ? dst_h : (int)g_frame_height;
    }

    if (draw_w > 0 && draw_h > 0)
    {
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, draw_w, draw_h,
                      0, 0, (int)g_frame_width, (int)g_frame_height,
                      g_frame, &g_frame_bmi, DIB_RGB_COLORS, SRCCOPY);
    }
}

static LRESULT CALLBACK
WD_VideoWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    /* Give input first refusal so it can consume mouse/key messages. */
    if (WD_InputHandleMessage(hwnd, msg, wparam, lparam))
        return 0;

    switch (msg)
    {
        case WM_CREATE:
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            WD_VideoPaint(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            /* Recreate the DIB to match the new client size. */
            if (g_dc)
            {
                BITMAPINFO bi;
                void *bits;
                HBITMAP nb;
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (rc.right > 0 && rc.bottom > 0)
                {
                    ZeroMemory(&bi, sizeof(bi));
                    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                    bi.bmiHeader.biWidth = rc.right;
                    bi.bmiHeader.biHeight = -rc.bottom; /* top-down */
                    bi.bmiHeader.biPlanes = 1;
                    bi.bmiHeader.biBitCount = 32;
                    bi.bmiHeader.biCompression = BI_RGB;
                    nb = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
                    if (nb)
                    {
                        HDC ndc = GetDC(hwnd);
                        HGDIOBJ old = SelectObject(ndc, nb);
                        SelectObject(ndc, old);
                        if (g_bitmap) DeleteObject(g_bitmap);
                        g_bitmap = nb;
                        g_bits = bits;
                        ZeroMemory(&bi, sizeof(bi));
                        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                        bi.bmiHeader.biWidth = rc.right;
                        bi.bmiHeader.biHeight = -rc.bottom;
                        bi.bmiHeader.biPlanes = 1;
                        bi.bmiHeader.biBitCount = 32;
                        bi.bmiHeader.biCompression = BI_RGB;
                        g_bmi = bi;
                    }
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_CLOSE:
            WD_PlatformRequestQuit();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            WD_InputOnWindowDestroy();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int
WD_VideoEnsureWindow(HINSTANCE inst, const char *title, u32 width, u32 height)
{
    WNDCLASSEXW wc;
    RECT rc;

    if (g_hwnd)
        return 0;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WD_VideoWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WD_VIDEO_CLASS;
    RegisterClassExW(&wc);

    rc.left = 0; rc.top = 0;
    rc.right = (LONG)width;
    rc.bottom = (LONG)height;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(0, WD_VIDEO_CLASS, L"WinDosDX",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return -1;

    if (title)
        SetWindowTextA(g_hwnd, title);

    g_dc = GetDC(g_hwnd);

    /* Create an initial DIB matching the client area. */
    SendMessage(g_hwnd, WM_SIZE, 0, 0);
    return 0;
}

int WD_VideoInit(const char *title, u32 width, u32 height, u32 bpp)
{
    HINSTANCE inst = GetModuleHandle(NULL);

    (void)bpp; /* fixed 32bpp for now */
    if (g_hwnd)
        return 0;

    g_width = width ? width : 640;
    g_height = height ? height : 480;

    if (WD_VideoEnsureWindow(inst, title, g_width, g_height) != 0)
        return -1;

    ShowWindow(g_hwnd, SW_SHOW);
    /* The DOS window is the initial front end, so make it the guest input
     * target immediately.  Without this, Explorer or the LiveCD dialog can
     * retain focus and the DOS shell appears alive but ignores the keyboard. */
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    UpdateWindow(g_hwnd);
    return 0;
}

void WD_VideoShutdown(void)
{
    if (g_dc && g_hwnd)
    {
        if (g_oldbitmap)
        {
            SelectObject(g_dc, g_oldbitmap);
            g_oldbitmap = NULL;
        }
        ReleaseDC(g_hwnd, g_dc);
        g_dc = NULL;
    }
    if (g_bitmap)
    {
        DeleteObject(g_bitmap);
        g_bitmap = NULL;
    }
    if (g_hwnd)
    {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    g_bits = NULL;
    if (g_frame)
    {
        free(g_frame);
        g_frame = NULL;
    }
    g_frame_width = 0;
    g_frame_height = 0;
    ZeroMemory(&g_frame_bmi, sizeof(g_frame_bmi));
}

HWND WD_GetVideoWindow(void)
{
    return g_hwnd;
}

int WD_VideoResize(u32 width, u32 height)
{
    RECT rc;
    if (!g_hwnd)
        return -1;
    rc.left = 0; rc.top = 0;
    rc.right = (LONG)width;
    rc.bottom = (LONG)height;
    AdjustWindowRect(&rc, (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE), FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0,
                 rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    return 0;
}

static void
WD_VideoSavePlacement(void)
{
    g_prev_placement.length = sizeof(g_prev_placement);
    GetWindowPlacement(g_hwnd, &g_prev_placement);
    g_saved_menu = GetMenu(g_hwnd);
    SetMenu(g_hwnd, NULL);
}

static void
WD_VideoRestorePlacement(void)
{
    SetMenu(g_hwnd, g_saved_menu);
    SetWindowPlacement(g_hwnd, &g_prev_placement);
    SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}

int WD_VideoSetFullscreen(int fullscreen)
{
    if (!g_hwnd)
        return -1;
    if (!!g_fullscreen == !!fullscreen)
        return 0;

    if (fullscreen)
    {
        WD_VideoSavePlacement();
        SetWindowLongPtrW(g_hwnd, GWL_STYLE,
                          WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0,
                     GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN),
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 1;
    }
    else
    {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE,
                          WS_OVERLAPPEDWINDOW);
        WD_VideoRestorePlacement();
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 0;
    }
    return 0;
}

int WD_VideoIsFullscreen(void)
{
    return g_fullscreen;
}

void WD_VideoPresent(const void *pixels, u32 width, u32 height, u32 pitch)
{
    u32 row_bytes;
    u32 y;
    void *new_frame;

    if (!g_hwnd || !pixels || !width || !height)
        return;

    row_bytes = width * sizeof(u32);
    if (!g_frame || g_frame_width != width || g_frame_height != height)
    {
        new_frame = realloc(g_frame, row_bytes * height);
        if (!new_frame)
            return;
        g_frame = new_frame;
        g_frame_width = width;
        g_frame_height = height;
        ZeroMemory(&g_frame_bmi, sizeof(g_frame_bmi));
        g_frame_bmi.bmiHeader.biSize = sizeof(g_frame_bmi.bmiHeader);
        g_frame_bmi.bmiHeader.biWidth = (LONG)width;
        g_frame_bmi.bmiHeader.biHeight = -(LONG)height; /* top-down */
        g_frame_bmi.bmiHeader.biPlanes = 1;
        g_frame_bmi.bmiHeader.biBitCount = 32;
        g_frame_bmi.bmiHeader.biCompression = BI_RGB;
    }

    for (y = 0; y < height; ++y)
    {
        const BYTE *src = (const BYTE *)pixels + (size_t)y * pitch;
        BYTE *dst = (BYTE *)g_frame + (size_t)y * row_bytes;
        memcpy(dst, src, row_bytes);
    }

    /* Let WM_PAINT perform the actual GDI presentation. */
    InvalidateRect(g_hwnd, NULL, FALSE);
}

void WD_VideoSetTitle(const char *title)
{
    if (g_hwnd && title)
        SetWindowTextA(g_hwnd, title);
}

void WD_VideoGetClientSize(u32 *out_width, u32 *out_height)
{
    RECT rc;
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    if (!g_hwnd)
        return;
    GetClientRect(g_hwnd, &rc);
    if (out_width) *out_width = (u32)(rc.right - rc.left);
    if (out_height) *out_height = (u32)(rc.bottom - rc.top);
}
