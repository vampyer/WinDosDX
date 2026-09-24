/*
 * PROJECT:     WinDosDX Intel I225 (Foxville) 2.5GbE Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Driver entrypoint
 */

#include "nic.h"

#include <debug.h>

ULONG DebugTraceLevel = MIN_TRACE;

NDIS_STATUS
NTAPI
MiniportReset(
    OUT PBOOLEAN AddressingReset,
    IN NDIS_HANDLE MiniportAdapterContext)
{
    PI225_ADAPTER Adapter = (PI225_ADAPTER)MiniportAdapterContext;
    NDIS_STATUS Status;

    *AddressingReset = FALSE;

    NICDisableTxRx(Adapter);

    Status = NICSoftReset(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        return Status;
    }

    NICUpdateMulticastList(Adapter);
    NICUpdateLinkStatus(Adapter);

    Adapter->InterruptMask = DEFAULT_INTERRUPT_MASK;
    NICApplyInterruptMask(Adapter);

    return NICEnableTxRx(Adapter);
}

VOID
NTAPI
MiniportHalt(
    IN NDIS_HANDLE MiniportAdapterContext)
{
    PI225_ADAPTER Adapter = (PI225_ADAPTER)MiniportAdapterContext;

    ASSERT(Adapter != NULL);

    /* MiniportInitialize's failure path calls this too, possibly before
     * the CSR BAR was mapped - only touch hardware if it was. */
    if (Adapter->IoBase)
    {
        ULONG Value;

        NICDisableTxRx(Adapter);

        /* Mask interrupts before deregistering the ISR so a level-triggered
         * line (e.g. a late link change) can't be left asserted with no
         * handler on a shared IRQ. */
        NICDisableInterrupts(Adapter);

        I225ReadUlong(Adapter, I225_REG_CTRL_EXT, &Value);
        I225WriteUlong(Adapter, I225_REG_CTRL_EXT, Value & ~I225_CTRL_EXT_DRV_LOAD);
    }

    NICUnregisterInterrupts(Adapter);
    NICReleaseIoResources(Adapter);

    NdisFreeMemory(Adapter, sizeof(*Adapter), 0);
}

