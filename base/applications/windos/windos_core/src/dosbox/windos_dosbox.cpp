/*
 *  Copyright (C) 2002-2010  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * WinDosDX platform layer for the DOSBox machine core.
 *
 * Upstream DOSBox implements its frontend in src/gui/sdlmain.cpp: the GFX_*
 * video surface, the SDL event loop, SDL audio and the machine bring-up
 * sequence.  WinDosDX omits that file and supplies the same contract here on
 * top of the windos_platform backend, which is the only WinDosDX code the
 * rest of the tree sees:
 *
 *   GFX_*        -> WD_VideoInit / WD_VideoPresent / WD_VideoResize
 *   SDL_* shims  -> WD_TimerNowMs / WD_SoundInit / WD_InputPump
 *   GFX_Events   -> WD_InputPump, translated to BIOS INT 16h / INT 33h
 *   WD_Core*     -> DOSBOX_Init / control->Init() / DOSBOX_RunMachine
 *
 * Keeping this in one file mirrors sdlmain.cpp's role, so future DOSBox
 * updates can be diffed against upstream the usual way.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "dosbox.h"

#include "bios.h"
#include "control.h"
#include "cpu.h"
#include "keyboard.h"
#include "logging.h"
#include "mapper.h"
#include "mouse.h"
#include "programs.h"
#include "setup.h"
#include "support.h"
#include "timer.h"
#include "video.h"

#include "windos_platform.h"
#include "windos_platform_bridge.h"

/* ------------------------------------------------------------------------- */
/* State                                                                       */
/* ------------------------------------------------------------------------- */

static void WD_CoreTrace(const char *message)
{
    char path[MAX_PATH];
    HANDLE file;
    DWORD written;
    char line[512];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    {
        char *slash = strrchr(path, '\\');
        if (slash)
            slash[1] = 0;
        else
            path[0] = 0;
    }
    lstrcatA(path, "windos.log");
    file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(file, 0, NULL, FILE_END);
    _snprintf(line, sizeof(line) - 2, "WinDosDX: %s\\r\\n", message);
    WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    CloseHandle(file);
}

static struct
{
    Bit8u *surface;          /* 32-bit XRGB back buffer the renderer draws into */
    Bitu   pitch;            /* bytes per surface row */
    Bitu   width;
    Bitu   height;
    bool   surface_valid;
    bool   active;           /* mirrors upstream's sdl.active */
    bool   updating;

    GFX_CallBack_t callback;
    char title[128];
} gfx;

bool mouselocked = false;     /* declared extern by include/video.h */

static char g_wd_initial_command[1024];

static void WD_CoreSetInitialCommandImpl(const char *command)
{
    if (command && *command)
    {
        strncpy(g_wd_initial_command, command,
                sizeof(g_wd_initial_command) - 1);
        g_wd_initial_command[sizeof(g_wd_initial_command) - 1] = '\0';
    }
    else
    {
        g_wd_initial_command[0] = '\0';
    }
}

/* ------------------------------------------------------------------------- */
/* SDL compatibility layer                                                     */
/* ------------------------------------------------------------------------- */

Uint32 SDLCALL SDL_GetTicks(void)
{
    return (Uint32)WD_TimerNowMs();
}

void SDLCALL SDL_Delay(Uint32 ms)
{
    Sleep(ms);
}

const char *SDLCALL SDL_GetError(void)
{
    return "WinDosDX platform backend";
}

/*
 * Audio.  The WinDosDX sound backend owns the waveOut ring and its mix
 * thread, so SDL's audio lock collapses to nothing: DOSBox's mixer is driven
 * from the PIT tick handler on the same thread that pumps input, and the fill
 * callback is invoked by the sound backend's own thread.
 */
static SDL_AudioCallback mix_callback = NULL;
static void *mix_userdata = NULL;

static void WD_MixFill(void *ctx, s16 *buffer, u32 frames)
{
    (void)ctx;
    if (mix_callback)
        mix_callback(mix_userdata, (Uint8 *)buffer,
                     (int)(frames * 2 * (u32)sizeof(s16)));
}

