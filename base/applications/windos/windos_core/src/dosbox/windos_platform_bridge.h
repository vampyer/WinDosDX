/*
 * WinDosDX - DOSBox machine core platform bridge
 * Copyright (C) 2026 WinDosDX
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Declarations shared inside the WinDosDX DOS engine only.  The rest of
 * WinDosDX sees nothing but the C windos_platform.h contract.
 */
#ifndef WINDOS_PLATFORM_BRIDGE_H
#define WINDOS_PLATFORM_BRIDGE_H

#include "dosbox.h"

/* Modifier keys tracked by the mapper for MMOD1 / MMOD2 matching. */
enum WD_Modifier
{
    WD_MOD_LEFT_ALT,
    WD_MOD_RIGHT_ALT,
    WD_MOD_LEFT_CTRL,
    WD_MOD_RIGHT_CTRL,
    WD_MOD_LEFT_SHIFT,
    WD_MOD_RIGHT_SHIFT
};

/* Implemented in windos_mapper.cpp. */
void WD_MapperSetModifier(int which, bool pressed);
void WD_MapperKeyEvent(Bit32u scancode, int extended, bool pressed);

/*
 * Machine lifecycle, implemented in windos_dosbox.cpp.  These have C linkage
 * because src/windos_host.c is C.
 */
extern "C" {
int  WD_CoreInit(const WD_MachineConfig *config);
int  WD_CoreRun(void);
void WD_CoreShutdown(void);
void WD_CoreSetInitialCommand(const char *command);
void WD_TimerDispatch(void);
}

#endif /* WINDOS_PLATFORM_BRIDGE_H */
