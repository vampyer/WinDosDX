/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        Sound backend (waveOut sink)
 * PROGRAMMERS:    WinDosDX Team
 *
 * Consumes the core's mixed 16-bit signed stereo buffer through waveOut. A
 * dedicated mixer thread pulls frames via the fill callback and pushes them
 * into a small queue of WAVEHDRs, so the machine thread never blocks on audio.
 */

#include "windos_internal.h"

#define WD_SOUND_BUFFERS 4

typedef struct
{
    WAVEHDR   hdr;
    BYTE     *data;
    u32       frames;      /* frames per buffer */
} WD_SoundBuf;

static HWAVEOUT       g_waveout;
static u32            g_rate;
static u16            g_channels;
static WD_SoundFillFn g_fill;
static void          *g_fill_ctx;
static WD_SoundBuf    g_bufs[WD_SOUND_BUFFERS];
static int            g_playing;
static int            g_paused;
static int            g_volume = 100;
static HANDLE         g_mix_thread;
static CRITICAL_SECTION g_lock;
static int            g_lock_ready;

static DWORD WINAPI
WD_SoundMixThread(LPVOID param)
{
    (void)param;
    for (;;)
    {
        int i;
        if (!g_playing)
            break;
        for (i = 0; i < WD_SOUND_BUFFERS; i++)
        {
            WD_SoundBuf *b = &g_bufs[i];
            if (b->hdr.dwFlags & WHDR_DONE)
            {
                waveOutUnprepareHeader(g_waveout, &b->hdr, sizeof(b->hdr));
                waveOutPrepareHeader(g_waveout, &b->hdr, sizeof(b->hdr));
                if (g_fill)
                    g_fill(g_fill_ctx, (s16 *)b->data, b->frames);
                b->hdr.dwBufferLength = b->frames * g_channels * sizeof(s16);
                b->hdr.dwFlags = 0;
                waveOutWrite(g_waveout, &b->hdr, sizeof(b->hdr));
            }
        }
        Sleep(5);
    }
    return 0;
}

int WD_SoundInit(u32 rate, u16 channels, WD_SoundFillFn fill, void *fill_ctx)
{
    WAVEFORMATEX wfx;
    u32 frame_bytes;
    int i;

    if (g_waveout)
        return 0;
    if (!rate || !channels)
        return -1;

    g_rate = rate;
    g_channels = channels ? channels : 2;
    g_fill = fill;
    g_fill_ctx = fill_ctx;

    if (!g_lock_ready)
    {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
    }

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = g_channels;
    wfx.nSamplesPerSec = g_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(g_channels * sizeof(s16));
    wfx.nAvgBytesPerSec = g_rate * g_channels * sizeof(s16);

    if (waveOutOpen(&g_waveout, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL)
        != MMSYSERR_NOERROR)
    {
        g_waveout = NULL;
        return -1;
    }

    frame_bytes = 1024; /* ~1024 frames per buffer */
    for (i = 0; i < WD_SOUND_BUFFERS; i++)
    {
        g_bufs[i].frames = 1024;
        g_bufs[i].data = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                           1024 * g_channels * sizeof(s16));
        if (!g_bufs[i].data)
            break;
        ZeroMemory(&g_bufs[i].data, 1024 * g_channels * sizeof(s16));
        ZeroMemory(&g_bufs[i].hdr, sizeof(g_bufs[i].hdr));
        g_bufs[i].hdr.lpData = (LPSTR)g_bufs[i].data;
        g_bufs[i].hdr.dwBufferLength = 1024 * g_channels * sizeof(s16);
        waveOutPrepareHeader(g_waveout, &g_bufs[i].hdr, sizeof(g_bufs[i].hdr));
    }

    (void)frame_bytes;

    g_playing = 1;
    g_mix_thread = CreateThread(NULL, 0, WD_SoundMixThread, NULL, 0, NULL);
    return 0;
}

void WD_SoundShutdown(void)
{
    int i;
    if (!g_waveout)
        return;

    g_playing = 0;
    if (g_mix_thread)
    {
        WaitForSingleObject(g_mix_thread, 2000);
        CloseHandle(g_mix_thread);
        g_mix_thread = NULL;
    }

    waveOutReset(g_waveout);
    for (i = 0; i < WD_SOUND_BUFFERS; i++)
    {
        if (g_bufs[i].data)
        {
            waveOutUnprepareHeader(g_waveout, &g_bufs[i].hdr,
                                   sizeof(g_bufs[i].hdr));
            HeapFree(GetProcessHeap(), 0, g_bufs[i].data);
            g_bufs[i].data = NULL;
        }
    }
    waveOutClose(g_waveout);
    g_waveout = NULL;
}

int WD_SoundSetVolume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_volume = volume;
    if (g_waveout)
    {
        WAVEFORMATEX wfx;
        wfx.nChannels = g_channels;
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nSamplesPerSec = g_rate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = (WORD)(g_channels * sizeof(s16));
        wfx.nAvgBytesPerSec = g_rate * g_channels * sizeof(s16);
        wfx.cbSize = 0;
        waveOutSetVolume(g_waveout, (DWORD)((volume * 0xFFFF) / 100));
    }
    return 0;
}

int WD_SoundGetVolume(void)
{
    return g_volume;
}

void WD_SoundPause(int paused)
{
    g_paused = paused;
    if (!g_waveout)
        return;
    if (paused)
        waveOutPause(g_waveout);
    else
        waveOutRestart(g_waveout);
}
