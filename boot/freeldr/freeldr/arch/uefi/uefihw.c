/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hardware detection routines
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <uefildr.h>
#include "../vidfb.h"

#include <debug.h>
DBG_DEFAULT_CHANNEL(HWDETECT);

/* GLOBALS *******************************************************************/

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

/* From uefivid.c */
extern ULONG_PTR VramAddress;
extern ULONG VramSize;
extern PCM_FRAMEBUF_DEVICE_DATA FrameBufferData;

BOOLEAN AcpiPresent = FALSE;
static EFI_EVENT IdleTimerEvent = NULL;

/* FUNCTIONS *****************************************************************/

VOID
StallExecutionProcessor(ULONG Microseconds)
{
    GlobalSystemTable->BootServices->Stall(Microseconds);
}

VOID
UefiHwIdle(VOID)
{
    UINTN Index;
    EFI_STATUS Status;
    EFI_BOOT_SERVICES *BootServices = GlobalSystemTable->BootServices;

    /* Keep one timer event around and arm it each idle tick */
    if (IdleTimerEvent == NULL)
    {
        Status = BootServices->CreateEvent(EVT_TIMER,
                                           TPL_APPLICATION,
                                           NULL,
                                           NULL,
                                           &IdleTimerEvent);
        if (EFI_ERROR(Status))
        {
            StallExecutionProcessor(10000); /* 10 ms fallback */
            return;
        }
    }

    /* Set a 10ms (100,000 * 100ns) relative timer */
    Status = BootServices->SetTimer(IdleTimerEvent, TimerRelative, 100000);
    if (!EFI_ERROR(Status))
        Status = BootServices->WaitForEvent(1, &IdleTimerEvent, &Index);
    if (EFI_ERROR(Status))
        StallExecutionProcessor(10000); /* 10 ms fallback */
}

BOOLEAN IsAcpiPresent(VOID)
{
    return AcpiPresent;
}

