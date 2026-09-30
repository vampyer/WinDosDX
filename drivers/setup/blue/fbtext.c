/*
 * PROJECT:     ReactOS Console Text-Mode Device Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Text output on a linear framebuffer, for machines without
 *              a VGA text mode (UEFI firmware leaves the display in the
 *              graphics mode set up by its GOP driver).
 * COPYRIGHT:   Copyright 2026 WinDosDX Team
 */

/* INCLUDES ******************************************************************/

#include "blue.h"
#include <ndk/exfuncs.h>

#define NDEBUG
#include <debug.h>

/* The loader's framebuffer description and the finder shared with BootVid */
#include <drivers/bootvid/framebuf.h>
#include <drivers/bootvid/framebuf.c>

/* GLOBALS *******************************************************************/

#define FB_CELL_WIDTH   8

/* Like the VGA mode blue.sys sets up (80x50 with an 8x8 font) */
#define FB_COLUMNS      80
#define FB_ROWS         50

BOOLEAN FbTextActive = FALSE;

static PUCHAR FbBase;           /* Mapped visible framebuffer */
static ULONG FbMappedSize;
static ULONG FbWidth, FbHeight; /* In pixels */
static ULONG FbPitch;           /* In bytes */
static ULONG FbBytesPerPixel;
static ULONG FbOriginX, FbOriginY;
static USHORT FbColumns, FbRows;
static ULONG FbCellHeight;      /* 8, or 16 on screens tall enough */
static ULONG FbPalette[16];     /* Console colors in framebuffer pixel format */

/* What is currently drawn, to only repaint the cells that changed */
static PUSHORT FbDrawn;
static USHORT FbCursorX = 0xFFFF, FbCursorY = 0xFFFF;
static BOOLEAN FbCursorDrawn;

/* FUNCTIONS *****************************************************************/

static ULONG
FbPackChannel(
    _In_ UCHAR Value,
    _In_ ULONG Mask)
{
    ULONG Shift = 0, Bits = 0;

    if (!Mask)
        return 0;
    while (!(Mask & (1UL << Shift)))
        ++Shift;
    while ((Shift + Bits < 32) && (Mask & (1UL << (Shift + Bits))))
        ++Bits;

    /* Scale the 8-bit value down (or up) to the channel width */
    if (Bits < 8)
        return ((ULONG)Value >> (8 - Bits)) << Shift;
    return ((ULONG)Value << (Bits - 8)) << Shift;
}

static BOOLEAN
FbIsUefiBoot(VOID)
{
    struct
    {
        GUID BootIdentifier;
        ULONG FirmwareType;
        ULONGLONG BootFlags;
    } BootInfo;
    NTSTATUS Status;

    Status = ZwQuerySystemInformation((SYSTEM_INFORMATION_CLASS)90 /* SystemBootEnvironmentInformation */,
                                      &BootInfo, sizeof(BootInfo), NULL);
    return NT_SUCCESS(Status) && (BootInfo.FirmwareType == 2 /* FirmwareTypeUefi */);
}

/*
 * Must be called from DriverEntry: the loader's hardware description
 * is only available while boot drivers initialize.
 */
