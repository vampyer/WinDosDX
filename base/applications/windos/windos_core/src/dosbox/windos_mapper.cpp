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
 * WinDosDX event mapper.
 *
 * Upstream DOSBox implements the mapper in src/gui/sdl_mapper.cpp, which
 * loads and saves a mapper file, lays out an interactive key-mapping dialog
 * and translates SDL key events.  None of that is available to WinDosDX, so
 * this implementation keeps only the part the emulated machine actually
 * needs: the registry of mapped events, and the translation of a host key
 * press into "the event bound to that key fired".
 *
 * Bindings are fixed at the DOSBox defaults below.  A future WinDosDX release
 * can add a config-file format and a settings UI on top of this without the
 * core changing.
 */

#include <string.h>
#include <vector>

#include "dosbox.h"
#include "mapper.h"
#include "setup.h"

#include "windos_platform.h"
#include "windos_platform_bridge.h"

/* --- registry ------------------------------------------------------------ */

struct MappedEvent
{
    MAPPER_Handler *handler;
    MapKeys         key;        /* the MapKeys value the core asked for */
    Bitu            mods;       /* MMOD1 / MMOD2 modifier requirement */
    const char     *eventname;  /* human-readable name, for diagnostics */
    const char     *buttonname; /* unique key, for de-duplication */
};

static std::vector<MappedEvent> g_events;

/*
 * Default bindings, in host set-1 scancode terms.  The core registers each
 * mapped action with a MapKeys/MMOD pair; the scancode below is what the
 * WinDosDX input backend reports for the same physical key.
 */
struct MappedKey
{
    MapKeys key;
    Bitu    mods;
    Bit32u  scancode;   /* set-1 make code */
    int     extended;  /* nonzero for the extended-key prefix */
};

static const MappedKey g_default_binds[] = {
    /* MapKeys           mods    scancode  ext */
    { MK_f1,             0,      0x3B,     0 },
    { MK_f2,             0,      0x3C,     0 },
    { MK_f3,             0,      0x3D,     0 },
    { MK_f4,             0,      0x3E,     0 },
    { MK_f5,             0,      0x3F,     0 },
    { MK_f6,             0,      0x40,     0 },
    { MK_f7,             0,      0x41,     0 },
    { MK_f8,             0,      0x42,     0 },
    { MK_f9,             0,      0x43,     0 },
    { MK_f10,            0,      0x44,     0 },
    { MK_f11,            0,      0x57,     0 },
    { MK_f12,            0,      0x58,     0 },
    { MK_return,         0,      0x1C,     0 },
    { MK_kpminus,        0,      0x4A,     0 },
    { MK_scrolllock,     0,      0x46,     0 },
    { MK_printscreen,    0,      0x37,     0 },
    { MK_pause,          0,      0x45,     0 },
};

/* Set by the key event translation in windos_dosbox.cpp. */
static bool g_left_alt = false;
static bool g_right_alt = false;
static bool g_left_ctrl = false;
static bool g_right_ctrl = false;
static bool g_left_shift = false;
static bool g_right_shift = false;

void WD_MapperSetModifier(int which, bool pressed)
{
    switch (which)
    {
    case WD_MOD_LEFT_ALT:    g_left_alt = pressed;    break;
    case WD_MOD_RIGHT_ALT:   g_right_alt = pressed;   break;
    case WD_MOD_LEFT_CTRL:   g_left_ctrl = pressed;   break;
    case WD_MOD_RIGHT_CTRL:  g_right_ctrl = pressed;  break;
    case WD_MOD_LEFT_SHIFT:  g_left_shift = pressed;  break;
    case WD_MOD_RIGHT_SHIFT: g_right_shift = pressed; break;
    default: break;
    }
}

/* --- core API ------------------------------------------------------------ */

void MAPPER_AddHandler(MAPPER_Handler *handler, MapKeys key, Bitu mods,
                       char const *const eventname, char const *const buttonname)
{
    /* Upstream de-duplicates on buttonname, so registering the same action
     * twice (as DOSBOX_RealInit and a hardware init both do) is a no-op. */
    for (size_t i = 0; i < g_events.size(); i++)
    {
        if (g_events[i].buttonname && buttonname &&
            !strcmp(g_events[i].buttonname, buttonname))
            return;
    }

    MappedEvent entry;
    entry.handler    = handler;
    entry.key        = key;
    entry.mods       = mods;
    entry.eventname  = eventname;
    entry.buttonname = buttonname;
    g_events.push_back(entry);
}

void MAPPER_Init(void)
{
    /* Nothing to lay out or load: the bindings are compiled in above. */
}

void MAPPER_StartUp(Section *sec)
{
    (void)sec;
}

void MAPPER_LosingFocus(void)
{
    g_left_alt = g_right_alt = false;
    g_left_ctrl = g_right_ctrl = false;
    g_left_shift = g_right_shift = false;
}

void MAPPER_Run(bool pressed)
{
    /* The interactive mapper has no analogue in WinDosDX. */
    (void)pressed;
}

void MAPPER_RunInternal()
{
    /* Ditto: upstream runs the SDL key-mapping dialog here. */
}

/*
 * Called from GFX_Events() for every translated key event.  Fires the handler
 * of whichever mapped event, if any, is bound to this key.
 */
void WD_MapperKeyEvent(Bit32u scancode, int extended, bool pressed)
{
    size_t i, n;

    for (i = 0, n = g_events.size(); i < n; i++)
    {
        const MappedEvent *event = &g_events[i];
        size_t b;

        for (b = 0; b < sizeof(g_default_binds) / sizeof(g_default_binds[0]); b++)
        {
            const MappedKey *bind = &g_default_binds[b];

            if (bind->scancode != scancode || bind->key != event->key)
                continue;
            if (bind->extended != (extended ? 1 : 0))
                continue;

            /* Honour the modifier requirement the core registered with. */
            if (event->mods & MMOD1)
            {
                if (!(g_left_ctrl || g_right_ctrl || g_left_alt || g_right_alt))
                    continue;
            }
            if (event->mods & MMOD2)
            {
                if (!(g_left_shift || g_right_shift))
                    continue;
            }

            event->handler(pressed);
            return;
        }
    }
}
