/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        WinDosDX platform backend interface (Win32)
 * PROGRAMMERS:    WinDosDX Team
 *
 * windos_platform.h - the platform contract that the DOSBox-derived core
 * binds to. The core (once dropped in under windos_core/dosbox) calls these
 * functions instead of SDL. Each subsystem is a separate .c that a build can
 * select or stub out (e.g. headless build with video=no sound=no).
 *
 * The whole surface is plain C with fixed-width types and no C++ types so it
 * can be consumed by both the C++ DOSBox core and the C host (windos.exe).
 */

#ifndef _WINDOS_PLATFORM_H_
#define _WINDOS_PLATFORM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Common                                                                      */
/* ------------------------------------------------------------------------- */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* Non-zero once the DOS subsystem is initialized. */
int  WD_PlatformReady(void);

/* ------------------------------------------------------------------------- */
/* Video                                                                       */
/* ------------------------------------------------------------------------- */

/* Pixel format of the framebuffer the core renders into. We use a packed
 * 32-bit XRGB surface; the video backend converts to whatever GDI wants. */
#define WD_PIXEL_XRGB 0

typedef struct
{
    u32 width;
    u32 height;
    u32 bpp;          /* must be 32 for now */
    u32 pixel_format; /* WD_PIXEL_XRGB */
} WD_VideoMode;

/*
 * Create the output window at the given mode. On success the core may render
 * into the buffer passed to WD_VideoPresent.
 */
int  WD_VideoInit(const char *title, u32 width, u32 height, u32 bpp);
void WD_VideoShutdown(void);

/* Resize the output window (windowed mode). Returns 0 on success. */
int  WD_VideoResize(u32 width, u32 height);

/* Toggle windowed <-> fullscreen. Returns 0 on success. */
int  WD_VideoSetFullscreen(int fullscreen);
int  WD_VideoIsFullscreen(void);

/*
 * Blit a frame. `pixels` points to width*height 32-bit XRGB pixels, `pitch`
 * is the number of bytes per source row. This is the hot path called once per
 * emulated frame.
 */
void WD_VideoPresent(const void *pixels, u32 width, u32 height, u32 pitch);

/* Optional: change the window title (e.g. to show the running program). */
void WD_VideoSetTitle(const char *title);

/* Return the window's client area, for letterboxing calculations. */
void WD_VideoGetClientSize(u32 *out_width, u32 *out_height);

/* ------------------------------------------------------------------------- */
/* Input                                                                       */
/* ------------------------------------------------------------------------- */

/* Scroll wheel / direction constants. */
#define WD_MOUSE_WHEEL_UP   1
#define WD_MOUSE_WHEEL_DOWN 2

typedef enum
{
    WD_EVENT_NONE = 0,
    WD_EVENT_KEY_DOWN,
    WD_EVENT_KEY_UP,
    WD_EVENT_MOUSE_MOVE,
    WD_EVENT_MOUSE_DOWN,
    WD_EVENT_MOUSE_UP,
    WD_EVENT_MOUSE_WHEEL,
    WD_EVENT_QUIT
} WD_EventType;

typedef struct
{
    WD_EventType type;
    /* key events */
    u32 scancode;   /* hardware set-1-ish scancode the core understands */
    int  extended;  /* nonzero if this was an extended key */
    /* mouse events */
    int  x, y;      /* relative or absolute, per WD_InputSetMouseMode */
    int  button;    /* 0=left 1=right 2=middle */
    int  wheel;     /* WD_MOUSE_WHEEL_* */
} WD_Event;

/*
 * Windows-message pump. The video backend owns the window, so input reads
 * messages from it. Call once per emulated frame; fills up to max_events
 * events and returns how many were written.
 */
int  WD_InputInit(void);
void WD_InputShutdown(void);
int  WD_InputPump(WD_Event *events, int max_events);

/* Mouse mode: 0 = relative (DOS default), 1 = absolute/windowed-captured. */
void WD_InputSetMouseMode(int mode);
int  WD_InputGetMouseMode(void);

/* Request async keyboard layout changes (scan-code -> VK translation). */
void WD_InputSetFocus(int focused);

/* ------------------------------------------------------------------------- */
/* Timer                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The core emulates the PIT itself; the timer backend only has to provide a
 * steady wall-clock "milliseconds since start" and a periodic tick callback so
 * the core can pace frames and drive its event loop. Frequency is the classic
 * 18.2 Hz PIT rate; the core may ignore it and use WD_TimerNowMs instead.
 */