BOOLEAN
FbTextInitialize(
    _In_reads_(16 * 3) const UCHAR* Palette)
{
    PHYSICAL_ADDRESS VramAddress, FrameBuffer, Translated;
    CM_FRAMEBUF_DEVICE_DATA VideoData;
    INTERFACE_TYPE Interface;
    ULONG VramSize, BusNumber, AddressSpace = 0, i;
    NTSTATUS Status;

    /* A BIOS machine has VGA text mode: keep using it */
    if (!FbIsUefiBoot())
        return FALSE;

    Status = FindBootDisplay(&VramAddress, &VramSize, &VideoData, NULL, &Interface, &BusNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No boot framebuffer, text output unavailable\n");
        return FALSE;
    }

    FbWidth = VideoData.ScreenWidth;
    FbHeight = VideoData.ScreenHeight;
    FbBytesPerPixel = (VideoData.BitsPerPixel + 7) / 8;
    FbPitch = VideoData.PixelsPerScanLine * FbBytesPerPixel;
    if (FbWidth < FB_CELL_WIDTH * FB_COLUMNS || FbHeight < 8 * FB_ROWS ||
        FbBytesPerPixel < 2 || FbBytesPerPixel > 4 || FbPitch < FbWidth * FbBytesPerPixel)
    {
        DPRINT1("Unsupported framebuffer %lux%lu, %lu bpp\n",
                FbWidth, FbHeight, VideoData.BitsPerPixel);
        return FALSE;
    }

    FrameBuffer.QuadPart = VramAddress.QuadPart + VideoData.FrameBufferOffset;
    if (!BootTranslateBusAddress(Interface, BusNumber, FrameBuffer, &AddressSpace, &Translated) ||
        AddressSpace != 0)
    {
        DPRINT1("Cannot translate framebuffer address 0x%I64x\n", FrameBuffer.QuadPart);
        return FALSE;
    }

    FbMappedSize = FbHeight * FbPitch;
    FbBase = MmMapIoSpace(Translated, FbMappedSize, MmWriteCombined);
    if (!FbBase)
        FbBase = MmMapIoSpace(Translated, FbMappedSize, MmNonCached);
    if (!FbBase)
        return FALSE;

    FbColumns = FB_COLUMNS;
    FbRows = FB_ROWS;
    FbCellHeight = (FbHeight >= 16 * FB_ROWS) ? 16 : 8;
    FbOriginX = (FbWidth - FbColumns * FB_CELL_WIDTH) / 2;
    FbOriginY = (FbHeight - FbRows * FbCellHeight) / 2;

    FbDrawn = ExAllocatePoolWithTag(NonPagedPool, FbColumns * FbRows * sizeof(USHORT), TAG_BLUE);
    if (!FbDrawn)
    {
        MmUnmapIoSpace(FbBase, FbMappedSize);
        FbBase = NULL;
        return FALSE;
    }

    for (i = 0; i < 16; ++i)
    {
        FbPalette[i] = FbPackChannel(Palette[i * 3 + 0], VideoData.PixelMasks.RedMask) |
                       FbPackChannel(Palette[i * 3 + 1], VideoData.PixelMasks.GreenMask) |
                       FbPackChannel(Palette[i * 3 + 2], VideoData.PixelMasks.BlueMask);
    }

    DPRINT1("Text output on a %lux%lu framebuffer (%lu bpp)\n",
            FbWidth, FbHeight, VideoData.BitsPerPixel);
    FbTextActive = TRUE;
    return TRUE;
}

VOID
FbTextGetGeometry(
    _Out_ PUSHORT Columns,
    _Out_ PUSHORT Rows,
    _Out_ PUCHAR ScanLines)
{
    *Columns = FbColumns;
    *Rows = FbRows;
    *ScanLines = (UCHAR)FbCellHeight;
}

static inline VOID
FbPutPixel(
    _In_ PUCHAR Pixel,
    _In_ ULONG Color)
{
    switch (FbBytesPerPixel)
    {
        case 4:
            *(volatile ULONG*)Pixel = Color;
            break;
        case 3:
            Pixel[0] = (UCHAR)Color;
            Pixel[1] = (UCHAR)(Color >> 8);
            Pixel[2] = (UCHAR)(Color >> 16);
            break;
        default:
            *(volatile USHORT*)Pixel = (USHORT)Color;
            break;
    }
}

