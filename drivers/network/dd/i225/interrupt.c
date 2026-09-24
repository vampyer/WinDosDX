/*
 * PROJECT:     WinDosDX Intel I225 (Foxville) 2.5GbE Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Interrupt handlers
 */

#include "nic.h"

#include <debug.h>

VOID
NTAPI
MiniportISR(
    OUT PBOOLEAN InterruptRecognized,
    OUT PBOOLEAN QueueMiniportHandleInterrupt,
    IN NDIS_HANDLE MiniportAdapterContext)
{
    ULONG Value;
    PI225_ADAPTER Adapter = (PI225_ADAPTER)MiniportAdapterContext;

    /* Reading ICR acknowledges the interrupts (RC/W1C, Section 8.8.7) */
    I225ReadUlong(Adapter, I225_REG_ICR, &Value);

    Value &= Adapter->InterruptMask;
    _InterlockedOr(&Adapter->InterruptPending, Value);

    if (Value)
    {
        *InterruptRecognized = TRUE;
        *QueueMiniportHandleInterrupt = TRUE;
    }
    else
    {
        *InterruptRecognized = FALSE;
        *QueueMiniportHandleInterrupt = FALSE;
    }
}

VOID
NTAPI
MiniportHandleInterrupt(
    IN NDIS_HANDLE MiniportAdapterContext)
{
    ULONG InterruptPending;
    PI225_ADAPTER Adapter = (PI225_ADAPTER)MiniportAdapterContext;
    volatile PI225_TRANSMIT_DESCRIPTOR TransmitDescriptor;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    InterruptPending = _InterlockedExchange(&Adapter->InterruptPending, 0);

    /* Link State Changed */
    if (InterruptPending & I225_IMS_LSC)
    {
        ULONG Status;

        InterruptPending &= ~I225_IMS_LSC;
        NDIS_DbgPrint(MAX_TRACE, ("Link status changed!.\n"));

        NICUpdateLinkStatus(Adapter);

        Status = Adapter->MediaState == NdisMediaStateConnected ? NDIS_STATUS_MEDIA_CONNECT : NDIS_STATUS_MEDIA_DISCONNECT;

        NdisMIndicateStatus(Adapter->AdapterHandle, Status, NULL, 0);
        NdisMIndicateStatusComplete(Adapter->AdapterHandle);
    }

    /* Handling receive interrupts */
    if (InterruptPending & (I225_IMS_RXDMT0 | I225_IMS_RXDW))
    {
        volatile PI225_RECEIVE_DESCRIPTOR ReceiveDescriptor;
        PETH_HEADER EthHeader;
        ULONG BufferOffset;
        BOOLEAN bGotAny = FALSE;
        ULONG RxDescHead, RxDescTail, CurrRxDesc;

        InterruptPending &= ~(I225_IMS_RXDMT0 | I225_IMS_RXDW);

        I225ReadUlong(Adapter, I225_REG_RDH(0), &RxDescHead);
        I225ReadUlong(Adapter, I225_REG_RDT(0), &RxDescTail);

        while (((RxDescTail + 1) % NUM_RECEIVE_DESCRIPTORS) != RxDescHead)
        {
            CurrRxDesc = (RxDescTail + 1) % NUM_RECEIVE_DESCRIPTORS;
            BufferOffset = CurrRxDesc * Adapter->ReceiveBufferEntrySize;
            ReceiveDescriptor = Adapter->ReceiveDescriptors + CurrRxDesc;

            /* Check if hardware has released this descriptor (DD) */
            if (!(ReceiveDescriptor->Status & I225_RDESC_STATUS_DD))
            {
                break;
            }

            ReceiveDescriptor->Status &= ~(I225_RDESC_STATUS_PIF);

            if (!Adapter->PacketFilter)
            {
                goto NextReceiveDescriptor;
            }

            if (ReceiveDescriptor->Length != 0 && ReceiveDescriptor->Address != 0)
            {
                EthHeader = (PETH_HEADER)(Adapter->ReceiveBuffer + BufferOffset);

                NdisMEthIndicateReceive(Adapter->AdapterHandle,
                                        NULL,
                                        (PCHAR)EthHeader,
                                        sizeof(ETH_HEADER),
                                        (PCHAR)(EthHeader + 1),
                                        ReceiveDescriptor->Length - sizeof(ETH_HEADER),
                                        ReceiveDescriptor->Length - sizeof(ETH_HEADER));

                bGotAny = TRUE;
            }
            else
            {
                NDIS_DbgPrint(MIN_TRACE, ("Got a NULL descriptor"));
            }

NextReceiveDescriptor:
            ReceiveDescriptor->Status = 0;

            RxDescTail = CurrRxDesc;
        }

        if (bGotAny)
        {
            I225WriteUlong(Adapter, I225_REG_RDT(0), RxDescTail);

            NDIS_DbgPrint(MAX_TRACE, ("Rx done (RDH: %u, RDT: %u)\n", RxDescHead, RxDescTail));

            NdisMEthIndicateReceiveComplete(Adapter->AdapterHandle);
        }
    }

    /* Handling transmit interrupts */
    if (InterruptPending & I225_IMS_TXDW)
    {
        PNDIS_PACKET AckPackets[40] = {0};
        ULONG NumPackets = 0, i;

        InterruptPending &= ~I225_IMS_TXDW;

        while ((Adapter->TxFull || Adapter->LastTxDesc != Adapter->CurrentTxDesc) && NumPackets < ARRAYSIZE(AckPackets))
        {
            TransmitDescriptor = Adapter->TransmitDescriptors + Adapter->LastTxDesc;

            if (TransmitDescriptor->Status & I225_TDESC_STATUS_DD)
            {
                if (Adapter->TransmitPackets[Adapter->LastTxDesc])
                {
                    AckPackets[NumPackets++] = Adapter->TransmitPackets[Adapter->LastTxDesc];
                    Adapter->TransmitPackets[Adapter->LastTxDesc] = NULL;
                    TransmitDescriptor->Status = 0;
                }

                Adapter->LastTxDesc = (Adapter->LastTxDesc + 1) % NUM_TRANSMIT_DESCRIPTORS;
                Adapter->TxFull = FALSE;
            }
            else
            {
                break;
            }
        }

        if (NumPackets)
        {
            NDIS_DbgPrint(MAX_TRACE, ("Tx: (TDH: %u, TDT: %u)\n", Adapter->CurrentTxDesc, Adapter->LastTxDesc));
            NDIS_DbgPrint(MAX_TRACE, ("Tx Done: %u packets to ack\n", NumPackets));

            for (i = 0; i < NumPackets; ++i)
            {
                NdisMSendComplete(Adapter->AdapterHandle, AckPackets[i], NDIS_STATUS_SUCCESS);
            }
        }
    }

    ASSERT(InterruptPending == 0);
}
