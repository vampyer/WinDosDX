/*
 * WinDosDX internal DOS machine compatibility stubs.
 *
 * The WinDosDX machine deliberately exposes only the windos_platform C
 * boundary.  These implementations keep optional DOSBox devices present in
 * the BIOS/device topology without importing SDL, MAME, ASPI, or a host
 * networking stack into WinDosDX.
 */

#include <sys/stat.h>

#include "dosbox.h"
#include "dos/cdrom.h"
#include "include/mouse.h"
#include "include/serialport.h"

bool autofire = false;

void Mouse_AutoLock(bool enable)
{
    (void)enable;
}

CSerial *serialports[4] = { NULL, NULL, NULL, NULL };

bool CSerial::Putchar(Bit8u data, bool wait_dtr, bool wait_rts, Bitu timeout)
{
    (void)data;
    (void)wait_dtr;
    (void)wait_rts;
    (void)timeout;
    return false;
}

bool CSerial::Getchar(Bit8u *data, Bit8u *lsr, bool wait_dsr, Bitu timeout)
{
    (void)data;
    (void)lsr;
    (void)wait_dsr;
    (void)timeout;
    return false;
}

void MIDI_Init(Section *sec)
{
    (void)sec;
}

bool MIDI_Available(void)
{
    return false;
}

void MIDI_RawOutByte(Bit8u data)
{
    (void)data;
}

void CMS_Init(Section *sec)
{
    (void)sec;
}

void CMS_ShutDown(Section *sec)
{
    (void)sec;
}

void TANDYSOUND_Init(Section *sec)
{
    (void)sec;
}

void TANDYSOUND_ShutDown(Section *sec)
{
    (void)sec;
}

bool TS_Get_Address(Bitu &address, Bitu &irq, Bitu &dma)
{
    address = 0;
    irq = 0;
    dma = 0;
    return false;
}

int CDROM_GetMountType(char *path, int forceCD)
{
    (void)path;
    (void)forceCD;
    return 2;
}

bool CDROM_Interface_Fake::GetAudioTracks(int &stTrack, int &end, TMSF &leadOut)
{
    stTrack = end = 1;
    leadOut.min = 60;
    leadOut.sec = leadOut.fr = 0;
    return true;
}

bool CDROM_Interface_Fake::GetAudioTrackInfo(int track, TMSF &start,
                                               unsigned char &attr)
{
    if (track > 1)
        return false;
    start.min = start.fr = 0;
    start.sec = 2;
    attr = 0x60;
    return true;
}

bool CDROM_Interface_Fake::GetAudioSub(unsigned char &attr, unsigned char &track,
                                         unsigned char &index, TMSF &relPos,
                                         TMSF &absPos)
{
    attr = 0;
    track = index = 1;
    relPos.min = relPos.fr = 0;
    relPos.sec = 2;
    absPos.min = absPos.fr = 0;
    absPos.sec = 2;
    return true;
}

bool CDROM_Interface_Fake::GetAudioStatus(bool &playing, bool &pause)
{
    playing = pause = false;
    return true;
}

bool CDROM_Interface_Fake::GetMediaTrayStatus(bool &mediaPresent,
                                                bool &mediaChanged,
                                                bool &trayOpen)
{
    mediaPresent = true;
    mediaChanged = false;
    trayOpen = false;
    return true;
}
