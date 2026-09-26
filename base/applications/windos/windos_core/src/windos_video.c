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

/* Display options (windos.ini [Display], also in the window's system menu). */
static u32      g_scale = 2;        /* window size: 1-4x the DOS resolution */
static int      g_aspect = 1;       /* show DOS modes at 4:3 */
static int      g_smooth = 0;       /* smooth instead of sharp pixels */
static u32      g_mode_width, g_mode_height; /* current DOS video mode */
static char     g_title[256];

/* System menu commands: below 0xF000 with the low four bits clear. */
#define WD_CMD_SCALE1      0x1010
#define WD_CMD_SCALE2      0x1020
#define WD_CMD_SCALE3      0x1030
#define WD_CMD_SCALE4      0x1040
#define WD_CMD_ASPECT      0x1050
#define WD_CMD_SMOOTH      0x1060
#define WD_CMD_FULLSCREEN  0x1070
#define WD_CMD_SCREENSHOT  0x1080
#define WD_CMD_SETTINGS    0x1090
#define WD_TITLE_TIMER     1

static const WCHAR *WD_VIDEO_CLASS = L"WinDosDX.WinDosVideo";

/*
 * Paint the last completed emulator frame during WM_PAINT.  The DOSBox
 * renderer calls WD_VideoPresent from its emulation loop, outside a normal
 * Win32 paint cycle.  Keeping the source frame here and invalidating the
 * window makes presentation reliable on ReactOS, where drawing directly to a
 * persistent window DC can otherwise be overwritten by the next background
 * paint.
 */
/* The shape a DOS mode is shown at: 4:3 when aspect correction is on. */
static void
WD_VideoDisplaySize(u32 width, u32 height, u32 *out_w, u32 *out_h)
{
    *out_w = width ? width : 640;
    *out_h = height ? height : 400;
    if (g_aspect && width)
        *out_h = width * 3 / 4;
}

/* Size the window for the current DOS mode at the chosen scale. */
static void
WD_VideoFitWindow(void)
{
    RECT rc;
    MONITORINFO monitor;
    u32 w, h, scale;
    LONG work_w = LONG_MAX, work_h = LONG_MAX;

    if (!g_hwnd || g_fullscreen || IsZoomed(g_hwnd) || !g_mode_width)
        return;
    WD_VideoDisplaySize(g_mode_width, g_mode_height, &w, &h);

    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        work_w = monitor.rcWork.right - monitor.rcWork.left;
        work_h = monitor.rcWork.bottom - monitor.rcWork.top;
    }

    /* The chosen scale, or the largest smaller one that fits the screen. */
    for (scale = g_scale; ; scale--)
    {
        rc.left = 0; rc.top = 0;
        rc.right = (LONG)(w * scale);
        rc.bottom = (LONG)(h * scale);
        AdjustWindowRect(&rc, (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE), FALSE);
        if (scale == 1 || (rc.right - rc.left <= work_w && rc.bottom - rc.top <= work_h))
            break;
    }

    /* Keep the whole window on the screen. */
    {
        RECT now;
        int x, y;
        GetWindowRect(g_hwnd, &now);
        x = now.left;
        y = now.top;
        if (work_w != LONG_MAX)
        {
            if (x + (rc.right - rc.left) > monitor.rcWork.right)
                x = monitor.rcWork.right - (rc.right - rc.left);
            if (y + (rc.bottom - rc.top) > monitor.rcWork.bottom)
                y = monitor.rcWork.bottom - (rc.bottom - rc.top);
            if (x < monitor.rcWork.left) x = monitor.rcWork.left;
            if (y < monitor.rcWork.top) y = monitor.rcWork.top;
        }
        SetWindowPos(g_hwnd, NULL, x, y, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER);
    }
}