static
PRSDP_DESCRIPTOR
FindAcpiBios(VOID)
{
    UINTN i;
    RSDP_DESCRIPTOR* rsdp = NULL;
    EFI_GUID acpi2_guid = EFI_ACPI_20_TABLE_GUID;

    for (i = 0; i < GlobalSystemTable->NumberOfTableEntries; i++)
    {
        if (!memcmp(&GlobalSystemTable->ConfigurationTable[i].VendorGuid,
                    &acpi2_guid, sizeof(acpi2_guid)))
        {
            rsdp = (RSDP_DESCRIPTOR*)GlobalSystemTable->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    return rsdp;
}

PDESCRIPTION_HEADER
UefiFindAcpiTable(
    _In_ ULONG Signature)
{
    UINTN Index, Count;
    PRSDP_DESCRIPTOR Rsdp;

    Rsdp = FindAcpiBios();
    if (Rsdp == NULL)
        return NULL;

    if ((Rsdp->revision > 0) && (Rsdp->xsdt_physical_address != 0))
    {
        PXSDT Xsdt = (PXSDT)(ULONG_PTR)Rsdp->xsdt_physical_address;

        if ((Xsdt != NULL) && (Xsdt->Header.Length >= sizeof(Xsdt->Header)))
        {
            Count = (Xsdt->Header.Length - sizeof(Xsdt->Header)) / sizeof(Xsdt->Tables[0]);
            for (Index = 0; Index < Count; ++Index)
            {
                PDESCRIPTION_HEADER Header =
                    (PDESCRIPTION_HEADER)(ULONG_PTR)Xsdt->Tables[Index].QuadPart;

                if ((Header != NULL) && (Header->Signature == Signature))
                    return Header;
            }
        }
    }

    if (Rsdp->rsdt_physical_address != 0)
    {
        PRSDT Rsdt = (PRSDT)(ULONG_PTR)Rsdp->rsdt_physical_address;

        if ((Rsdt != NULL) && (Rsdt->Header.Length >= sizeof(Rsdt->Header)))
        {
            Count = (Rsdt->Header.Length - sizeof(Rsdt->Header)) / sizeof(Rsdt->Tables[0]);
            for (Index = 0; Index < Count; ++Index)
            {
                PDESCRIPTION_HEADER Header =
                    (PDESCRIPTION_HEADER)(ULONG_PTR)Rsdt->Tables[Index];

                if ((Header != NULL) && (Header->Signature == Signature))
                    return Header;
            }
        }
    }

    return NULL;
}

VOID
DetectAcpiBios(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCONFIGURATION_COMPONENT_DATA BiosKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PRSDP_DESCRIPTOR Rsdp;
    PACPI_BIOS_DATA AcpiBiosData;
    ULONG TableSize, Size;

    Rsdp = FindAcpiBios();

    if (Rsdp)
    {
        /* Set up the flag in the loader block */
        AcpiPresent = TRUE;

        /* Calculate the table size */
        TableSize = sizeof(ACPI_BIOS_DATA);

        /* Set 'Configuration Data' value */
        Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[1]) + TableSize;
        PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
        if (PartialResourceList == NULL)
        {
            ERR("Failed to allocate resource descriptor\n");
            return;
        }

        RtlZeroMemory(PartialResourceList, Size);
        PartialResourceList->Version = 0;
        PartialResourceList->Revision = 0;
        PartialResourceList->Count = 1;

        PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
        PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
        PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
        PartialDescriptor->u.DeviceSpecificData.DataSize = TableSize;

        /* Fill the table */
        AcpiBiosData = (PACPI_BIOS_DATA)(PartialDescriptor + 1);

        if (Rsdp->revision > 0)
        {
            TRACE("ACPI >1.0, using XSDT address\n");
            AcpiBiosData->RSDTAddress.QuadPart = Rsdp->xsdt_physical_address;
        }
        else
        {
            TRACE("ACPI 1.0, using RSDT address\n");
            AcpiBiosData->RSDTAddress.LowPart = Rsdp->rsdt_physical_address;
        }

        AcpiBiosData->Count = 0;

        TRACE("RSDT %p, data size %x\n", Rsdp->rsdt_physical_address, TableSize);

        /* Create new bus key */
        FldrCreateComponentKey(SystemKey,
                               AdapterClass,
                               MultiFunctionAdapter,
                               0x0,
                               0x0,
                               0xFFFFFFFF,
                               "ACPI BIOS",
                               PartialResourceList,
                               Size,
                               &BiosKey);

        /* Increment bus number */
        (*BusNumber)++;
    }
}

static VOID
DetectDisplayController(
    _In_ PCONFIGURATION_COMPONENT_DATA BusKey)
{
    PCONFIGURATION_COMPONENT_DATA ControllerKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PCM_FRAMEBUF_DEVICE_DATA FramebufData;
    ULONG Size;

    if (!VramAddress || (VramSize == 0) || !FrameBufferData)
        return;

    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[2]) + sizeof(*FramebufData);
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (PartialResourceList == NULL)
    {
        ERR("Failed to allocate resource descriptor\n");
        return;
    }

    /* Initialize resource descriptor */
    RtlZeroMemory(PartialResourceList, Size);
    PartialResourceList->Version  = 1;
    PartialResourceList->Revision = 2;
    PartialResourceList->Count = 2;

    /* Set Memory */
    PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
    PartialDescriptor->Type = CmResourceTypeMemory;
    PartialDescriptor->ShareDisposition = CmResourceShareDeviceExclusive;
    PartialDescriptor->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
    PartialDescriptor->u.Memory.Start.QuadPart = VramAddress;
    PartialDescriptor->u.Memory.Length = VramSize;

    /* Set framebuffer-specific data */
    PartialDescriptor = &PartialResourceList->PartialDescriptors[1];
    PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
    PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
    PartialDescriptor->Flags = 0;
    PartialDescriptor->u.DeviceSpecificData.DataSize = sizeof(*FramebufData);

    /* Get pointer to framebuffer-specific data */
    FramebufData = (PCM_FRAMEBUF_DEVICE_DATA)(PartialDescriptor + 1);
    RtlCopyMemory(FramebufData, FrameBufferData, sizeof(*FrameBufferData));
    FramebufData->Version  = 1;
    FramebufData->Revision = 3;
    FramebufData->VideoClock = 0; // FIXME: Use EDID

    FldrCreateComponentKey(BusKey,
                           ControllerClass,
                           DisplayController,
                           Output | ConsoleOut,
                           0,
                           0xFFFFFFFF,
                           "UEFI GOP Framebuffer",
                           PartialResourceList,
                           Size,
                           &ControllerKey);

    // NOTE: Don't add a MonitorPeripheral for now.
    // We should use EDID data for it.
}

#define FIRST_BIOS_DISK 0x80

