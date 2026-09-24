/*
 * PROJECT:     WinDosDX Intel I225 (Foxville) 2.5GbE Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Hardware specific functions
 */

#ifndef _I225_PCH_
#define _I225_PCH_

#include <ndis.h>

#include "i225hw.h"

#define I225_TAG '522i'

#define MAXIMUM_FRAME_SIZE   1522
#define RECEIVE_BUFFER_SIZE  2048

#define DRIVER_VERSION 1

#define DEFAULT_INTERRUPT_MASK  (I225_IMS_LSC | I225_IMS_TXDW | I225_IMS_TXQE | I225_IMS_RXDMT0 | I225_IMS_RXT0 | I225_IMS_TXD_LOW)


typedef struct _I225_ADAPTER
{
    /* NIC Memory */
    volatile PUCHAR IoBase;
    NDIS_PHYSICAL_ADDRESS IoAddress;
    ULONG IoLength;

    NDIS_HANDLE AdapterHandle;
    USHORT VendorID;
    USHORT DeviceID;
    USHORT SubsystemID;
    USHORT SubsystemVendorID;

    UCHAR PermanentMacAddress[IEEE_802_ADDR_LENGTH];

    struct {
        UCHAR MacAddress[IEEE_802_ADDR_LENGTH];
    } MulticastList[MAXIMUM_MULTICAST_ADDRESSES];
    ULONG MulticastListSize;

    ULONG LinkSpeedMbps;
    ULONG MediaState;
    ULONG PacketFilter;

    /* Interrupt */
    ULONG InterruptVector;
    ULONG InterruptLevel;
    BOOLEAN InterruptShared;
    ULONG InterruptFlags;

    NDIS_MINIPORT_INTERRUPT Interrupt;
    BOOLEAN InterruptRegistered;

    LONG InterruptMask;

    _Interlocked_
    volatile LONG InterruptPending;

    /* Transmit (queue 0 only - this driver does not yet use multiple Tx/Rx
     * queues, though the chip supports 4) */
    PI225_TRANSMIT_DESCRIPTOR TransmitDescriptors;
    NDIS_PHYSICAL_ADDRESS TransmitDescriptorsPa;

    PNDIS_PACKET TransmitPackets[NUM_TRANSMIT_DESCRIPTORS];

    ULONG CurrentTxDesc;
    ULONG LastTxDesc;
    BOOLEAN TxFull;

    /* Receive (queue 0 only) */
    PI225_RECEIVE_DESCRIPTOR ReceiveDescriptors;
    NDIS_PHYSICAL_ADDRESS ReceiveDescriptorsPa;

    I225_RCVBUF_SIZE ReceiveBufferType;
    volatile PUCHAR ReceiveBuffer;
    NDIS_PHYSICAL_ADDRESS ReceiveBufferPa;
    ULONG ReceiveBufferEntrySize;

} I225_ADAPTER, *PI225_ADAPTER;


BOOLEAN
NTAPI
NICRecognizeHardware(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICInitializeAdapterResources(
    IN PI225_ADAPTER Adapter,
    IN PNDIS_RESOURCE_LIST ResourceList);

NDIS_STATUS
NTAPI
NICAllocateIoResources(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICRegisterInterrupts(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICUnregisterInterrupts(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICReleaseIoResources(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICSoftReset(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICEnableTxRx(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICDisableTxRx(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICGetPermanentMacAddress(
    IN PI225_ADAPTER Adapter,
    OUT PUCHAR MacAddress);

NDIS_STATUS
NTAPI
NICUpdateMulticastList(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
NICApplyPacketFilter(
    IN PI225_ADAPTER Adapter);

VOID
NTAPI
NICUpdateLinkStatus(
    IN PI225_ADAPTER Adapter);

NDIS_STATUS
NTAPI
MiniportSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_PACKET Packet,
    _In_ UINT Flags);

NDIS_STATUS
NTAPI
MiniportSetInformation(
    IN NDIS_HANDLE MiniportAdapterContext,
    IN NDIS_OID Oid,
    IN PVOID InformationBuffer,
    IN ULONG InformationBufferLength,
    OUT PULONG BytesRead,
    OUT PULONG BytesNeeded);

NDIS_STATUS
NTAPI
MiniportQueryInformation(
    IN NDIS_HANDLE MiniportAdapterContext,
    IN NDIS_OID Oid,
    IN PVOID InformationBuffer,
    IN ULONG InformationBufferLength,
    OUT PULONG BytesWritten,
    OUT PULONG BytesNeeded);

VOID
NTAPI
MiniportISR(
    OUT PBOOLEAN InterruptRecognized,
    OUT PBOOLEAN QueueMiniportHandleInterrupt,
    IN NDIS_HANDLE MiniportAdapterContext);

VOID
NTAPI
MiniportHandleInterrupt(
    IN NDIS_HANDLE MiniportAdapterContext);

FORCEINLINE
VOID
I225ReadUlong(
    _In_ PI225_ADAPTER Adapter,
    _In_ ULONG Address,
    _Out_ PULONG Value)
{
    NdisReadRegisterUlong((PULONG)(Adapter->IoBase + Address), Value);
}

FORCEINLINE
VOID
I225WriteUlong(
    _In_ PI225_ADAPTER Adapter,
    _In_ ULONG Address,
    _In_ ULONG Value)
{
    NdisWriteRegisterUlong((PULONG)(Adapter->IoBase + Address), Value);
}

FORCEINLINE
VOID
NICApplyInterruptMask(
    _In_ PI225_ADAPTER Adapter)
{
    I225WriteUlong(Adapter, I225_REG_IMS, Adapter->InterruptMask);
}

FORCEINLINE
VOID
NICDisableInterrupts(
    _In_ PI225_ADAPTER Adapter)
{
    /* Section 4.7.4 (p118-119): drivers disable interrupts via EIMC during
     * init as well as the classic IMC - write both to be safe. */
    I225WriteUlong(Adapter, I225_REG_EIMC, ~0);
    I225WriteUlong(Adapter, I225_REG_IMC, ~0);
}

#endif /* _I225_PCH_ */
