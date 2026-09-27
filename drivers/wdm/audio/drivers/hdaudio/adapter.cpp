/*
 * PROJECT:     WinDosDX HD Audio codec driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry, device start, subdevice registration
 */

#include <initguid.h>
#include "hdacodec.h"

#define NDEBUG
#include <debug.h>

typedef NTSTATUS (*PFN_CREATE_MINIPORT)(PUNKNOWN *, REFCLSID, PUNKNOWN, POOL_TYPE);

/* Create a port and its miniport, and register the pair as a subdevice. */
static NTSTATUS
InstallSubdevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PCWSTR Name, REFCLSID PortClassId,
                 PFN_CREATE_MINIPORT CreateMiniport, PUNKNOWN UnknownAdapter,
                 PRESOURCELIST ResourceList, PUNKNOWN *OutPort)
{
    PPORT Port = NULL;
    PUNKNOWN Miniport = NULL;
    NTSTATUS Status;

    Status = PcNewPort(&Port, PortClassId);
    if (NT_SUCCESS(Status))
        Status = CreateMiniport(&Miniport, GUID_NULL, NULL, NonPagedPool);
    if (NT_SUCCESS(Status))
        Status = Port->Init(DeviceObject, Irp, Miniport, UnknownAdapter, ResourceList);
    if (NT_SUCCESS(Status))
        Status = PcRegisterSubdevice(DeviceObject, (PWCHAR)Name, Port);

    if (NT_SUCCESS(Status))
        *OutPort = Port;
    else if (Port)
        Port->Release();
    if (Miniport)
        Miniport->Release();
    return Status;
}

static NTSTATUS NTAPI
StartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList)
{
    CHdaCodec *Codec;
    PUNKNOWN WavePort = NULL, TopoPort = NULL;
    NTSTATUS Status;

    Codec = new (NonPagedPool, HDA_TAG) CHdaCodec();
    if (!Codec)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = Codec->Init(DeviceObject);
    if (NT_SUCCESS(Status))
    {
        Status = InstallSubdevice(DeviceObject, Irp, L"Topology", CLSID_PortTopology,
                                  CreateTopologyMiniport, Codec, ResourceList, &TopoPort);
    }
    if (NT_SUCCESS(Status))
    {
        Status = InstallSubdevice(DeviceObject, Irp, L"Wave", CLSID_PortWaveCyclic,
                                  CreateWaveMiniport, Codec, ResourceList, &WavePort);
    }
    if (NT_SUCCESS(Status))
    {
        /* The wave filter's analog output feeds the topology filter. */
        Status = PcRegisterPhysicalConnection(DeviceObject, WavePort, WAVE_PIN_BRIDGE,
                                              TopoPort, TOPO_PIN_WAVEIN);
    }

    if (WavePort)
        WavePort->Release();
    if (TopoPort)
        TopoPort->Release();
    /* The miniports hold their own references to the codec. */
    Codec->Release();

    if (!NT_SUCCESS(Status))
        DPRINT1("hdaudio: start failed (0x%lx)\n", Status);
    return Status;
}

static NTSTATUS NTAPI
AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    /* Two subdevices: Wave and Topology. */
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, StartDevice, 2, 0);
}

extern "C" NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    return PcInitializeAdapterDriver(DriverObject, RegistryPath, (PDRIVER_ADD_DEVICE)AddDevice);
}