/*
 * Report the disks the firmware can boot from the way the PC loader reports
 * its BIOS drives: INT13-style parameters in the System key, and a
 * DiskController with one DiskPeripheral per drive, identified by
 * checksum and MBR signature. Setup uses these to decide which disks
 * the firmware sees and where the system partition can go.
 */
static
VOID
DetectFirmwareDisks(
    _In_ PCONFIGURATION_COMPONENT_DATA SystemKey,
    _In_ PCONFIGURATION_COMPONENT_DATA BusKey)
{
    PCONFIGURATION_COMPONENT_DATA ControllerKey, DiskKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_INT13_DRIVE_PARAMETER Int13Drives;
    PCM_DISK_GEOMETRY_DEVICE_DATA DiskGeometry;
    GEOMETRY Geometry;
    UCHAR DiskCount, i;
    PCSTR Identifier;
    ULONGLONG Cylinders;
    ULONG Size;

    DiskCount = UefiGetHarddiskCount();
    if (DiskCount == 0)
        return;

    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (!PartialResourceList)
        return;
    RtlZeroMemory(PartialResourceList, Size);
    FldrCreateComponentKey(BusKey,
                           ControllerClass,
                           DiskController,
                           Output | Input,
                           0x0,
                           0xFFFFFFFF,
                           NULL,
                           PartialResourceList,
                           Size,
                           &ControllerKey);

    /* INT13-style drive parameters. UEFI has no CHS geometry; use the
     * usual translated one so the values stay plausible. */
    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[1]) +
           sizeof(CM_INT13_DRIVE_PARAMETER) * DiskCount;
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (!PartialResourceList)
        return;
    RtlZeroMemory(PartialResourceList, Size);
    PartialResourceList->Version = 1;
    PartialResourceList->Revision = 1;
    PartialResourceList->Count = 1;
    PartialResourceList->PartialDescriptors[0].Type = CmResourceTypeDeviceSpecific;
    PartialResourceList->PartialDescriptors[0].u.DeviceSpecificData.DataSize =
        sizeof(CM_INT13_DRIVE_PARAMETER) * DiskCount;
    Int13Drives = (PCM_INT13_DRIVE_PARAMETER)&PartialResourceList->PartialDescriptors[1];
    for (i = 0; i < DiskCount; i++)
    {
        Int13Drives[i].DriveSelect = FIRST_BIOS_DISK + i;
        Int13Drives[i].MaxCylinders = 1023;
        Int13Drives[i].SectorsPerTrack = 63;
        Int13Drives[i].MaxHeads = 254;
        Int13Drives[i].NumberDrives = DiskCount;
    }
    FldrSetConfigurationData(SystemKey, PartialResourceList, Size);

    for (i = 0; i < DiskCount; i++)
    {
        Identifier = UefiGetHarddiskIdentifier(FIRST_BIOS_DISK + i);

        Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[1]) + sizeof(*DiskGeometry);
        PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
        if (!PartialResourceList)
            return;
        RtlZeroMemory(PartialResourceList, Size);
        PartialResourceList->Version = 1;
        PartialResourceList->Revision = 1;
        PartialResourceList->Count = 1;
        PartialResourceList->PartialDescriptors[0].Type = CmResourceTypeDeviceSpecific;
        PartialResourceList->PartialDescriptors[0].u.DeviceSpecificData.DataSize = sizeof(*DiskGeometry);
        DiskGeometry = (PCM_DISK_GEOMETRY_DEVICE_DATA)&PartialResourceList->PartialDescriptors[1];
        if (UefiDiskGetDriveGeometry(FIRST_BIOS_DISK + i, &Geometry))
        {
            Cylinders = Geometry.Sectors / (63 * 255);
            DiskGeometry->BytesPerSector = Geometry.BytesPerSector;
            DiskGeometry->NumberOfCylinders = (ULONG)min(Cylinders, 0xFFFFFFFF);
            DiskGeometry->SectorsPerTrack = 63;
            DiskGeometry->NumberOfHeads = 255;
        }

        FldrCreateComponentKey(ControllerKey,
                               PeripheralClass,
                               DiskPeripheral,
                               Output | Input,
                               i,
                               0xFFFFFFFF,
                               Identifier ? Identifier : "",
                               PartialResourceList,
                               Size,
                               &DiskKey);
    }
}