NDIS_STATUS
NTAPI
MiniportInitialize(
    OUT PNDIS_STATUS OpenErrorStatus,
    OUT PUINT SelectedMediumIndex,
    IN PNDIS_MEDIUM MediumArray,
    IN UINT MediumArraySize,
    IN NDIS_HANDLE MiniportAdapterHandle,
    IN NDIS_HANDLE WrapperConfigurationContext)
{
    PI225_ADAPTER Adapter;
    NDIS_STATUS Status;
    UINT i;
    PNDIS_RESOURCE_LIST ResourceList;
    UINT ResourceListSize;
    PCI_COMMON_CONFIG PciConfig;

    for (i = 0; i < MediumArraySize; i++)
    {
        if (MediumArray[i] == NdisMedium802_3)
        {
            *SelectedMediumIndex = i;
            break;
        }
    }

    if (i == MediumArraySize)
    {
        NDIS_DbgPrint(MIN_TRACE, ("802.3 medium was not found in the medium array\n"));
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    }

    ResourceList = NULL;
    ResourceListSize = 0;

    Status = NdisAllocateMemoryWithTag((PVOID*)&Adapter,
                                       sizeof(*Adapter),
                                       I225_TAG);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Failed to allocate adapter context (0x%x)\n", Status));
        return NDIS_STATUS_RESOURCES;
    }

    RtlZeroMemory(Adapter, sizeof(*Adapter));
    Adapter->AdapterHandle = MiniportAdapterHandle;

    NdisMSetAttributesEx(MiniportAdapterHandle,
                         Adapter,
                         0,
                         NDIS_ATTRIBUTE_BUS_MASTER,
                         NdisInterfacePci);

    NdisReadPciSlotInformation(Adapter->AdapterHandle,
                               0,
                               FIELD_OFFSET(PCI_COMMON_CONFIG, VendorID),
                               &PciConfig, sizeof(PciConfig));

    Adapter->VendorID = PciConfig.VendorID;
    Adapter->DeviceID = PciConfig.DeviceID;

    Adapter->SubsystemID = PciConfig.u.type0.SubSystemID;
    Adapter->SubsystemVendorID = PciConfig.u.type0.SubVendorID;

    if (!NICRecognizeHardware(Adapter))
    {
        NDIS_DbgPrint(MIN_TRACE, ("Hardware not recognized\n"));
        Status = NDIS_STATUS_UNSUPPORTED_MEDIA;
        goto Cleanup;
    }

    NdisMQueryAdapterResources(&Status,
                               WrapperConfigurationContext,
                               ResourceList,
                               &ResourceListSize);
    if (Status != NDIS_STATUS_RESOURCES)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unexpected failure of NdisMQueryAdapterResources (0x%x)\n", Status));
        Status = NDIS_STATUS_FAILURE;
        goto Cleanup;
    }

    Status = NdisAllocateMemoryWithTag((PVOID*)&ResourceList,
                                ResourceListSize,
                                I225_TAG);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Failed to allocate resource list (0x%x)\n", Status));
        goto Cleanup;
    }

    NdisMQueryAdapterResources(&Status,
                               WrapperConfigurationContext,
                               ResourceList,
                               &ResourceListSize);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unexpected failure of NdisMQueryAdapterResources (0x%x)\n", Status));
        goto Cleanup;
    }

    ASSERT(ResourceList->Version == 1);
    ASSERT(ResourceList->Revision == 1);

    Status = NICInitializeAdapterResources(Adapter, ResourceList);

    NdisFreeMemory(ResourceList, ResourceListSize, 0);
    ResourceList = NULL;

    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Adapter didn't receive enough resources\n"));
        goto Cleanup;
    }

    Status = NdisMInitializeScatterGatherDma(MiniportAdapterHandle,
                                             FALSE,
                                             MAXIMUM_FRAME_SIZE);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to configure DMA\n"));
        Status = NDIS_STATUS_RESOURCES;
        goto Cleanup;
    }

    Status = NICAllocateIoResources(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to allocate resources\n"));
        Status = NDIS_STATUS_RESOURCES;
        goto Cleanup;
    }

    /* Section 4.7.3 (p117): disable interrupts -> software reset ->
     * disable interrupts again -> PHY/link setup -> stats -> Rx -> Tx ->
     * enable interrupts. NICSoftReset covers the first steps. */
    Status = NICSoftReset(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to reset the NIC (0x%x)\n", Status));
        goto Cleanup;
    }

    Status = NICGetPermanentMacAddress(Adapter, Adapter->PermanentMacAddress);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to get the fixed MAC address (0x%x)\n", Status));
        goto Cleanup;
    }

    RtlCopyMemory(Adapter->MulticastList[0].MacAddress, Adapter->PermanentMacAddress, IEEE_802_ADDR_LENGTH);

    NICUpdateMulticastList(Adapter);

    NICUpdateLinkStatus(Adapter);

    Status = NICRegisterInterrupts(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to register interrupt (0x%x)\n", Status));
        goto Cleanup;
    }

    Adapter->InterruptMask = DEFAULT_INTERRUPT_MASK;
    NICApplyInterruptMask(Adapter);

    Status = NICEnableTxRx(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to enable TX and RX (0x%x)\n", Status));
        goto Cleanup;
    }

    return NDIS_STATUS_SUCCESS;

Cleanup:
    if (ResourceList != NULL)
    {
        NdisFreeMemory(ResourceList, ResourceListSize, 0);
    }

    MiniportHalt(Adapter);

    return Status;
}

NTSTATUS
NTAPI
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    NDIS_HANDLE WrapperHandle;
    NDIS_MINIPORT_CHARACTERISTICS Characteristics = { 0 };
    NDIS_STATUS Status;

    Characteristics.MajorNdisVersion = NDIS_MINIPORT_MAJOR_VERSION;
    Characteristics.MinorNdisVersion = NDIS_MINIPORT_MINOR_VERSION;
    Characteristics.CheckForHangHandler = NULL;
    Characteristics.DisableInterruptHandler = NULL;
    Characteristics.EnableInterruptHandler = NULL;
    Characteristics.HaltHandler = MiniportHalt;
    Characteristics.HandleInterruptHandler = MiniportHandleInterrupt;
    Characteristics.InitializeHandler = MiniportInitialize;
    Characteristics.ISRHandler = MiniportISR;
    Characteristics.QueryInformationHandler = MiniportQueryInformation;
    Characteristics.ReconfigureHandler = NULL;
    Characteristics.ResetHandler = MiniportReset;
    Characteristics.SendHandler = MiniportSend;
    Characteristics.SetInformationHandler = MiniportSetInformation;
    Characteristics.TransferDataHandler = NULL;
    Characteristics.ReturnPacketHandler = NULL;
    Characteristics.SendPacketsHandler = NULL;
    Characteristics.AllocateCompleteHandler = NULL;

    NdisMInitializeWrapper(&WrapperHandle, DriverObject, RegistryPath, NULL);
    if (!WrapperHandle)
    {
        return NDIS_STATUS_FAILURE;
    }

    Status = NdisMRegisterMiniport(WrapperHandle, &Characteristics, sizeof(Characteristics));
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NdisTerminateWrapper(WrapperHandle, 0);
        return NDIS_STATUS_FAILURE;
    }

    return NDIS_STATUS_SUCCESS;
}