int SDLCALL SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    if (!desired)
        return -1;

    mix_callback = desired->callback;
    mix_userdata = desired->userdata;

    if (WD_SoundInit((u32)desired->freq, desired->channels, WD_MixFill, NULL) != 0)
        return -1;

    if (obtained)
    {
        obtained->freq        = desired->freq;
        obtained->format      = desired->format;
        obtained->channels    = desired->channels;
        obtained->samples     = desired->samples;
        obtained->callback    = desired->callback;
        obtained->userdata    = desired->userdata;
        obtained->silence     = 0;
        obtained->size        = (Uint32)desired->samples *
                                (Uint32)desired->channels * (Uint32)sizeof(Sint16);
    }

    return 0;
}

void SDLCALL SDL_CloseAudio(void)
{
    WD_SoundShutdown();
    mix_callback = NULL;
    mix_userdata = NULL;
}

void SDLCALL SDL_PauseAudio(int pause_on)
{
    WD_SoundPause(pause_on);
}

void SDLCALL SDL_LockAudio(void)
{
    /* See above: no concurrent access to the mix state in this model. */
}

void SDLCALL SDL_UnlockAudio(void)
{
}

/* --- mutexes -------------------------------------------------------------
 * cdrom_image.cpp takes a mutex to serialise access to the image file.  A
 * Windows critical section preserves that intent for any future threading.
 */
SDL_mutex *SDLCALL SDL_CreateMutex(void)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (!cs)
        return NULL;
    InitializeCriticalSection(cs);
    return (SDL_mutex *)cs;
}

void SDLCALL SDL_DestroyMutex(SDL_mutex *mutex)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)mutex;
    if (!cs)
        return;
    DeleteCriticalSection(cs);
    free(cs);
}

void SDLCALL SDL_mutexP(SDL_mutex *mutex)
{
    if (mutex)
        EnterCriticalSection((CRITICAL_SECTION *)mutex);
}

void SDLCALL SDL_mutexV(SDL_mutex *mutex)
{
    if (mutex)
        LeaveCriticalSection((CRITICAL_SECTION *)mutex);
}

/* --- CD-ROM --------------------------------------------------------------
 * WinDosDX serves CD images through DOSBox's own cdrom_image.cpp, so there is
 * never a physical drive.  Reporting zero drives keeps MSCDEX and the
 * "mount cdrom" shell command working; image mounts take the code-0x01 path.
 */
int SDLCALL SDL_CDNumDrives(void)
{
    return 0;
}

const char *SDLCALL SDL_CDName(int drive)
{
    (void)drive;
    return NULL;
}

SDL_CD *SDLCALL SDL_CDOpen(int drive)
{
    (void)drive;
    return NULL;
}

int SDLCALL SDL_CDClose(SDL_CD *cd)
{
    (void)cd;
    return 0;
}

