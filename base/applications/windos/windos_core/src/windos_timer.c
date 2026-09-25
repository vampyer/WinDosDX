/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        Timer backend (monotonic clock + periodic callbacks)
 * PROGRAMMERS:    WinDosDX Team
 */

#include "windos_internal.h"

#define WD_MAX_TIMER_CALLBACKS 16

typedef struct
{
    int  in_use;
    u32  interval_ms;
    u64  next_due_ms;
    void (*cb)(void *ctx);
    void *ctx;
} WD_TimerSlot;

static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_start;
static int          g_inited;
static WD_TimerSlot g_slots[WD_MAX_TIMER_CALLBACKS];

int WD_TimerInit(u32 pit_hz)
{
    (void)pit_hz; /* the core paces itself; we only need a steady clock */
    if (g_inited)
        return 0;

    if (!QueryPerformanceFrequency(&g_freq) || g_freq.QuadPart == 0)
        return -1;
    QueryPerformanceCounter(&g_start);

    ZeroMemory(g_slots, sizeof(g_slots));
    g_inited = 1;
    return 0;
}

void WD_TimerShutdown(void)
{
    g_inited = 0;
    ZeroMemory(g_slots, sizeof(g_slots));
}

u64 WD_TimerNowUs(void)
{
    LARGE_INTEGER now;
    double elapsed;
    if (!g_inited)
        return 0;
    QueryPerformanceCounter(&now);
    /* Use double math to avoid pulling in the 64-bit integer helpers
     * (__allmul) that a 32-bit build does not provide in msvcrt. */
    elapsed = (double)(now.QuadPart - g_start.QuadPart) * 1000000.0 /
              (double)g_freq.QuadPart;
    return (u64)elapsed;
}

u64 WD_TimerNowMs(void)
{
    return WD_TimerNowUs() / 1000ULL;
}

int WD_TimerAddCallback(u32 interval_ms, void (*cb)(void *ctx), void *ctx)
{
    int i;
    if (!g_inited || !cb || interval_ms == 0)
        return -1;
    for (i = 0; i < WD_MAX_TIMER_CALLBACKS; i++)
    {
        if (!g_slots[i].in_use)
        {
            g_slots[i].in_use = 1;
            g_slots[i].interval_ms = interval_ms;
            g_slots[i].next_due_ms = WD_TimerNowMs() + interval_ms;
            g_slots[i].cb = cb;
            g_slots[i].ctx = ctx;
            return i;
        }
    }
    return -1;
}

void WD_TimerRemoveCallback(int handle)
{
    if (handle >= 0 && handle < WD_MAX_TIMER_CALLBACKS)
        g_slots[handle].in_use = 0;
}

/*
 * Called by the host machine loop once per frame to fire due callbacks.
 * Exposed for the host; not part of the public contract.
 */
void WD_TimerDispatch(void)
{
    u64 now;
    int i;
    if (!g_inited)
        return;
    now = WD_TimerNowMs();
    for (i = 0; i < WD_MAX_TIMER_CALLBACKS; i++)
    {
        if (g_slots[i].in_use && now >= g_slots[i].next_due_ms)
        {
            g_slots[i].next_due_ms = now + g_slots[i].interval_ms;
            g_slots[i].cb(g_slots[i].ctx);
        }
    }
}
