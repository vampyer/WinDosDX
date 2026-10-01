/*
 * PROJECT:     ReactOS VBE miniport video driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Firmware framebuffer discovery (no kernel headers needed)
 * COPYRIGHT:   Copyright 2026 WinDosDX Team
 */

#pragma once

typedef struct _VBE_FB_INFO
{
    ULONGLONG Address;      /* Physical address of the visible framebuffer */
    ULONG Width;
    ULONG Height;
    ULONG Pitch;            /* In bytes */
    ULONG BitsPerPixel;
    ULONG RedMask;
    ULONG GreenMask;
    ULONG BlueMask;
} VBE_FB_INFO, *PVBE_FB_INFO;

BOOLEAN
VBEIsUefiBoot(VOID);

BOOLEAN
VBEFindFirmwareFramebuffer(
    _Out_ PVBE_FB_INFO Info);