int SDLCALL SDL_CDStatus(SDL_CD *cd)
{
    (void)cd;
    return CD_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Logging                                                                     */
/* ------------------------------------------------------------------------- */

void GFX_ShowMsg(char const *format, ...)
{
    char buffer[2048];
    va_list args;

    va_start(args, format);
    _vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    buffer[sizeof(buffer) - 1] = '\0';
    va_end(args);

    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
    /* Do not silently lose a core initialization exception.  In the embedded
     * LiveCD there is no console attached to userinit, so surface the same
     * diagnostic that the standalone host would print. */
    MessageBoxA(NULL, buffer, "WinDosDX DOS core", MB_OK | MB_ICONERROR);
}

/* ------------------------------------------------------------------------- */
/* GFX_* video surface                                                         */
/* ------------------------------------------------------------------------- */

/*
 * DOSBox's renderer will happily emit 8-bit palettised output when a scaler
 * advertises GFX_CAN_8, which is cheaper to fill.  WinDosDX always presents
 * 32-bit XRGB, so 8-bit is refused and the renderer falls back to a 32-bit
 * scaler.  That keeps WD_VideoPresent a single straight blit with no palette
 * translation step in the hot path.
 */
Bitu GFX_GetBestMode(Bitu flags)
{
    flags &= ~(GFX_CAN_8 | GFX_LOVE_8 |
               GFX_CAN_15 | GFX_LOVE_15 |
               GFX_CAN_16 | GFX_LOVE_16 |
               GFX_RGBONLY | GFX_HARDWARE | GFX_CAN_RANDOM);
    flags |= GFX_CAN_32 | GFX_LOVE_32;
    return flags;
}

Bitu GFX_GetRGB(Bit8u red, Bit8u green, Bit8u blue)
{
    /* Matches WD_PIXEL_XRGB as consumed by WD_VideoPresent. */
    return (Bitu)(((Bit32u)red << 16) | ((Bit32u)green << 8) | (Bit32u)blue);
}

Bitu GFX_SetSize(Bitu width, Bitu height, Bitu flags, double scalex, double scaley,
                 GFX_CallBack_t callback)
{
    char trace[96];
    _snprintf(trace, sizeof(trace), "GFX_SetSize %ux%u", width, height);
    WD_CoreTrace(trace);
    (void)scalex;
    (void)scaley;

    /* An in-flight update would leave render.scale.outWrite dangling. */
    if (gfx.updating)
        GFX_EndUpdate(NULL);

    GFX_Stop();

    gfx.width  = width;
    gfx.height = height;
    gfx.pitch  = width * 4;
    gfx.surface = (Bit8u *)calloc(1, (size_t)gfx.pitch * height);
    if (!gfx.surface)
    {
        gfx.width = gfx.height = gfx.pitch = 0;
        return 0;
    }

    gfx.surface_valid = true;
    gfx.callback = callback;

    if (WD_VideoResize(width, height) != 0)
    {
        free(gfx.surface);
        gfx.surface = NULL;
        gfx.surface_valid = false;
        return 0;
    }

    GFX_Start();

    if (callback)
        callback(GFX_CallBackReset);

    return flags | GFX_CAN_32;
}

bool GFX_StartUpdate(Bit8u *&pixels, Bitu &pitch)
{
    /* The emulated machine can spend long stretches inside BIOS keyboard
     * waits, where DOSBOX's normal loop does not reach GFX_Events().  Pump
     * Win32 messages at frame start as well so the DOS window cannot become
     * permanently non-interactive while it is waiting for input. */
    GFX_Events();

    if (!gfx.surface_valid || !gfx.active || gfx.updating)
        return false;

    gfx.updating = true;
    pixels = gfx.surface;
    pitch  = gfx.pitch;
    return true;
}

void GFX_EndUpdate(const Bit16u *changedLines)
{
    (void)changedLines;

    if (!gfx.surface_valid)
    {
        gfx.updating = false;
        return;
    }

    WD_VideoPresent(gfx.surface, gfx.width, gfx.height, gfx.pitch);
    {
        static bool first_frame = true;
        if (first_frame)
        {
            first_frame = false;
            OutputDebugStringA("WinDosDX: first DOS frame presented\n");
        }
    }
    gfx.updating = false;
}

void GFX_SetPalette(Bitu start, Bitu count, GFX_PalEntry *entries)
{
    /*
     * The renderer is forced into 32-bit output by GFX_GetBestMode(), so it
     * never asks for a palettised surface.  The VGA palette is still applied
     * to the emulated hardware by vga.cpp; this hook only matters if a
     * palettised scaler is ever enabled, in which case the entries would be
     * uploaded here.
     */
    (void)start;
    (void)count;
    (void)entries;
}

void GFX_ResetScreen(void)
{
    if (gfx.surface_valid)
        memset(gfx.surface, 0, (size_t)gfx.pitch * gfx.height);
    if (gfx.callback)
        gfx.callback(GFX_CallBackRedraw);
}

void GFX_Stop(void)
{
    if (gfx.updating)
        GFX_EndUpdate(NULL);
    gfx.active = false;
}

void GFX_Start(void)
{
    /* Keep the platform trace useful even when the machine is launched
     * outside the QEMU wrapper. */
    OutputDebugStringA("WinDosDX: GFX_Start\n");
    gfx.active = true;
}

void GFX_SwitchFullScreen(void)
{
    WD_VideoSetFullscreen(!WD_VideoIsFullscreen());
    GFX_ResetScreen();
}

void GFX_GetSize(int &width, int &height, bool &fullscreen)
{
    width  = (int)gfx.width;
    height = (int)gfx.height;
    fullscreen = WD_VideoIsFullscreen() != 0;
}

void GFX_LosingFocus(void)
{
    SDL_PauseAudio(1);
    if (mouselocked)
        GFX_CaptureMouse();
}

void GFX_CaptureMouse(void)
{
    mouselocked = !mouselocked;
    WD_InputSetMouseMode(mouselocked ? 1 : 0);
}

void GFX_SetTitle(Bit32s cycles, Bits frameskip, bool paused)
{
    char buffer[192];

    if (cycles < 0)
    {
        strncpy(buffer, gfx.title, sizeof(buffer) - 1);
    }
    else
    {
        _snprintf(buffer, sizeof(buffer), "%s  %08d cycles%s%s",
                  gfx.title, (int)cycles,
                  paused ? "  [paused]" : "",
                  (frameskip > 0) ? "  [frameskip]" : "");
    }

    buffer[sizeof(buffer) - 1] = '\0';
    WD_VideoSetTitle(buffer);
}

bool GFX_SDLUsingWinDIB(void)
{
    return false;
}

/* ------------------------------------------------------------------------- */
/* Input: WD_Event -> BIOS INT 16h / INT 33h                                   */
/* ------------------------------------------------------------------------- */

/*
 * DOSBox's keyboard module is addressed by symbolic key rather than by
 * scancode, so the set-1 scancodes from the WinDosDX input backend are
 * translated here.  Indexed by scancode (0x00..0x7f).
 */
static const KBD_KEYS scancode_to_key[128] = {

    /* 0x00 */ KBD_NONE,       KBD_esc,       KBD_1,          KBD_2,
    /* 0x04 */ KBD_3,           KBD_4,         KBD_5,          KBD_6,
    /* 0x08 */ KBD_7,           KBD_8,         KBD_9,          KBD_0,
    /* 0x0c */ KBD_minus,      KBD_equals,    KBD_backspace,  KBD_tab,
    /* 0x10 */ KBD_q,           KBD_w,         KBD_e,          KBD_r,
    /* 0x14 */ KBD_t,           KBD_y,         KBD_u,          KBD_i,
    /* 0x18 */ KBD_o,           KBD_p,         KBD_leftbracket,KBD_rightbracket,
    /* 0x1c */ KBD_enter,      KBD_leftctrl,  KBD_a,          KBD_s,
    /* 0x20 */ KBD_d,           KBD_f,         KBD_g,          KBD_h,
    /* 0x24 */ KBD_j,           KBD_k,         KBD_l,          KBD_semicolon,
    /* 0x28 */ KBD_quote,      KBD_grave,     KBD_leftshift,  KBD_backslash,
    /* 0x2c */ KBD_z,           KBD_x,         KBD_c,          KBD_v,
    /* 0x30 */ KBD_b,           KBD_n,         KBD_m,          KBD_comma,
    /* 0x34 */ KBD_period,     KBD_slash,     KBD_rightshift, KBD_kpmultiply,
    /* 0x38 */ KBD_leftalt,     KBD_space,     KBD_capslock,   KBD_f1,
    /* 0x3c */ KBD_f2,          KBD_f3,        KBD_f4,         KBD_f5,
    /* 0x40 */ KBD_f6,          KBD_f7,        KBD_f8,         KBD_f9,
    /* 0x44 */ KBD_f10,         KBD_numlock,   KBD_scrolllock, KBD_kp7,
    /* 0x48 */ KBD_kp8,         KBD_kp9,       KBD_kpminus,    KBD_kp4,
    /* 0x4c */ KBD_kp5,         KBD_kp6,       KBD_kpplus,     KBD_kp1,
    /* 0x50 */ KBD_kp2,         KBD_kp3,       KBD_kp0,        KBD_kpperiod,
    /* 0x54 */ KBD_NONE,       KBD_NONE,      KBD_NONE,       KBD_NONE,
    /* 0x58 */ KBD_NONE,       KBD_NONE,      KBD_NONE,       KBD_NONE,
    /* 0x5c */ KBD_NONE,       KBD_NONE,      KBD_NONE,       KBD_NONE,
    /* 0x60 */ KBD_NONE,       KBD_NONE,      KBD_NONE,       KBD_NONE,
    /* 0x64 */ KBD_NONE,       KBD_home,      KBD_up,         KBD_pageup,
    /* 0x68 */ KBD_NONE,       KBD_left,      KBD_NONE,       KBD_right,
    /* 0x6c */ KBD_NONE,       KBD_end,       KBD_down,       KBD_pagedown,
    /* 0x70 */ KBD_NONE,       KBD_insert,    KBD_delete,     KBD_NONE,
    /* 0x74 */ KBD_NONE,       KBD_NONE,      KBD_NONE,       KBD_NONE,
    /* 0x78 */ KBD_NONE,       KBD_NONE,      KBD_NONE,       KBD_NONE,
    /* 0x7c */ KBD_NONE,       KBD_NONE,      KBD_NONE,       KBD_NONE
};

static void WD_TranslateScancode(Bit32u scancode, int extended, bool pressed)
{
    /* Break codes arrive with bit 7 set. */
    scancode &= 0x7f;

    /* The mapper needs the raw key plus the modifier state, before the
     * symbolic translation below. */
    if (extended)
    {
        switch (scancode)
        {
        case 0x1d:
            WD_MapperSetModifier(WD_MOD_RIGHT_CTRL, pressed);
            break;
        case 0x38:
            WD_MapperSetModifier(WD_MOD_RIGHT_ALT, pressed);
            break;
        default:
            break;
        }
    }
    else
    {
        switch (scancode)
        {
        case 0x1d: WD_MapperSetModifier(WD_MOD_LEFT_CTRL, pressed);   break;
        case 0x38: WD_MapperSetModifier(WD_MOD_LEFT_ALT, pressed);    break;
        case 0x2a: WD_MapperSetModifier(WD_MOD_LEFT_SHIFT, pressed);  break;
        case 0x36: WD_MapperSetModifier(WD_MOD_RIGHT_SHIFT, pressed); break;
        default: break;
        }
    }

    WD_MapperKeyEvent(scancode, extended, pressed);

    if (extended)
    {
        switch (scancode)
        {
        case 0x1d: KEYBOARD_AddKey(KBD_rightctrl, pressed); return;
        case 0x38: KEYBOARD_AddKey(KBD_rightalt,  pressed); return;
        case 0x4a: KEYBOARD_AddKey(KBD_kpdivide, pressed); return;
        case 0x6c: KEYBOARD_AddKey(KBD_kpenter,  pressed); return;
        default: break;
        }
    }

    if (scancode < 128)
    {
        KBD_KEYS key = scancode_to_key[scancode];
        if (key != KBD_NONE)
            KEYBOARD_AddKey(key, pressed);
    }
}

void GFX_Events(void)
{
    /* The platform timer is independent from DOSBox's emulated PIT.  Run
     * its host callbacks here, at the same point as WinDosDX input. */
    WD_TimerDispatch();

    WD_Event events[64];
    int count, i;

    count = WD_InputPump(events, (int)(sizeof(events) / sizeof(events[0])));

    for (i = 0; i < count; i++)
    {
        const WD_Event *ev = &events[i];

        switch (ev->type)
        {
        case WD_EVENT_KEY_DOWN:
            WD_TranslateScancode(ev->scancode, ev->extended, true);
            break;

        case WD_EVENT_KEY_UP:
            WD_TranslateScancode(ev->scancode, ev->extended, false);
            break;

        case WD_EVENT_MOUSE_MOVE:
            Mouse_CursorMoved((float)ev->x, (float)ev->y, 0.0f, 0.0f, true);
            break;

        case WD_EVENT_MOUSE_DOWN:
            Mouse_ButtonPressed((Bit8u)ev->button);
            break;

        case WD_EVENT_MOUSE_UP:
            Mouse_ButtonReleased((Bit8u)ev->button);
            break;

        case WD_EVENT_MOUSE_WHEEL:
            /*
             * The wheel has no DOS equivalent; DOSBox's own mouse module
             * already maps vertical motion to the PS/2 wheel packets, so
             * forward the notch as a vertical step.
             */
            Mouse_CursorMoved(0.0f, 0.0f, 0.0f,
                              (ev->wheel == WD_MOUSE_WHEEL_UP) ? -1.0f : 1.0f,
                              false);
            break;

        case WD_EVENT_QUIT:
            /* Same killswitch DOSBox's SDL frontend uses. */
            WD_PlatformRequestQuit();
            throw 0;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Machine lifecycle                                                           */
/* ------------------------------------------------------------------------- */

/*
 * The host (src/windos_host.c) is C, so the three entry points below keep C
 * linkage.  Their declarations live in windos_platform_bridge.h.
 */

/*
 * DOSBox's DOSBOX_RunMachine() is re-entered from inside the emulated machine
 * (callback.cpp, paging.cpp, iohandler.cpp) and never returns to its caller,
 * so the host's frame loop is not the place to pump it.  WD_CoreRun()
 * therefore hands control to DOSBOX and lets the emulated code drive, exactly
 * as sdlmain.cpp's main() does.
 */
extern "C" void WD_CoreSetInitialCommand(const char *command)
{
    WD_CoreSetInitialCommandImpl(command);
}

extern "C" int WD_CoreInit(const WD_MachineConfig *config)
{
    /*
     * CommandLine is concrete in DOSBox and is normally built from argv.
     * WinDosDX passes a synthetic command line so DOSBOX_RealInit()'s
     * -machine probe and the config parser see a well-formed object.
     */
    static char arg0[] = "windos";
    static char arg_machine[] = "-machine";
    static char arg_value[] = "svga_s3";
    static char * const argv[] = { arg0, arg_machine, arg_value };
    static CommandLine cmdline(3, argv);

    static Config config_object(&cmdline);
    control = &config_object;

    if (config && config->title)
    {
        strncpy(gfx.title, config->title, sizeof(gfx.title) - 1);
        gfx.title[sizeof(gfx.title) - 1] = '\0';
    }

    /*
     * DOSBox's own config-file search wants to read and write dosbox.conf in
     * per-user directories. WinDosDX owns configuration through windos.ini,
     * so no DOSBox config file is parsed; the host's WD_MachineLoadConfig()
     * has already applied the user's settings to the backend.
     */
    DOSBOX_Init();

    /* Apply the host configuration through DOSBox's own property system so
     * the values are validated by the same code that ships with the core. */
    if (config)
    {
        char line[64];

        if (config->memory_kb)
        {
            _snprintf(line, sizeof(line), "memsize=%u",
                      (unsigned)(config->memory_kb / 1024u));
            control->GetSection("dosbox")->HandleInputline(line);
        }
        if (config->cycles)
        {
            _snprintf(line, sizeof(line), "cycles=fixed %u", config->cycles);
            control->GetSection("cpu")->HandleInputline(line);
        }
        if (config->mute)
            control->GetSection("mixer")->HandleInputline("nosound=true");
    }

    GFX_Start();

    /* Install the WinDosDX C: mapping as a DOSBox autoexec MOUNT line.
     * The filesystem backend remains the only host-path boundary; the
     * DOSBox localDrive consumes the resulting mounted directory. */
    const char *c_mount = WD_FSGetMount('C');
    if (c_mount && *c_mount)
    {
        char mount_line[1024];
        _snprintf(mount_line, sizeof(mount_line),
                  "MOUNT C \\\"%s\\\"", c_mount);
        control->GetSection("autoexec")->HandleInputline(mount_line);
        OutputDebugStringA("WinDosDX: C: mount configured\n");
    }

    if (g_wd_initial_command[0])
    {
        control->GetSection("autoexec")->HandleInputline(g_wd_initial_command);
        WD_CoreTrace("initial DOS command installed");
    }

    OutputDebugStringA("WinDosDX: DOS core init complete\n");
    return 0;
}

extern "C" int WD_CoreRun(void)
{
    DOSBOX_SetNormalLoop();

    try
    {
        control->Init();
        WD_CoreTrace("control->Init complete");
        MAPPER_Init();
        WD_CoreTrace("mapper init complete");
        control->StartUp();
        WD_CoreTrace("DOS core startup returned");

        /* Config::StartUp() only initializes the DOS shell and returns.  The
         * emulated machine is driven by DOSBOX_RunMachine(); without this
         * call WD_CoreRun() returned immediately, userinit tore down the
         * video window, and the guest showed a blank/frozen DOS front end. */
        DOSBOX_RunMachine();
        WD_CoreTrace("DOS core machine loop returned");
    }
    catch (int)
    {
        /* Clean exit: the killswitch was pulled. */
    }
    catch (char *error)
    {
        GFX_ShowMsg("WinDosDX: %s", error ? error : "unknown error");
    }
    catch (...)
    {
        GFX_ShowMsg("WinDosDX: unknown fatal error in DOS machine core");
    }

    return 0;
}

void WD_CorePresentFrame(void)
{
    /*
     * Frames are presented by GFX_EndUpdate() as the emulated VGA finishes
     * each one. There is nothing to do from the host's idle path.
     */
}

extern "C" void WD_CoreShutdown(void)
{
    GFX_Stop();

    if (gfx.callback)
    {
        gfx.callback(GFX_CallBackStop);
        gfx.callback = NULL;
    }

    if (gfx.surface)
    {
        free(gfx.surface);
        gfx.surface = NULL;
    }
    gfx.surface_valid = false;
    gfx.width = gfx.height = gfx.pitch = 0;

    SDL_CloseAudio();
}
