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
 * WinDosDX physical CD-ROM support.
 *
 * dos/dos_mscdex.cpp instantiates CDROM_Interface_SDL when MSCDEX is asked to
 * mount a physical drive (CDROM_GetMountType returning 0x00).  WinDosDX has no
 * host CD-ROM to hand out, so this implementation reports the drive as absent.
 * Mounting an ISO or BIN/CUE image still works, because that path selects
 * CDROM_Interface_Image from cdrom_image.cpp instead.
 *
 * The class is defined by DOSBox in dos/cdrom.h, so only its members are
 * implemented here.
 */

#include <string.h>

#include "SDL.h"
#include "dos/cdrom.h"

CDROM_Interface_SDL::CDROM_Interface_SDL(void)
{
    driveID = 0;
    oldLeadOut = 0;
}

CDROM_Interface_SDL::~CDROM_Interface_SDL(void)
{
    Close();
}

bool CDROM_Interface_SDL::Open(void)
{
    /* No physical CD-ROM drives are exposed to the DOS machine. */
    return false;
}

void CDROM_Interface_SDL::Close(void)
{
}

bool CDROM_Interface_SDL::SetDevice(char *path, int forceCD)
{
    (void)path;
    (void)forceCD;

    /* No physical CD-ROM drives are exposed to the DOS machine, so the mount
     * never succeeds.  ISO and BIN/CUE images take the CDROM_Interface_Image
     * path in dos_mscdex.cpp instead and are unaffected. */
    return false;
}

bool CDROM_Interface_SDL::GetAudioTracks(int &stTrack, int &end, TMSF &leadOut)
{
    stTrack = 0;
    end = 0;
    leadOut.min = leadOut.sec = leadOut.fr = 0;
    return false;
}

bool CDROM_Interface_SDL::GetAudioTrackInfo(int track, TMSF &start,
                                            unsigned char &attr)
{
    (void)track;
    start.min = start.sec = start.fr = 0;
    attr = 0;
    return false;
}

bool CDROM_Interface_SDL::GetAudioSub(unsigned char &attr, unsigned char &track,
                                      unsigned char &index, TMSF &relPos,
                                      TMSF &absPos)
{
    attr = track = index = 0;
    relPos.min = relPos.sec = relPos.fr = 0;
    absPos.min = absPos.sec = absPos.fr = 0;
    return false;
}

bool CDROM_Interface_SDL::GetAudioStatus(bool &playing, bool &pause)
{
    playing = false;
    pause = false;
    return false;
}

bool CDROM_Interface_SDL::GetMediaTrayStatus(bool &mediaPresent,
                                             bool &mediaChanged, bool &trayOpen)
{
    mediaPresent = false;
    mediaChanged = false;
    trayOpen = false;
    return false;
}

bool CDROM_Interface_SDL::PlayAudioSector(unsigned long start, unsigned long len)
{
    (void)start;
    (void)len;
    return false;
}

bool CDROM_Interface_SDL::PauseAudio(bool resume)
{
    (void)resume;
    return false;
}

bool CDROM_Interface_SDL::StopAudio(void)
{
    return false;
}

bool CDROM_Interface_SDL::LoadUnloadMedia(bool unload)
{
    (void)unload;
    return false;
}
