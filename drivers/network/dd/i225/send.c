/*
 * PROJECT:     WinDosDX Intel I225 (Foxville) 2.5GbE Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Sending packets
 */

#include "nic.h"

#include <debug.h>

static
NDIS_STATUS
NICTransmitPacket(
    _In_ PI225_ADAPTER Adapter,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ ULONG Length)
{
    volatile PI225_TRANSMIT_DESCRIPTOR TransmitDescriptor;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    TransmitDescriptor = Adapter->TransmitDescriptors + Adapter->CurrentTxDesc;
    TransmitDescriptor->Address = PhysicalAddress.QuadPart;
    TransmitDescriptor->Length = (USHORT)Length;
    TransmitDescriptor->ChecksumOffset = 0;
    TransmitDescriptor->Command = I225_TDESC_CMD_RS | I225_TDESC_CMD_IFCS |
                                  I225_TDESC_CMD_EOP | I225_TDESC_CMD_IDE;
    TransmitDescriptor->Status = 0;
    TransmitDescriptor->ChecksumStartField = 0;
    TransmitDescriptor->Special = 0;

    Adapter->CurrentTxDesc = (Adapter->CurrentTxDesc + 1) % NUM_TRANSMIT_DESCRIPTORS;

    I225WriteUlong(Adapter, I225_REG_TDT(0), Adapter->CurrentTxDesc);

    if (Adapter->CurrentTxDesc == Adapter->LastTxDesc)
    {
        NDIS_DbgPrint(MID_TRACE, ("All TX descriptors are full now\n"));
        Adapter->TxFull = TRUE;
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
MiniportSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_PACKET Packet,
    _In_ UINT Flags)
{
    PI225_ADAPTER Adapter = (PI225_ADAPTER)MiniportAdapterContext;
    PSCATTER_GATHER_LIST sgList;
    ULONG TransmitLength;
    PHYSICAL_ADDRESS TransmitBuffer;
    NDIS_STATUS Status;

    sgList = NDIS_PER_PACKET_INFO_FROM_PACKET(Packet, ScatterGatherListPacketInfo);

    ASSERT(sgList != NULL);
    ASSERT(sgList->NumberOfElements == 1);
    ASSERT(sgList->Elements[0].Length <= MAXIMUM_FRAME_SIZE);

    if (Adapter->TxFull)
    {
        NDIS_DbgPrint(MIN_TRACE, ("All TX descriptors are full\n"));
        return NDIS_STATUS_RESOURCES;
    }

    TransmitLength = sgList->Elements[0].Length;
    TransmitBuffer = sgList->Elements[0].Address;
    Adapter->TransmitPackets[Adapter->CurrentTxDesc] = Packet;

    Status = NICTransmitPacket(Adapter, TransmitBuffer, TransmitLength);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Transmit packet failed\n"));
        return Status;
    }

    return NDIS_STATUS_PENDING;
}
