/*
 * Sound check: plays two seconds of a 1 kHz tone through waveOut.
 *
 * Reported on SNDREG lines; the harness can record what the guest's sound
 * card plays (ntfs-regression.py --audio-wav) and wav-level.py tells whether
 * the tone arrived. Like the DOS checks, it does not change the overall
 * result: the file system suites run on machines without sound too.
 */

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS

#include <windef.h>
#include <winbase.h>
#include <mmsystem.h>
#include <math.h>

#include "exfat-tests.h"

#define TONE_RATE       48000
#define TONE_SECONDS    2

VOID
SoundRunTests(void)
{
    WAVEFORMATEX Format;
    WAVEHDR Header;
    HWAVEOUT WaveOut;
    WAVEOUTCAPSW Caps;
    MMRESULT Result;
    SHORT *Samples;
    ULONG Frames = TONE_RATE * TONE_SECONDS, i, Waited;
    UINT Devices;

    Emit("SNDREG BEGIN");
    Devices = waveOutGetNumDevs();
    Emit("SNDREG INFO %u wave output device(s)", Devices);
    if (Devices && waveOutGetDevCapsW(0, &Caps, sizeof(Caps)) == MMSYSERR_NOERROR)
    {
        char Name[64];
        for (i = 0; i < ARRAYSIZE(Name) - 1 && Caps.szPname[i]; i++)
            Name[i] = (Caps.szPname[i] >= 0x20 && Caps.szPname[i] < 0x7F) ? (char)Caps.szPname[i] : '?';
        Name[i] = '\0';
        Emit("SNDREG INFO device 0: %s", Name);
    }
    if (!Devices)
    {
        Emit("SNDREG FAIL no wave output device");
        Emit("SNDREG END");
        return;
    }

    Format.wFormatTag = WAVE_FORMAT_PCM;
    Format.nChannels = 2;
    Format.nSamplesPerSec = TONE_RATE;
    Format.wBitsPerSample = 16;
    Format.nBlockAlign = 4;
    Format.nAvgBytesPerSec = TONE_RATE * 4;
    Format.cbSize = 0;
    Result = waveOutOpen(&WaveOut, WAVE_MAPPER, &Format, 0, 0, CALLBACK_NULL);
    if (Result != MMSYSERR_NOERROR)
    {
        Emit("SNDREG FAIL waveOutOpen %u", Result);
        Emit("SNDREG END");
        return;
    }

    Samples = (SHORT *)HeapAlloc(GetProcessHeap(), 0, Frames * 4);
    if (!Samples)
    {
        waveOutClose(WaveOut);
        Emit("SNDREG FAIL out of memory");
        Emit("SNDREG END");
        return;
    }
    /* 1 kHz at half scale, both channels. */
    for (i = 0; i < Frames; i++)
    {
        SHORT Value = (SHORT)(16384.0 * sin(2.0 * 3.14159265358979 * 1000.0 * i / TONE_RATE));
        Samples[2 * i] = Samples[2 * i + 1] = Value;
    }

    ZeroMemory(&Header, sizeof(Header));
    Header.lpData = (LPSTR)Samples;
    Header.dwBufferLength = Frames * 4;
    Result = waveOutPrepareHeader(WaveOut, &Header, sizeof(Header));
    if (Result == MMSYSERR_NOERROR)
        Result = waveOutWrite(WaveOut, &Header, sizeof(Header));
    if (Result != MMSYSERR_NOERROR)
    {
        Emit("SNDREG FAIL waveOutWrite %u", Result);
    }
    else
    {
        Emit("SNDREG INFO playing a %u s tone", TONE_SECONDS);
        for (Waited = 0; Waited < (TONE_SECONDS + 6) * 10 && !(Header.dwFlags & WHDR_DONE); Waited++)
            Sleep(100);
        if (Header.dwFlags & WHDR_DONE)
            Emit("SNDREG PASS tone played (%lu ms)", Waited * 100);
        else
            Emit("SNDREG FAIL tone not finished after %u s", TONE_SECONDS + 6);
    }

    waveOutReset(WaveOut);
    waveOutUnprepareHeader(WaveOut, &Header, sizeof(Header));
    waveOutClose(WaveOut);
    HeapFree(GetProcessHeap(), 0, Samples);
    Emit("SNDREG END");
}
