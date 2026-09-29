/*
 * PROJECT:     WinDosDX xHCI (USB 3) host controller driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver structures
 *
 * A miniport of the USB port driver (usbport.sys), like usbehci. The port
 * driver's model is USB 2: it addresses devices with SET_ADDRESS and has
 * no notion of device slots. This driver keeps that model for the rest of
 * the stack and maps it onto xHCI:
 *  - the default pipe of a new device (address 0) gets a device slot
 *    (Enable Slot, Address Device with SET_ADDRESS blocked);
 *  - the SET_ADDRESS request becomes the Address Device command;
 *  - every connected device is reported to the port driver as high speed
 *    (no split transactions to schedule); the real speed goes into the slot.
 * The root port of a new device is the port the hub driver reset last.
 */

#pragma once

#include <ntddk.h>
#include <windef.h>
#include <stdio.h>
#include <hubbusif.h>
#include <usbbusif.h>
#include <usbdlib.h>
#include <drivers/usbport/usbmport.h>
#include "hardware.h"

#define XHCI_MAX_SLOTS          32
#define XHCI_MAX_SCRATCHPADS    64
#define XHCI_COMMAND_TRBS       64
#define XHCI_EVENT_TRBS         256
#define XHCI_RING_TRBS          256     /* per endpoint: 4 KB */
#define XHCI_MAX_TRANSFER       0x10000 /* the port driver splits bigger ones */
#define XHCI_CONTEXT_STRIDE     64      /* room for 64-byte contexts */

/* Everything the controller needs, in one DMA block from the port driver. */
typedef struct _XHCI_HC_RESOURCES
{
    DECLSPEC_ALIGN(4096) UCHAR Scratchpad[XHCI_MAX_SCRATCHPADS][4096];
    DECLSPEC_ALIGN(64) ULONG64 Dcbaa[256];
    DECLSPEC_ALIGN(64) ULONG64 ScratchpadArray[XHCI_MAX_SCRATCHPADS];
    DECLSPEC_ALIGN(64) XHCI_TRB CommandRing[XHCI_COMMAND_TRBS];
    DECLSPEC_ALIGN(64) XHCI_TRB EventRing[XHCI_EVENT_TRBS];
    DECLSPEC_ALIGN(64) XHCI_ERST_ENTRY Erst[1];
    DECLSPEC_ALIGN(64) UCHAR DeviceContext[XHCI_MAX_SLOTS][32 * XHCI_CONTEXT_STRIDE];
    DECLSPEC_ALIGN(64) UCHAR InputContext[XHCI_MAX_SLOTS][33 * XHCI_CONTEXT_STRIDE];
} XHCI_HC_RESOURCES, *PXHCI_HC_RESOURCES;

struct _XHCI_ENDPOINT;
struct _XHCI_TRANSFER;

typedef struct _XHCI_SLOT
{
    BOOLEAN InUse;
    BOOLEAN PendingFree;    /* default pipe closed: freed unless reopened */
    UCHAR Speed;            /* XHCI_SPEED_* */
    UCHAR RootPort;
    UCHAR UsbAddress;       /* the address the port driver uses */
    struct _XHCI_ENDPOINT *Endpoints[32];   /* by DCI */
} XHCI_SLOT, *PXHCI_SLOT;

typedef struct _XHCI_EXTENSION
{
    PUCHAR Base;
    PUCHAR Operational;
    PUCHAR Runtime;
    PULONG Doorbells;
    ULONG MaxSlots;
    ULONG MaxPorts;
    ULONG ContextSize;      /* 32 or 64 */
    ULONG ScratchpadCount;

    PXHCI_HC_RESOURCES Resources;
    ULONG ResourcesPA;

    KSPIN_LOCK Lock;        /* rings, events, slots */

    /* Command ring */
    ULONG CommandEnqueue;
    ULONG CommandCycle;
    BOOLEAN CommandBusy;
    BOOLEAN CommandDone;
    ULONG64 CommandTrbPA;
    ULONG CommandCompletion;
    ULONG CommandSlot;

    /* Event ring */
    ULONG EventDequeue;
    ULONG EventCycle;

    XHCI_SLOT Slots[XHCI_MAX_SLOTS + 1];    /* by slot id, from 1 */
    UCHAR AddressToSlot[128];

    USHORT LastResetPort;   /* root port of the next new device */
    BOOLEAN PortChanged;

    ULONG FrameHigh;
    ULONG LastFrame;
} XHCI_EXTENSION, *PXHCI_EXTENSION;

typedef struct _XHCI_ENDPOINT
{
    USBPORT_ENDPOINT_PROPERTIES Properties;
    UCHAR Slot;
    UCHAR Dci;
    BOOLEAN Halted;
    BOOLEAN NeedsPoll;
    ULONG State;            /* USBPORT_ENDPOINT_* */

    PXHCI_TRB Ring;
    ULONG RingPA;
    ULONG Enqueue;
    ULONG Cycle;
    ULONG TrbsInUse;

    /* Per ring TRB: the transfer it belongs to and where its data starts */
    struct _XHCI_TRANSFER *TrbOwner[XHCI_RING_TRBS];
    ULONG TrbOffset[XHCI_RING_TRBS];
    ULONG TrbLength[XHCI_RING_TRBS];

    LIST_ENTRY Transfers;   /* in submission order */
} XHCI_ENDPOINT, *PXHCI_ENDPOINT;

typedef struct _XHCI_TRANSFER
{
    LIST_ENTRY Link;
    PUSBPORT_TRANSFER_PARAMETERS Parameters;
    PUSBPORT_SCATTER_GATHER_LIST SgList;
    ULONG FirstTrb;
    ULONG LastTrb;          /* the TRB with IOC */
    ULONG TrbCount;
    ULONG Requested;
    ULONG Transferred;
    BOOLEAN ShortSeen;
    BOOLEAN IsControl;
    BOOLEAN Done;
    USBD_STATUS Status;
} XHCI_TRANSFER, *PXHCI_TRANSFER;

/* usbxhci.c */
BOOLEAN XhciProcessEvents(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT *Invalidate, PULONG InvalidateCount);
ULONG XhciReadPortsc(PXHCI_EXTENSION Xhci, USHORT Port);
VOID XhciWritePortsc(PXHCI_EXTENSION Xhci, USHORT Port, ULONG Value);

/* roothub.c */
VOID NTAPI XHCI_RH_GetRootHubData(PVOID, PVOID);
MPSTATUS NTAPI XHCI_RH_GetStatus(PVOID, PUSHORT);
MPSTATUS NTAPI XHCI_RH_GetPortStatus(PVOID, USHORT, PUSB_PORT_STATUS_AND_CHANGE);
MPSTATUS NTAPI XHCI_RH_GetHubStatus(PVOID, PUSB_HUB_STATUS_AND_CHANGE);
MPSTATUS NTAPI XHCI_RH_SetFeaturePortReset(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_SetFeaturePortPower(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_SetFeaturePortEnable(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_SetFeaturePortSuspend(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_ClearFeaturePortEnable(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_ClearFeaturePortPower(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_ClearFeaturePortSuspend(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_ClearFeaturePortEnableChange(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_ClearFeaturePortConnectChange(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_ClearFeaturePortResetChange(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_ClearFeaturePortSuspendChange(PVOID, USHORT);
MPSTATUS NTAPI XHCI_RH_ClearFeaturePortOvercurrentChange(PVOID, USHORT);
VOID NTAPI XHCI_RH_DisableIrq(PVOID);
VOID NTAPI XHCI_RH_EnableIrq(PVOID);

extern USBPORT_REGISTRATION_PACKET RegPacket;
