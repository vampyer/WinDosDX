/*
 * WinDosDX - DOSBox machine core integration
 * Copyright (C) 2026 WinDosDX
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * WinDosDX-specific declarations shared between the Win32 platform layer and
 * the vendored DOSBox machine core.  Nothing outside windos_core includes
 * this header.
 */
#ifndef WINDOS_DOSBOX_H
#define WINDOS_DOSBOX_H

#include "dosbox.h"

/* Implemented in windos_dosbox.cpp against the windos_platform backend. */
void WD_DOSBOX_Init(void);
void WD_DOSBOX_Shutdown(void);

/* Implemented in windos_mixer.cpp, replacing the SDL audio backend. */
void WD_MIXER_Start(Bitu freq, Bitu blocksize, void (*callback)(void *, Bit8u *, int));
void WD_MIXER_Stop(void);

/* Implemented in windos_cdrom.cpp, replacing CDROM_Interface_SDL. */
class CDROM_Interface_WinDos;

/* Implemented in windos_serial.cpp, replacing the SDL_net serial backends. */
void SERIAL_Init(Section * sec);

#endif /* WINDOS_DOSBOX_H */