static VOID
FbDrawCell(
    _In_ USHORT X,
    _In_ USHORT Y,
    _In_ UCHAR Char,
    _In_ UCHAR Attribute,
    _In_opt_ PUCHAR Font8x8,
    _In_ BOOLEAN Cursor)
{
    ULONG Fore = FbPalette[Attribute & 0x0F];
    ULONG Back = FbPalette[(Attribute >> 4) & 0x0F];
    PUCHAR Line = FbBase + (FbOriginY + Y * FbCellHeight) * FbPitch +
                  (FbOriginX + X * FB_CELL_WIDTH) * FbBytesPerPixel;
    ULONG Row, Col;
    UCHAR Bits;

    for (Row = 0; Row < FbCellHeight; ++Row, Line += FbPitch)
    {
        /* The console loads an 8x8 code page font; before that, use the
         * built-in 8x16 one. Each is scaled to the cell height. */
        if (Font8x8)
            Bits = Font8x8[Char * 8 + Row * 8 / FbCellHeight];
        else if (FbCellHeight == 16)
            Bits = FbTextFont8x16[Char * 16 + Row];
        else
            Bits = FbTextFont8x16[Char * 16 + Row * 2] | FbTextFont8x16[Char * 16 + Row * 2 + 1];

        /* The cursor is an underline over the bottom scan lines */
        if (Cursor && Row >= FbCellHeight - FbCellHeight / 8)
            Bits = 0xFF;

        for (Col = 0; Col < FB_CELL_WIDTH; ++Col)
            FbPutPixel(Line + Col * FbBytesPerPixel, (Bits & (0x80 >> Col)) ? Fore : Back);
    }
}

VOID
FbTextClear(VOID)
{
    PUCHAR Line = FbBase;
    ULONG Y, X;

    for (Y = 0; Y < FbHeight; ++Y, Line += FbPitch)
    {
        for (X = 0; X < FbWidth; ++X)
            FbPutPixel(Line + X * FbBytesPerPixel, FbPalette[0]);
    }
    FbTextInvalidate();
}

/* Forget what is on the screen, so the next refresh repaints every cell */
VOID
FbTextInvalidate(VOID)
{
    if (FbDrawn)
        RtlFillMemoryUlong(FbDrawn, FbColumns * FbRows * sizeof(USHORT), 0xFFFFFFFF);
    FbCursorDrawn = FALSE;
}

/*
 * Repaints the cells of the character/attribute text buffer that changed
 * since the last call, and moves the cursor.
 */
VOID
FbTextRefresh(
    _In_reads_(Columns * Rows * 2) PUCHAR TextBuffer,
    _In_ USHORT Columns,
    _In_ USHORT Rows,
    _In_opt_ PUCHAR Font8x8,
    _In_ USHORT CursorX,
    _In_ USHORT CursorY,
    _In_ BOOLEAN CursorVisible)
{
    PUSHORT Cells = (PUSHORT)TextBuffer;
    ULONG Index, Count;
    USHORT X, Y;

    if (!FbTextActive || !TextBuffer)
        return;

    Columns = min(Columns, FbColumns);
    Rows = min(Rows, FbRows);
    Count = Columns * Rows;

    /* Erase the old cursor if it moved */
    if (FbCursorDrawn && (FbCursorX != CursorX || FbCursorY != CursorY || !CursorVisible) &&
        FbCursorX < Columns && FbCursorY < Rows)
    {
        FbDrawn[FbCursorY * Columns + FbCursorX] = 0xFFFF;
    }

    for (Index = 0; Index < Count; ++Index)
    {
        if (FbDrawn[Index] == Cells[Index])
            continue;
        X = (USHORT)(Index % Columns);
        Y = (USHORT)(Index / Columns);
        FbDrawCell(X, Y, (UCHAR)Cells[Index], (UCHAR)(Cells[Index] >> 8), Font8x8, FALSE);
        FbDrawn[Index] = Cells[Index];
    }

    FbCursorDrawn = FALSE;
    if (CursorVisible && CursorX < Columns && CursorY < Rows)
    {
        Index = CursorY * Columns + CursorX;
        FbDrawCell(CursorX, CursorY, (UCHAR)Cells[Index], (UCHAR)(Cells[Index] >> 8), Font8x8, TRUE);
        /* Make the next refresh redraw this cell without the cursor if needed */
        FbDrawn[Index] = 0xFFFF;
        FbCursorDrawn = TRUE;
    }
    FbCursorX = CursorX;
    FbCursorY = CursorY;
}

/* EOF */