static
VOID
DetectInternal(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCONFIGURATION_COMPONENT_DATA BusKey;
    ULONG Size;

    /* Set 'Configuration Data' value */
    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (PartialResourceList == NULL)
    {
        ERR("Failed to allocate resource descriptor\n");
        return;
    }

    /* Initialize resource descriptor */
    RtlZeroMemory(PartialResourceList, Size);
    PartialResourceList->Version  = 1;
    PartialResourceList->Revision = 1;
    PartialResourceList->Count = 0;

    /* Create new bus key */
    FldrCreateComponentKey(SystemKey,
                           AdapterClass,
                           MultiFunctionAdapter,
                           0,
                           0,
                           0xFFFFFFFF,
                           "UEFI Internal",
                           PartialResourceList,
                           Size,
                           &BusKey);

    /* Increment bus number */
    (*BusNumber)++;

    /* Detect devices that do not belong to "standard" buses */
    DetectDisplayController(BusKey);
    DetectFirmwareDisks(SystemKey, BusKey);

    /* FIXME: Detect more devices */
}

#if defined(_M_IX86) || defined(_M_AMD64)
/*
 * UEFI has no PCI BIOS to ask, so probe configuration mechanism #1
 * directly (every x86 chipset since the PCI 2.0 era implements it).
 * The HAL needs this "PCI" adapter entry to serve HalGetBusData(),
 * which legacy drivers (scsiport miniports, video) use to find devices.
 */
static
ULONG
UefiPciReadConfig(
    _In_ ULONG Bus,
    _In_ ULONG Device,
    _In_ ULONG Function,
    _In_ ULONG Offset)
{
    WRITE_PORT_ULONG((PULONG)0xCF8,
                     0x80000000 | (Bus << 16) | (Device << 11) | (Function << 8) | (Offset & 0xFC));
    return READ_PORT_ULONG((PULONG)0xCFC);
}

static
BOOLEAN
UefiDetectPciBus(
    _In_ PCONFIGURATION_COMPONENT_DATA SystemKey,
    _Inout_ PULONG BusNumber,
    _Out_ PPCI_REGISTRY_INFO BusData)
{
    ULONG Saved, Bus, Device, LastBus = 0;

    UNREFERENCED_PARAMETER(SystemKey);
    UNREFERENCED_PARAMETER(BusNumber);

    /* The address port must hold what was written to it */
    Saved = READ_PORT_ULONG((PULONG)0xCF8);
    WRITE_PORT_ULONG((PULONG)0xCF8, 0x80000000);
    if (READ_PORT_ULONG((PULONG)0xCF8) != 0x80000000)
    {
        WRITE_PORT_ULONG((PULONG)0xCF8, Saved);
        WARN("No PCI configuration mechanism #1\n");
        return FALSE;
    }

    /* The highest bus number that has a device on it */
    for (Bus = 0; Bus < 256; ++Bus)
    {
        for (Device = 0; Device < 32; ++Device)
        {
            if ((UefiPciReadConfig(Bus, Device, 0, 0) & 0xFFFF) != 0xFFFF)
            {
                LastBus = Bus;
                break;
            }
        }
    }
    WRITE_PORT_ULONG((PULONG)0xCF8, Saved);

    BusData->MajorRevision = 2;
    BusData->MinorRevision = 0x10;
    BusData->NoBuses = (UCHAR)min(LastBus + 1, 0xFF);
    BusData->HardwareMechanism = 1;
    TRACE("PCI: mechanism #1, %u buses\n", BusData->NoBuses);
    return TRUE;
}
#endif

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(
    _In_opt_ PCSTR Options)
{
    PCONFIGURATION_COMPONENT_DATA SystemKey;
    ULONG BusNumber = 0;

    TRACE("DetectHardware()\n");

    /* Create the 'System' key */
#if defined(_M_IX86) || defined(_M_AMD64)
    FldrCreateSystemKey(&SystemKey, "AT/AT COMPATIBLE");
#elif defined(_M_IA64)
    FldrCreateSystemKey(&SystemKey, "Intel Itanium processor family");
#elif defined(_M_ARM) || defined(_M_ARM64)
    FldrCreateSystemKey(&SystemKey, "ARM processor family");
#else
    #error Please define a system key for your architecture
#endif

    /* Detect buses */
    DetectInternal(SystemKey, &BusNumber);
#if defined(_M_IX86) || defined(_M_AMD64)
    DetectPciBus(SystemKey, &BusNumber, UefiDetectPciBus);
#endif
    DetectAcpiBios(SystemKey, &BusNumber);

    TRACE("DetectHardware() Done\n");
    return SystemKey;
}