static void
WD_VideoUpdateMenu(void)
{
    HMENU sys = g_hwnd ? GetSystemMenu(g_hwnd, FALSE) : NULL;
    UINT i;
    if (!sys)
        return;
    for (i = 1; i <= 4; i++)
        CheckMenuItem(sys, WD_CMD_SCALE1 + (i - 1) * 0x10,
                      MF_BYCOMMAND | (g_scale == i ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(sys, WD_CMD_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(sys, WD_CMD_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(sys, WD_CMD_FULLSCREEN, MF_BYCOMMAND | (g_fullscreen ? MF_CHECKED : MF_UNCHECKED));
}

/* Display commands live in the system menu (window icon, Alt+Space), which
   leaves every key to the DOS program. */
static void
WD_VideoBuildMenu(void)
{
    HMENU sys = GetSystemMenu(g_hwnd, FALSE);
    HMENU size = CreatePopupMenu();
    if (!sys || !size)
        return;
    AppendMenuW(size, MF_STRING, WD_CMD_SCALE1, L"&1x");
    AppendMenuW(size, MF_STRING, WD_CMD_SCALE2, L"&2x");
    AppendMenuW(size, MF_STRING, WD_CMD_SCALE3, L"&3x");
    AppendMenuW(size, MF_STRING, WD_CMD_SCALE4, L"&4x");
    AppendMenuW(sys, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sys, MF_POPUP, (UINT_PTR)size, L"&Window size");
    AppendMenuW(sys, MF_STRING, WD_CMD_ASPECT, L"Correct &aspect ratio (4:3)");
    AppendMenuW(sys, MF_STRING, WD_CMD_SMOOTH, L"Smoot&h scaling");
    AppendMenuW(sys, MF_STRING, WD_CMD_FULLSCREEN, L"&Full screen\tAlt+Enter");
    AppendMenuW(sys, MF_STRING, WD_CMD_SCREENSHOT, L"Save scree&nshot\tCtrl+F5");
    AppendMenuW(sys, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sys, MF_STRING, WD_CMD_SETTINGS, L"S&ettings...");
    WD_VideoUpdateMenu();
}

/* Show a note in the title bar for a few seconds. */
static void
WD_VideoFlashTitle(const char *note)
{
    char text[320];
    if (!g_hwnd)
        return;
    _snprintf(text, sizeof(text), "%s - %s", g_title, note);
    text[sizeof(text) - 1] = '\0';
    SetWindowTextA(g_hwnd, text);
    SetTimer(g_hwnd, WD_TITLE_TIMER, 3000, NULL);
}

static int
WD_VideoCommand(UINT id)
{
    switch (id)
    {
        case WD_CMD_SCALE1: case WD_CMD_SCALE2: case WD_CMD_SCALE3: case WD_CMD_SCALE4:
            g_scale = (id - WD_CMD_SCALE1) / 0x10 + 1;
            WD_ConfigSaveInt("Display", "scale", (int)g_scale);
            if (IsZoomed(g_hwnd))
                ShowWindow(g_hwnd, SW_RESTORE);
            WD_VideoFitWindow();
            break;
        case WD_CMD_ASPECT:
            g_aspect = !g_aspect;
            WD_ConfigSaveInt("Display", "aspect", g_aspect);
            WD_VideoFitWindow();
            break;
        case WD_CMD_SMOOTH:
            g_smooth = !g_smooth;
            WD_ConfigSaveInt("Display", "smooth", g_smooth);
            break;
        case WD_CMD_FULLSCREEN:
            WD_VideoSetFullscreen(!g_fullscreen);
            break;
        case WD_CMD_SCREENSHOT:
            WD_VideoScreenshot();
            break;
        case WD_CMD_SETTINGS:
            WD_SettingsShow(g_hwnd);
            break;
        default:
            return 0;
    }
    WD_VideoUpdateMenu();
    InvalidateRect(g_hwnd, NULL, FALSE);
    return 1;
}

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

    /* Fit the displayed shape (4:3 with aspect correction) into the client
       area, centred, with black bars. */
    {
        u32 shape_w, shape_h;
        WD_VideoDisplaySize(g_frame_width, g_frame_height, &shape_w, &shape_h);
        if ((double)dst_w / shape_w < (double)dst_h / shape_h)
        {
            draw_w = dst_w;
            draw_h = (int)((double)dst_w * shape_h / shape_w + 0.5);
        }
        else
        {
            draw_h = dst_h;
            draw_w = (int)((double)dst_h * shape_w / shape_h + 0.5);
        }
        x = (dst_w - draw_w) / 2;
        y = (dst_h - draw_h) / 2;
    }

    if (draw_w > 0 && draw_h > 0)
    {
        if (g_smooth)
        {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
        }
        else
        {
            SetStretchBltMode(dc, COLORONCOLOR);
        }
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

        case WM_SYSCOMMAND:
            if (WD_VideoCommand((UINT)(wparam & 0xFFF0)))
                return 0;
            break;

        case WM_TIMER:
            if (wparam == WD_TITLE_TIMER)
            {
                KillTimer(hwnd, WD_TITLE_TIMER);
                SetWindowTextA(hwnd, g_title);
                return 0;
            }
            break;

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
    {
        lstrcpynA(g_title, title, sizeof(g_title));
        SetWindowTextA(g_hwnd, title);
    }

    g_dc = GetDC(g_hwnd);
    WD_VideoBuildMenu();

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

/* A DOS video mode change: the window follows at the chosen scale. */
int WD_VideoResize(u32 width, u32 height)
{
    if (!g_hwnd)
        return -1;
    if (width != g_mode_width || height != g_mode_height)
    {
        g_mode_width = width;
        g_mode_height = height;
        WD_VideoFitWindow();
    }
    return 0;
}

void WD_VideoGetOptions(u32 *scale, int *aspect, int *smooth)
{
    *scale = g_scale;
    *aspect = g_aspect;
    *smooth = g_smooth;
}

void WD_VideoConfigure(u32 scale, int aspect, int smooth)
{
    g_scale = (scale >= 1 && scale <= 4) ? scale : 2;
    g_aspect = aspect ? 1 : 0;
    g_smooth = smooth ? 1 : 0;
    if (g_hwnd)
    {
        WD_VideoFitWindow();
        WD_VideoUpdateMenu();
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/*
 * Save the current DOS screen, as the emulator drew it, as a 24-bit .bmp in
 * Pictures\WinDosDX (My Documents\My Pictures on older profiles).
 */
int WD_VideoScreenshot(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    BITMAPFILEHEADER file_header;
    BITMAPINFOHEADER info;
    SYSTEMTIME now;
    HANDLE file;
    DWORD written;
    u32 row_bytes, y, x;
    BYTE *row;
    DWORD n;

    if (!g_frame || !g_frame_width || !g_frame_height)
        return -1;

    n = GetEnvironmentVariableA("USERPROFILE", dir, MAX_PATH - 40);
    if (n == 0 || n >= MAX_PATH - 40)
        return -1;
    lstrcatA(dir, "\\Pictures");
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES)
    {
        dir[n] = '\0';
        lstrcatA(dir, "\\My Documents\\My Pictures");
        CreateDirectoryA(dir, NULL);
    }
    lstrcatA(dir, "\\WinDosDX");
    CreateDirectoryA(dir, NULL);

    GetLocalTime(&now);
    _snprintf(path, sizeof(path), "%s\\dos-%04u%02u%02u-%02u%02u%02u.bmp", dir,
              now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    path[sizeof(path) - 1] = '\0';

    row_bytes = (g_frame_width * 3 + 3) & ~3u;
    ZeroMemory(&info, sizeof(info));
    info.biSize = sizeof(info);
    info.biWidth = (LONG)g_frame_width;
    info.biHeight = (LONG)g_frame_height;   /* bottom-up */
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = row_bytes * g_frame_height;
    ZeroMemory(&file_header, sizeof(file_header));
    file_header.bfType = 0x4D42;            /* "BM" */
    file_header.bfOffBits = sizeof(file_header) + sizeof(info);
    file_header.bfSize = file_header.bfOffBits + info.biSizeImage;

    row = (BYTE *)calloc(1, row_bytes);
    if (!row)
        return -1;
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        free(row);
        return -1;
    }
    WriteFile(file, &file_header, sizeof(file_header), &written, NULL);
    WriteFile(file, &info, sizeof(info), &written, NULL);
    for (y = g_frame_height; y-- > 0; )
    {
        const u32 *src = (const u32 *)g_frame + (size_t)y * g_frame_width;
        for (x = 0; x < g_frame_width; x++)
        {
            row[x * 3 + 0] = (BYTE)(src[x]);
            row[x * 3 + 1] = (BYTE)(src[x] >> 8);
            row[x * 3 + 2] = (BYTE)(src[x] >> 16);
        }
        WriteFile(file, row, row_bytes, &written, NULL);
    }
    CloseHandle(file);
    free(row);

    WD_VideoFlashTitle("screenshot saved in Pictures\\WinDosDX");
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
        WD_VideoFitWindow();
    }
    WD_VideoUpdateMenu();
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
    if (title)
        lstrcpynA(g_title, title, sizeof(g_title));
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
