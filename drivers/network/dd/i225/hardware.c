/*
 * PROJECT:     WinDosDX Intel I225 (Foxville) 2.5GbE Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Hardware specific functions
 *
 * The reset/init sequence here follows "Intel Ethernet Controller I225
 * Software User Manual" Rev 1.3, Section 4.7.3 "Initialization Sequence"
 * (p117) and its sub-sections 4.7.4-4.7.10 (p118-122):
 *   disable interrupts -> software reset -> disable interrupts again ->
 *   PHY/link setup -> init statistics -> init receive -> init transmit ->
 *   enable interrupts.
 */

#include "nic.h"

#include <debug.h>


static ULONG PacketFilterToMask(ULONG PacketFilter)
{
    ULONG FilterMask = 0;

    if (PacketFilter & NDIS_PACKET_TYPE_ALL_MULTICAST)
    {
        FilterMask |= I225_RCTL_MPE;
    }
    if (PacketFilter & NDIS_PACKET_TYPE_PROMISCUOUS)
    {
        FilterMask |= I225_RCTL_UPE;
        FilterMask |= I225_RCTL_MPE;
    }
    if (PacketFilter & NDIS_PACKET_TYPE_MAC_FRAME)
    {
        FilterMask |= I225_RCTL_PMCF;
    }
    if (PacketFilter & NDIS_PACKET_TYPE_BROADCAST)
    {
        FilterMask |= I225_RCTL_BAM;
    }

    return FilterMask;
}

BOOLEAN
NTAPI
NICRecognizeHardware(
    IN PI225_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    if (Adapter->VendorID != HW_VENDOR_INTEL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unknown vendor: 0x%x\n", Adapter->VendorID));
        return FALSE;
    }

    /* Only the I225-V has been confirmed against real hardware so far
     * (see i225hw.h) - other Foxville SKUs are not added until verified,
     * not guessed. */
    if (Adapter->DeviceID != I225_DEVICE_ID_V)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unknown device: 0x%x\n", Adapter->DeviceID));
        return FALSE;
    }

    return TRUE;
}

