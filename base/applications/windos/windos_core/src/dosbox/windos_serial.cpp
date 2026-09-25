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
 * WinDosDX serial port support.
 *
 * Upstream DOSBox's hardware/serialport/ directory implements modem, null
 * modem and direct-serial backends on top of BSD sockets and SDL_net, none of
 * which WinDosDX has.  Rather than pull a socket stack into the DOS engine,
 * WinDosDX reports the standard PC COM port base addresses to the BIOS so that
 * DOS programs which probe for serial hardware still see a realistic machine,
 * and leaves the ports themselves disconnected.
 *
 * Real host serial passthrough can be added later behind the same entry point.
 */

#include "dosbox.h"
#include "bios.h"
#include "setup.h"

void SERIAL_Init(Section *sec)
{
    /* Standard PC COM port base addresses, as a real ISA machine reports. */
    static Bit16u biosParameter[4] = { 0x3F8, 0x2F8, 0x3E8, 0x2E8 };

    (void)sec;

    BIOS_SetComPorts(biosParameter);
}
