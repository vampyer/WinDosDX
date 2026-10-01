/*
 * PROJECT:     ReactOS VBE miniport video driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Use the firmware framebuffer on machines without a video BIOS
 *              (UEFI firmware leaves the display in a GOP graphics mode).
 * COPYRIGHT:   Copyright 2026 WinDosDX Team
 */

/* This file uses the kernel headers, so it is kept apart from the
 * miniport ones (video.h) that the rest of the driver is built with. */

#include <ntifs.h>
#include <ndk/exfuncs.h>
#include "fbmode.h"

#define NDEBUG
#include <debug.h>

/* The loader's framebuffer description and its parser, shared with BootVid */
#include <drivers/bootvid/framebuf.h>
#include <drivers/bootvid/framebuf.c>

#define TAG_VBE_FB 'BFBV'

BOOLEAN
VBEIsUefiBoot(VOID)
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

static HANDLE
FbOpenKey(
    _In_opt_ HANDLE Root,
    _In_ PCWSTR Name)
{
    UNICODE_STRING KeyName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle;

    RtlInitUnicodeString(&KeyName, Name);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Root, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Handle, KEY_READ, &ObjectAttributes)))
        return NULL;
    return Handle;
}

/* Name of the Index'th subkey of Key, as a NUL-terminated string */
static BOOLEAN
FbEnumKey(
    _In_ HANDLE Key,
    _In_ ULONG Index,
    _Out_writes_(NameChars) PWSTR Name,
    _In_ ULONG NameChars)
{
    UCHAR Buffer[sizeof(KEY_BASIC_INFORMATION) + 64 * sizeof(WCHAR)];
    PKEY_BASIC_INFORMATION Info = (PKEY_BASIC_INFORMATION)Buffer;
    ULONG Length;

    if (!NT_SUCCESS(ZwEnumerateKey(Key, Index, KeyBasicInformation,
                                   Info, sizeof(Buffer), &Length)))
        return FALSE;
    if (Info->NameLength / sizeof(WCHAR) >= NameChars)
        return FALSE;
    RtlCopyMemory(Name, Info->Name, Info->NameLength);
    Name[Info->NameLength / sizeof(WCHAR)] = UNICODE_NULL;
    return TRUE;
}

/* Parse the "Configuration Data" of a DisplayController key */
static BOOLEAN
FbReadDisplayController(
    _In_ HANDLE Controller,
    _Out_ PVBE_FB_INFO Info)
{
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"Configuration Data");
    PKEY_VALUE_PARTIAL_INFORMATION Value;
    PCM_FULL_RESOURCE_DESCRIPTOR Full;
    CM_FRAMEBUF_DEVICE_DATA VideoData;
    PHYSICAL_ADDRESS VramAddress;
    ULONG VramSize, Length = 0;
    NTSTATUS Status;
    BOOLEAN Found = FALSE;

    Status = ZwQueryValueKey(Controller, &ValueName, KeyValuePartialInformation, NULL, 0, &Length);
    if (Status != STATUS_BUFFER_TOO_SMALL && Status != STATUS_BUFFER_OVERFLOW)
        return FALSE;

    Value = ExAllocatePoolWithTag(PagedPool, Length, TAG_VBE_FB);
    if (!Value)
        return FALSE;

    Status = ZwQueryValueKey(Controller, &ValueName, KeyValuePartialInformation,
                             Value, Length, &Length);
    if (NT_SUCCESS(Status) &&
        Value->DataLength > FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR, PartialResourceList))
    {
        Full = (PCM_FULL_RESOURCE_DESCRIPTOR)Value->Data;
        RtlZeroMemory(&VideoData, sizeof(VideoData));
        Status = GetFramebufferVideoData(&VramAddress, &VramSize, &VideoData,
                                         &Full->PartialResourceList,
                                         Value->DataLength -
                                         FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR, PartialResourceList));
        if (NT_SUCCESS(Status) && VramAddress.QuadPart != 0 &&
            VideoData.ScreenWidth > 1 && VideoData.ScreenHeight > 1 &&
            VideoData.BitsPerPixel >= 16 && VideoData.BitsPerPixel <= 32)
        {
            Info->Address = VramAddress.QuadPart + VideoData.FrameBufferOffset;
            Info->Width = VideoData.ScreenWidth;
            Info->Height = VideoData.ScreenHeight;
            Info->BitsPerPixel = VideoData.BitsPerPixel;
            Info->Pitch = VideoData.PixelsPerScanLine * ((VideoData.BitsPerPixel + 7) / 8);
            Info->RedMask = VideoData.PixelMasks.RedMask;
            Info->GreenMask = VideoData.PixelMasks.GreenMask;
            Info->BlueMask = VideoData.PixelMasks.BlueMask;
            Found = TRUE;
        }
    }

    ExFreePoolWithTag(Value, TAG_VBE_FB);
    return Found;
}

/*
 * The loader describes the boot framebuffer in the hardware tree, which the
 * kernel copies to HARDWARE\DESCRIPTION\System\<Adapter>\<N>\DisplayController.
 */
BOOLEAN
VBEFindFirmwareFramebuffer(
    _Out_ PVBE_FB_INFO Info)
{
    WCHAR AdapterName[64], BusName[16], ControllerName[16];
    HANDLE System, Adapter, Bus, Display, Controller;
    ULONG i, j, k;
    BOOLEAN Found = FALSE;

    PAGED_CODE();

    System = FbOpenKey(NULL, L"\\Registry\\Machine\\HARDWARE\\DESCRIPTION\\System");
    if (!System)
        return FALSE;

    for (i = 0; !Found && FbEnumKey(System, i, AdapterName, ARRAYSIZE(AdapterName)); ++i)
    {
        Adapter = FbOpenKey(System, AdapterName);
        if (!Adapter)
            continue;

        for (j = 0; !Found && FbEnumKey(Adapter, j, BusName, ARRAYSIZE(BusName)); ++j)
        {
            Bus = FbOpenKey(Adapter, BusName);
            if (!Bus)
                continue;

            Display = FbOpenKey(Bus, L"DisplayController");
            if (Display)
            {
                for (k = 0; !Found && FbEnumKey(Display, k, ControllerName, ARRAYSIZE(ControllerName)); ++k)
                {
                    Controller = FbOpenKey(Display, ControllerName);
                    if (!Controller)
                        continue;
                    Found = FbReadDisplayController(Controller, Info);
                    ZwClose(Controller);
                }
                ZwClose(Display);
            }
            ZwClose(Bus);
        }
        ZwClose(Adapter);
    }
    ZwClose(System);

    if (Found)
    {
        DPRINT1("Firmware framebuffer at 0x%I64x, %lux%lu, %lu bpp, pitch %lu\n",
                Info->Address, Info->Width, Info->Height, Info->BitsPerPixel, Info->Pitch);
    }
    return Found;
}