NDIS_STATUS
NTAPI
NICInitializeAdapterResources(
    IN PI225_ADAPTER Adapter,
    IN PNDIS_RESOURCE_LIST ResourceList)
{
    UINT n;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    for (n = 0; n < ResourceList->Count; n++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR ResourceDescriptor = ResourceList->PartialDescriptors + n;

        switch (ResourceDescriptor->Type)
        {
        case CmResourceTypeInterrupt:
            ASSERT(Adapter->InterruptVector == 0);
            ASSERT(Adapter->InterruptLevel == 0);

            Adapter->InterruptVector = ResourceDescriptor->u.Interrupt.Vector;
            Adapter->InterruptLevel = ResourceDescriptor->u.Interrupt.Level;
            Adapter->InterruptShared = (ResourceDescriptor->ShareDisposition == CmResourceShareShared);
            Adapter->InterruptFlags = ResourceDescriptor->Flags;

            NDIS_DbgPrint(MID_TRACE, ("IRQ vector is %d\n", Adapter->InterruptVector));
            break;

        case CmResourceTypeMemory:
            /* The CSR BAR is >= 128KB (larger if the NVM maps flash into
             * it); the MSI-X BAR is 16KB (Section 9.3.11). Select by size
             * rather than resource order, and map only the CSR window. */
            if (Adapter->IoAddress.QuadPart == 0 &&
                ResourceDescriptor->u.Memory.Length >= I225_CSR_SPACE_SIZE)
            {
                Adapter->IoAddress.QuadPart = ResourceDescriptor->u.Memory.Start.QuadPart;
                Adapter->IoLength = I225_CSR_SPACE_SIZE;
                NDIS_DbgPrint(MID_TRACE, ("CSR BAR at %I64x (length 0x%x, mapping 0x%x)\n",
                                          Adapter->IoAddress.QuadPart,
                                          ResourceDescriptor->u.Memory.Length,
                                          Adapter->IoLength));
            }
            break;

        default:
            NDIS_DbgPrint(MIN_TRACE, ("Unrecognized resource type: 0x%x\n", ResourceDescriptor->Type));
            break;
        }
    }

    if (Adapter->IoAddress.QuadPart == 0 || Adapter->InterruptVector == 0)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Adapter didn't receive enough resources\n"));
        return NDIS_STATUS_RESOURCES;
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICAllocateIoResources(
    IN PI225_ADAPTER Adapter)
{
    NDIS_STATUS Status;
    UINT n;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    Status = NdisMMapIoSpace((PVOID*)&Adapter->IoBase,
                             Adapter->AdapterHandle,
                             Adapter->IoAddress,
                             Adapter->IoLength);
    if (Status != NDIS_STATUS_SUCCESS || Adapter->IoBase == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to map IO space (0x%x)\n", Status));
        Adapter->IoBase = NULL;
        return NDIS_STATUS_RESOURCES;
    }

    NdisMAllocateSharedMemory(Adapter->AdapterHandle,
                              sizeof(I225_TRANSMIT_DESCRIPTOR) * NUM_TRANSMIT_DESCRIPTORS,
                              FALSE,
                              (PVOID*)&Adapter->TransmitDescriptors,
                              &Adapter->TransmitDescriptorsPa);
    if (Adapter->TransmitDescriptors == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to allocate transmit descriptors\n"));
        return NDIS_STATUS_RESOURCES;
    }

    for (n = 0; n < NUM_TRANSMIT_DESCRIPTORS; ++n)
    {
        PI225_TRANSMIT_DESCRIPTOR Descriptor = Adapter->TransmitDescriptors + n;
        Descriptor->Address = 0;
        Descriptor->Length = 0;
    }

    Adapter->ReceiveBufferType = I225_RCVBUF_2048;
    Adapter->ReceiveBufferEntrySize = RECEIVE_BUFFER_SIZE;

    NdisMAllocateSharedMemory(Adapter->AdapterHandle,
                              sizeof(I225_RECEIVE_DESCRIPTOR) * NUM_RECEIVE_DESCRIPTORS,
                              FALSE,
                              (PVOID*)&Adapter->ReceiveDescriptors,
                              &Adapter->ReceiveDescriptorsPa);
    if (Adapter->ReceiveDescriptors == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to allocate receive descriptors\n"));
        return NDIS_STATUS_RESOURCES;
    }

    NdisMAllocateSharedMemory(Adapter->AdapterHandle,
                              Adapter->ReceiveBufferEntrySize * NUM_RECEIVE_DESCRIPTORS,
                              FALSE,
                              (PVOID*)&Adapter->ReceiveBuffer,
                              &Adapter->ReceiveBufferPa);
    if (Adapter->ReceiveBuffer == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to allocate receive buffer\n"));
        return NDIS_STATUS_RESOURCES;
    }

    for (n = 0; n < NUM_RECEIVE_DESCRIPTORS; ++n)
    {
        PI225_RECEIVE_DESCRIPTOR Descriptor = Adapter->ReceiveDescriptors + n;

        RtlZeroMemory(Descriptor, sizeof(*Descriptor));
        Descriptor->Address = Adapter->ReceiveBufferPa.QuadPart + n * Adapter->ReceiveBufferEntrySize;
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICRegisterInterrupts(
    IN PI225_ADAPTER Adapter)
{
    NDIS_STATUS Status;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    Status = NdisMRegisterInterrupt(&Adapter->Interrupt,
                                    Adapter->AdapterHandle,
                                    Adapter->InterruptVector,
                                    Adapter->InterruptLevel,
                                    TRUE,
                                    Adapter->InterruptShared,
                                    (Adapter->InterruptFlags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                    NdisInterruptLatched : NdisInterruptLevelSensitive);

    if (Status == NDIS_STATUS_SUCCESS)
    {
        Adapter->InterruptRegistered = TRUE;
    }

    return Status;
}

NDIS_STATUS
NTAPI
NICUnregisterInterrupts(
    IN PI225_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    if (Adapter->InterruptRegistered)
    {
        NdisMDeregisterInterrupt(&Adapter->Interrupt);
        Adapter->InterruptRegistered = FALSE;
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICReleaseIoResources(
    IN PI225_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    if (Adapter->ReceiveDescriptors != NULL)
    {
        if (Adapter->IoBase)
        {
            I225WriteUlong(Adapter, I225_REG_RDH(0), 0);
            I225WriteUlong(Adapter, I225_REG_RDT(0), 0);
        }

        NdisMFreeSharedMemory(Adapter->AdapterHandle,
                              sizeof(I225_RECEIVE_DESCRIPTOR) * NUM_RECEIVE_DESCRIPTORS,
                              FALSE,
                              Adapter->ReceiveDescriptors,
                              Adapter->ReceiveDescriptorsPa);

        Adapter->ReceiveDescriptors = NULL;
    }

    if (Adapter->ReceiveBuffer != NULL)
    {
        NdisMFreeSharedMemory(Adapter->AdapterHandle,
                              Adapter->ReceiveBufferEntrySize * NUM_RECEIVE_DESCRIPTORS,
                              FALSE,
                              Adapter->ReceiveBuffer,
                              Adapter->ReceiveBufferPa);

        Adapter->ReceiveBuffer = NULL;
        Adapter->ReceiveBufferEntrySize = 0;
    }

    if (Adapter->TransmitDescriptors != NULL)
    {
        if (Adapter->IoBase)
        {
            I225WriteUlong(Adapter, I225_REG_TDH(0), 0);
            I225WriteUlong(Adapter, I225_REG_TDT(0), 0);
        }

        NdisMFreeSharedMemory(Adapter->AdapterHandle,
                              sizeof(I225_TRANSMIT_DESCRIPTOR) * NUM_TRANSMIT_DESCRIPTORS,
                              FALSE,
                              Adapter->TransmitDescriptors,
                              Adapter->TransmitDescriptorsPa);

        Adapter->TransmitDescriptors = NULL;
    }

    if (Adapter->IoBase)
    {
        NdisMUnmapIoSpace(Adapter->AdapterHandle, Adapter->IoBase, Adapter->IoLength);
    }

    return NDIS_STATUS_SUCCESS;
}

/* 100 x 100us = 10ms budget for pending master requests to drain */
#define MAX_MASTER_DISABLE_POLLS  100

/*
 * Section 4.7.4/4.7.5 (p118-119): disable interrupts, issue CTRL.DEV_RST,
 * disable interrupts again, then wait >=3ms (Section 4.3.1, p110) before
 * touching any other register and verify EEC.Auto_RD and STATUS.RST_DONE
 * are both set before considering the reset complete. Section 4.3.1 also
 * requires the master disable flow (Section 5.2.3.3, p131) before
 * DEV_RST, so the reset cannot race with DMA already in flight.
 */
NDIS_STATUS
NTAPI
NICSoftReset(
    IN PI225_ADAPTER Adapter)
{
    ULONG Value;
    UINT n;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    /* Section 7.3.3.11 (p310): GPIE selects the interrupt mode and "should
     * be set to the correct mode before accessing other interrupt control
     * registers". All-zero is "INT-x/MSI + Legacy" (Table 7-54). GPIE,
     * EIAM and EIMS are not reset by CTRL.DEV_RST, so set the whole legacy
     * configuration explicitly rather than trusting whatever state an
     * earlier owner (firmware, a previous driver load) left behind. */
    I225WriteUlong(Adapter, I225_REG_GPIE, 0);
    I225WriteUlong(Adapter, I225_REG_IAM, 0);
    I225WriteUlong(Adapter, I225_REG_EIAC, 0);
    I225WriteUlong(Adapter, I225_REG_EIAM, 0);

    NICDisableInterrupts(Adapter);

    /* Master disable: block new master requests, then wait for pending
     * ones to drain (STATUS.GIO_MASTER_EN clears). On timeout, reset
     * anyway - DEV_RST is itself the documented recovery for a stuck
     * device, and it clears GIO_MASTER_DISABLE again (Section 5.2.3.3). */
    I225ReadUlong(Adapter, I225_REG_CTRL, &Value);
    I225WriteUlong(Adapter, I225_REG_CTRL, Value | I225_CTRL_GIO_MASTER_DISABLE);

    for (n = 0; n < MAX_MASTER_DISABLE_POLLS; n++)
    {
        I225ReadUlong(Adapter, I225_REG_STATUS, &Value);
        if (!(Value & I225_STATUS_GIO_MASTER_EN))
            break;

        NdisStallExecution(100);
    }

    if (n == MAX_MASTER_DISABLE_POLLS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Master requests did not drain before reset, resetting anyway\n"));
    }

    I225ReadUlong(Adapter, I225_REG_CTRL, &Value);
    I225WriteUlong(Adapter, I225_REG_CTRL, Value | I225_CTRL_DEV_RST);

    /* CTRL.DEV_RST is self-clearing (SC); the datasheet directs waiting
     * >=3ms rather than polling the bit itself before the next access. */
    NdisStallExecution(3000);

    NICDisableInterrupts(Adapter);

    /* Clear out any interrupts (ICR clears on read) */
    I225ReadUlong(Adapter, I225_REG_ICR, &Value);

    for (n = 0; n < MAX_RESET_ATTEMPTS; n++)
    {
        ULONG Eec, Status;

        I225ReadUlong(Adapter, I225_REG_EEC, &Eec);
        I225ReadUlong(Adapter, I225_REG_STATUS, &Status);

        if ((Eec & I225_EEC_AUTO_RD) && (Status & I225_STATUS_RST_DONE))
        {
            NDIS_DbgPrint(MAX_TRACE, ("Device reset complete (%u)\n", n));
            break;
        }

        NdisStallExecution(1000);
    }

    if (n == MAX_RESET_ATTEMPTS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Device did not report reset completion (EEC.Auto_RD / STATUS.RST_DONE)\n"));
        return NDIS_STATUS_FAILURE;
    }

    /* Section 4.7.5 (p119): several CTRL bits need to be (re-)set after
     * reset for normal operation - SLU must be set by software to permit
     * the MAC to recognize the PHY's link indication. FD/SPEED are left
     * as auto-negotiated (don't-care per the datasheet when
     * FRCDPLX/FRCSPD are both 0, which is the reset default). */
    I225ReadUlong(Adapter, I225_REG_CTRL, &Value);
    Value &= ~I225_CTRL_VME;
    Value |= I225_CTRL_SLU;
    I225WriteUlong(Adapter, I225_REG_CTRL, Value);

    /* Section 8.2.3 (p378): DRV_LOAD "should be set by the software device
     * driver after it is loaded" - tells manageability firmware a host
     * driver owns the port. Cleared again in MiniportHalt. */
    I225ReadUlong(Adapter, I225_REG_CTRL_EXT, &Value);
    I225WriteUlong(Adapter, I225_REG_CTRL_EXT, Value | I225_CTRL_EXT_DRV_LOAD);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICEnableTxRx(
    IN PI225_ADAPTER Adapter)
{
    ULONG Value;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));
    NDIS_DbgPrint(MID_TRACE, ("Setting up transmit.\n"));

    /* Section 4.7.10 (p121-122): per-queue Tx init, then TCTL.EN last */
    I225WriteUlong(Adapter, I225_REG_TCTL, 0);

    I225WriteUlong(Adapter, I225_REG_TDBAH(0), Adapter->TransmitDescriptorsPa.HighPart);
    I225WriteUlong(Adapter, I225_REG_TDBAL(0), Adapter->TransmitDescriptorsPa.LowPart);
    I225WriteUlong(Adapter, I225_REG_TDLEN(0), sizeof(I225_TRANSMIT_DESCRIPTOR) * NUM_TRANSMIT_DESCRIPTORS);
    I225WriteUlong(Adapter, I225_REG_TDH(0), 0);
    I225WriteUlong(Adapter, I225_REG_TDT(0), 0);
    Adapter->CurrentTxDesc = 0;
    Adapter->LastTxDesc = 0;
    Adapter->TxFull = FALSE;

    /* Queue zero is enabled by default per Section 4.7.10, but write it
     * explicitly for clarity/robustness. Only ENABLE is set (confirmed
     * bit 25, Section 8.11.15) - the PTHRESH/HTHRESH/WTHRESH threshold
     * fields are left at their reset defaults (untuned but valid). */
    I225ReadUlong(Adapter, I225_REG_TXDCTL(0), &Value);
    I225WriteUlong(Adapter, I225_REG_TXDCTL(0), Value | I225_TXDCTL_ENABLE);

    I225WriteUlong(Adapter, I225_REG_TCTL, I225_TCTL_EN | I225_TCTL_PSP);
    I225WriteUlong(Adapter, I225_REG_TIPG, I225_TIPG_IPGT_DEF | I225_TIPG_IPGR1_DEF | I225_TIPG_IPGR2_DEF);

    NDIS_DbgPrint(MID_TRACE, ("Setting up receive.\n"));

    /* Section 4.7.9 (p120-121): leave RCTL.RXEN clear until the ring is
     * fully set up, then enable last */
    I225WriteUlong(Adapter, I225_REG_RCTL, 0);

    I225WriteUlong(Adapter, I225_REG_RDBAH(0), Adapter->ReceiveDescriptorsPa.HighPart);
    I225WriteUlong(Adapter, I225_REG_RDBAL(0), Adapter->ReceiveDescriptorsPa.LowPart);
    I225WriteUlong(Adapter, I225_REG_RDLEN(0), sizeof(I225_RECEIVE_DESCRIPTOR) * NUM_RECEIVE_DESCRIPTORS);
    I225WriteUlong(Adapter, I225_REG_RDH(0), 0);
    I225WriteUlong(Adapter, I225_REG_RDT(0), NUM_RECEIVE_DESCRIPTORS - 1);

    I225ReadUlong(Adapter, I225_REG_RXDCTL(0), &Value);
    I225WriteUlong(Adapter, I225_REG_RXDCTL(0), Value | I225_RXDCTL_ENABLE);

    /* RCTL: use legacy descriptor mode's generic BSIZE=2048 (00b, the
     * reset default, needs no BSIZE bits set) and strip the Ethernet CRC
     * (SECRC, confirmed bit 26, Section 8.9.1 p421) since NDIS callers
     * don't expect the FCS trailer in received frames. */
    Value = I225_RCTL_RXEN | I225_RCTL_DPF | I225_RCTL_SECRC;
    Value |= PacketFilterToMask(Adapter->PacketFilter);
    I225WriteUlong(Adapter, I225_REG_RCTL, Value);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICDisableTxRx(
    IN PI225_ADAPTER Adapter)
{
    ULONG Value;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    I225ReadUlong(Adapter, I225_REG_TCTL, &Value);
    Value &= ~I225_TCTL_EN;
    I225WriteUlong(Adapter, I225_REG_TCTL, Value);

    I225ReadUlong(Adapter, I225_REG_RCTL, &Value);
    Value &= ~I225_RCTL_RXEN;
    I225WriteUlong(Adapter, I225_REG_RCTL, Value);

    return NDIS_STATUS_SUCCESS;
}

/*
 * Reads the station MAC address from RAL0/RAH0 (Section 8.9.14/8.9.15,
 * p428) rather than doing a software NVM (EERD) read like the e1000
 * driver does - RAL0/RAH0 are auto-loaded by hardware from the NVM at
 * reset (Section 3.3.2.1 "Initialization from the Shadow RAM"), so by the
 * time NICSoftReset has completed, the primary station address is already
 * present in these registers.
 */
NDIS_STATUS
NTAPI
NICGetPermanentMacAddress(
    IN PI225_ADAPTER Adapter,
    OUT PUCHAR MacAddress)
{
    ULONG Ral, Rah;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    I225ReadUlong(Adapter, I225_REG_RAL + 0, &Ral);
    I225ReadUlong(Adapter, I225_REG_RAH + 0, &Rah);

    if (Ral == 0 && (Rah & ~I225_RAH_AV) == 0)
    {
        NDIS_DbgPrint(MIN_TRACE, ("RAL0/RAH0 are empty - NVM may not have auto-loaded a station address\n"));
        return NDIS_STATUS_FAILURE;
    }

    MacAddress[0] = (UCHAR)(Ral & 0xFF);
    MacAddress[1] = (UCHAR)((Ral >> 8) & 0xFF);
    MacAddress[2] = (UCHAR)((Ral >> 16) & 0xFF);
    MacAddress[3] = (UCHAR)((Ral >> 24) & 0xFF);
    MacAddress[4] = (UCHAR)(Rah & 0xFF);
    MacAddress[5] = (UCHAR)((Rah >> 8) & 0xFF);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICUpdateMulticastList(
    IN PI225_ADAPTER Adapter)
{
    UINT n;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    for (n = 0; n < MAXIMUM_MULTICAST_ADDRESSES; ++n)
    {
        ULONG Ral = *(ULONG *)Adapter->MulticastList[n].MacAddress;
        ULONG Rah = *(USHORT *)&Adapter->MulticastList[n].MacAddress[4];

        if (Rah || Ral)
        {
            Rah |= I225_RAH_AV;

            I225WriteUlong(Adapter, I225_REG_RAL + (8 * n), Ral);
            I225WriteUlong(Adapter, I225_REG_RAH + (8 * n), Rah);
        }
        else
        {
            I225WriteUlong(Adapter, I225_REG_RAH + (8 * n), 0);
            I225WriteUlong(Adapter, I225_REG_RAL + (8 * n), 0);
        }
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICApplyPacketFilter(
    IN PI225_ADAPTER Adapter)
{
    ULONG FilterMask;

    I225ReadUlong(Adapter, I225_REG_RCTL, &FilterMask);

    FilterMask &= ~I225_RCTL_FILTER_BITS;
    FilterMask |= PacketFilterToMask(Adapter->PacketFilter);
    I225WriteUlong(Adapter, I225_REG_RCTL, FilterMask);

    return NDIS_STATUS_SUCCESS;
}

/*
 * Section 8.2.2 (p375-376): link up, speed and the 2.5Gb/s flag are all
 * in STATUS (same place as e1000 for SPEED; Speed_2P5 is I225-specific).
 */
VOID
NTAPI
NICUpdateLinkStatus(
    IN PI225_ADAPTER Adapter)
{
    ULONG DeviceStatus;
    SIZE_T SpeedIndex;
    static ULONG SpeedValues[] = { 10, 100, 1000, 1000 };

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    I225ReadUlong(Adapter, I225_REG_STATUS, &DeviceStatus);

    Adapter->MediaState = (DeviceStatus & I225_STATUS_LU) ? NdisMediaStateConnected : NdisMediaStateDisconnected;

    if (DeviceStatus & I225_STATUS_SPEED_2P5)
    {
        Adapter->LinkSpeedMbps = 2500;
    }
    else
    {
        SpeedIndex = (DeviceStatus & I225_STATUS_SPEED_MASK) >> I225_STATUS_SPEED_SHIFT;
        Adapter->LinkSpeedMbps = SpeedValues[SpeedIndex];
    }
}