#define WD_PIT_HZ 18

int  WD_TimerInit(u32 pit_hz);
void WD_TimerShutdown(void);

/* Milliseconds since WD_TimerInit (monotonic). */
u64  WD_TimerNowMs(void);

/* Microseconds since WD_TimerInit (monotonic), for cycle pacing. */
u64  WD_TimerNowUs(void);

/* Register a callback invoked every `interval_ms`. Returns a handle >= 0. */
int  WD_TimerAddCallback(u32 interval_ms, void (*cb)(void *ctx), void *ctx);
void WD_TimerRemoveCallback(int handle);

/* ------------------------------------------------------------------------- */
/* Sound                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The core mixes audio into a 16-bit signed stereo buffer at `rate` Hz. The
 * sound backend consumes it via waveOut. Callbacks may run on a separate
 * thread; keep mixing lock-free and bounded.
 */
typedef void (*WD_SoundFillFn)(void *ctx, s16 *buffer, u32 frames);

int  WD_SoundInit(u32 rate, u16 channels, WD_SoundFillFn fill, void *fill_ctx);
void WD_SoundShutdown(void);

/* Master volume 0..100. Returns 0 on success. */
int  WD_SoundSetVolume(int volume);
int  WD_SoundGetVolume(void);

/* Pause/resume the output device (e.g. when the window loses focus). */
void WD_SoundPause(int paused);

/* ------------------------------------------------------------------------- */
/* Filesystem                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * DOSBox mounts host directories as DOS drives. The fs backend resolves a DOS
 * path (e.g. "C:\\DOOM") against a host mount table. The core calls these to
 * perform real I/O through the Win32 CRT, so no POSIX assumptions leak into
 * the core.
 */

/* Resolve a host path for a DOS drive letter, e.g. mount 'C' -> "D:\\Games".
 * Returns 0 on success. */
int  WD_FSAddMount(char drive_letter, const char *host_path);

/* Look up the host path for a DOS drive letter. Returns NULL if unmounted. */
const char *WD_FSGetMount(char drive_letter);

/* Case-insensitive DOS-style path normalisation helper (8.3-agnostic). */
void WD_FSNormalizePath(char *inout_path, size_t max_len);

/* File I/O passthrough used by DOSBox's localDrive. */
void *WD_FSOpen(const char *path, const char *mode);
int   WD_FSClose(void *fp);
long  WD_FSSize(void *fp);
long  WD_FSSeek(void *fp, long offset, int whence);
size_t WD_FSRead(void *fp, void *buf, size_t size);
size_t WD_FSWrite(void *fp, const void *buf, size_t size);
int   WD_FSFlush(void *fp);

/* Directory listing helper: returns the number of entries copied, or <0. */
int   WD_FSReadDir(const char *path, char *out_names, int max_entries,
                   size_t name_max, int want_dirs);

/* ------------------------------------------------------------------------- */
/* Machine lifecycle (implemented by the host, declared for the core)          */
/* ------------------------------------------------------------------------- */

/* Configuration the host passes to the core at startup (parsed from
 * windos.conf / windos.ini). Kept minimal; extend as the core lands. */
typedef struct
{
    const char *title;      /* window title */
    u32  width, height;     /* initial mode */
    u32  cycles;            /* emulated CPU cycles, 0 = auto */
    u32  memory_kb;         /* conventional memory, 0 = auto */
    int  fullscreen;        /* start fullscreen */
    int  mute;              /* start muted */
} WD_MachineConfig;

/* Boot / shutdown the emulated machine. Return 0 on success. Provided by the
 * host (windos.exe); called by the core once it is wired up. */
int  WD_MachineInit(const WD_MachineConfig *config);
void WD_MachineShutdown(void);

/* Run the machine until it exits or WD_PlatformRequestQuit is called. */
int  WD_MachineRun(void);

/* Ask the machine loop to exit (thread-safe; callable from input/quit). */
void WD_PlatformRequestQuit(void);

/* Load an optional config file (host path). Returns 0 if none/absent. */
int  WD_MachineLoadConfig(const char *host_path, WD_MachineConfig *out_cfg);

#ifdef __cplusplus
}
#endif

#endif /* _WINDOS_PLATFORM_H_ */
