/*
 *  Copyright (C) 2002-2010  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  WinDosDX replacement for SDL.h.
 *
 *  The DOSBox machine core only uses a very small part of SDL 1.2: a
 *  millisecond clock, a sleep, the audio device, mutexes, and CD drive
 *  enumeration for the "mount cdrom" shell command.  Rather than patch the
 *  vendored sources, this header provides exactly that surface and routes it
 *  through the WinDosDX windos_platform backend.
 *
 *  This is an internal implementation detail of windos_core; nothing outside
 *  the WinDosDX DOS engine links against it.
 */
#ifndef WINDOS_SDL_COMPAT_H
#define WINDOS_SDL_COMPAT_H

#include "windos_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SDL version reported to the core.  0.74-3 gates some keyboard LED
 * behaviour on this, and WinDosDX implements the newer behaviour. */
#define SDL_MAJOR_VERSION 1
#define SDL_MINOR_VERSION 2
#define SDL_PATCHLEVEL 14
#define SDL_VERSION_ATLEAST(X, Y, Z) \
    (SDL_MAJOR_VERSION > (X) || \
     (SDL_MAJOR_VERSION == (X) && SDL_MINOR_VERSION > (Y)) || \
     (SDL_MAJOR_VERSION == (X) && SDL_MINOR_VERSION == (Y) && SDL_PATCHLEVEL >= (Z)))

/* SDL 1.2 spells the SDL_bool/fixed-width types like this. */
typedef unsigned char Uint8;
typedef signed char   Sint8;
typedef unsigned short Uint16;
typedef signed short   Sint16;
typedef unsigned int   Uint32;
typedef signed int     Sint32;

#define SDL_FALSE 0
#define SDL_TRUE  1
typedef enum { SDL_FALSE_T = 0, SDL_TRUE_T = 1 } SDL_bool_T;

#define SDLCALL __cdecl

/* Audio sample format; only AUDIO_S16SYS is used by the mixer. */
#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define AUDIO_U16LSB 0x0010
#define AUDIO_S16LSB 0x8010
#define AUDIO_U16SYS AUDIO_U16LSB
#define AUDIO_S16SYS AUDIO_S16LSB

typedef void (SDLCALL *SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct SDL_AudioSpec {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

/* Opaque audio device handle; only used to pass a non-NULL value back. */
typedef struct SDL_AudioDeviceID_t *SDL_AudioDeviceID;

/* Opaque mutex handle used by the CD image reader.  Note this is the *type*,
 * matching SDL 1.2, so the declarations below take SDL_mutex *.
 */
typedef struct SDL_mutex_t *SDL_mutex;

/* Opaque CD drive handle.  WinDosDX never opens a physical drive, so this is
 * only ever a pointer that SDL_CDOpen() returns as NULL. */
typedef struct SDL_CD {
    int unused;
} SDL_CD;

/* CD audio status bits (as in SDL_cdrom.h). */
#define CD_TRAYEMPTY   0x00
#define CD_STOPPED     0x01
#define CD_PLAYING     0x02
#define CD_PAUSED      0x03
#define CD_ERROR       0x04
#define CD_UNKNOWN     0x08
#define CD_INDRIVE     (CD_PLAYING | CD_PAUSED | CD_UNKNOWN)

/* --- video -----------------------------------------------------------------
 * The video path does not go through SDL at all: the core calls the GFX_*
 * entry points declared in include/video.h, which are implemented against
 * WD_VideoPresent by windos_dosbox.cpp.  Nothing from SDL is needed here. */

/* --- timing ---------------------------------------------------------------- */
Uint32 SDLCALL SDL_GetTicks(void);
void   SDLCALL SDL_Delay(Uint32 ms);

/* --- errors ---------------------------------------------------------------- */
const char *SDLCALL SDL_GetError(void);

/* --- audio -----------------------------------------------------------------
 * Implemented by windos_dosbox.cpp on top of WD_SoundInit/WD_SoundShutdown.
 * The mixer is double-buffered, so the audio lock is a no-op: the mix thread
 * never runs concurrently with the emulated CPU in this model. */
int  SDLCALL SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDLCALL SDL_CloseAudio(void);
void SDLCALL SDL_PauseAudio(int pause_on);
void SDLCALL SDL_LockAudio(void);
void SDLCALL SDL_UnlockAudio(void);

/* --- threads / mutexes ----------------------------------------------------- */
SDL_mutex *SDLCALL SDL_CreateMutex(void);
void       SDLCALL SDL_DestroyMutex(SDL_mutex *mutex);
void       SDLCALL SDL_mutexP(SDL_mutex *mutex);
void       SDLCALL SDL_mutexV(SDL_mutex *mutex);

/* --- CD-ROM ----------------------------------------------------------------
 * WinDosDX serves CD images through DOSBox's own cdrom_image.cpp, so the
 * physical drive is always reported as absent.  These stubs keep the "mount
 * cdrom" shell command and MSCDEX's interface probing linkable. */
int  SDLCALL SDL_CDNumDrives(void);
const char *SDLCALL SDL_CDName(int drive);
SDL_CD *SDLCALL SDL_CDOpen(int drive);
int  SDLCALL SDL_CDClose(SDL_CD *cd);
int  SDLCALL SDL_CDStatus(SDL_CD *cd);

#ifdef __cplusplus
}
#endif

#endif /* WINDOS_SDL_COMPAT_H */
