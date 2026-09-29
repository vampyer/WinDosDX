/*
 * PROJECT:     WinDosDX xHCI (USB 3) host controller driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Root hub: xHCI ports as USB 2 hub ports
 *
 * USB 2 and USB 3 ports of the controller appear as one list of ports.
 * A connected device is reported as high speed (see usbxhci.h); its real
 * speed is read from the port when its slot is made.
 */

#include "usbxhci.h"

#define NDEBUG
#include <debug.h>

/* USB 2.0 hub port status (low word) and change (high word) bits */
#define PS_CONNECT          (1 << 0)
#define PS_ENABLE           (1 << 1)
#define PS_SUSPEND          (1 << 2)
#define PS_OVERCURRENT      (1 << 3)
#define PS_RESET            (1 << 4)
#define PS_POWER            (1 << 8)
#define PS_HIGH_SPEED       (1 << 10)
#define PC_CONNECT          (1 << 16)
#define PC_ENABLE           (1 << 17)
#define PC_SUSPEND          (1 << 18)
#define PC_OVERCURRENT      (1 << 19)
#define PC_RESET            (1 << 20)

static BOOLEAN
ValidPort(PXHCI_EXTENSION Xhci, USHORT Port)
{
    return Port >= 1 && Port <= Xhci->MaxPorts;
}

/* Writes PORTSC keeping its state and clearing only the given bits */
static VOID
PortWrite(PXHCI_EXTENSION Xhci, USHORT Port, ULONG Set)
{
    ULONG Portsc = XhciReadPortsc(Xhci, Port);
    XhciWritePortsc(Xhci, Port, PORTSC_WRITE_MASK(Portsc) | Set);
}

VOID
NTAPI
XHCI_RH_GetRootHubData(IN PVOID MiniPortExtension, IN PVOID RootHubData)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PUSBPORT_ROOT_HUB_DATA Data = RootHubData;
    USBPORT_HUB_20_CHARACTERISTICS Characteristics;

    Characteristics.AsUSHORT = 0;
    Characteristics.PowerControlMode = 1;           /* per port */
    Characteristics.OverCurrentProtectionMode = 1;  /* per port */

    Data->NumberOfPorts = Xhci->MaxPorts;
    Data->HubCharacteristics.Usb20HubCharacteristics = Characteristics;
    Data->PowerOnToPowerGood = 10;                  /* 20 ms */
    Data->HubControlCurrent = 0;
}

MPSTATUS
NTAPI
XHCI_RH_GetStatus(IN PVOID MiniPortExtension, IN PUSHORT Status)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    *Status = USB_GETSTATUS_SELF_POWERED;
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_GetPortStatus(IN PVOID MiniPortExtension, IN USHORT Port,
                      IN PUSB_PORT_STATUS_AND_CHANGE PortStatus)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    ULONG Portsc, Value = 0;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    Portsc = XhciReadPortsc(Xhci, Port);

    if (Portsc & PORTSC_CCS)
        Value |= PS_CONNECT | PS_HIGH_SPEED;
    if (Portsc & PORTSC_PED)
        Value |= PS_ENABLE;
    if (PORTSC_PLS(Portsc) == PLS_U3)
        Value |= PS_SUSPEND;
    if (Portsc & PORTSC_OCA)
        Value |= PS_OVERCURRENT;
    if (Portsc & PORTSC_PR)
        Value |= PS_RESET;
    if (Portsc & PORTSC_PP)
        Value |= PS_POWER;

    if (Portsc & PORTSC_CSC)
        Value |= PC_CONNECT;
    if (Portsc & PORTSC_PEC)
        Value |= PC_ENABLE;
    if (Portsc & PORTSC_OCC)
        Value |= PC_OVERCURRENT;
    /* A USB 3 port reports a finished warm reset separately */
    if (Portsc & (PORTSC_PRC | PORTSC_WRC))
        Value |= PC_RESET;

    PortStatus->AsUlong32 = Value;
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_GetHubStatus(IN PVOID MiniPortExtension, IN PUSB_HUB_STATUS_AND_CHANGE HubStatus)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    HubStatus->AsUlong32 = 0;
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortReset(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    /* The next new device is on this port (see OpenDefaultPipe) */
    Xhci->LastResetPort = Port;
    PortWrite(Xhci, Port, PORTSC_PR);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortPower(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    PortWrite(Xhci, Port, PORTSC_PP);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortEnable(IN PVOID MiniPortExtension, IN USHORT Port)
{
    /* Ports are enabled by a reset (USB 2) or by link training (USB 3) */
    UNREFERENCED_PARAMETER(MiniPortExtension);
    UNREFERENCED_PARAMETER(Port);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_SetFeaturePortSuspend(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    ULONG Portsc;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    Portsc = XhciReadPortsc(Xhci, Port);
    XhciWritePortsc(Xhci, Port, (PORTSC_WRITE_MASK(Portsc) & ~PORTSC_PLS_MASK) |
                                PORTSC_LWS | (PLS_U3 << 5));
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortEnable(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    PortWrite(Xhci, Port, PORTSC_PED);     /* writing 1 disables */
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortPower(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    ULONG Portsc;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    Portsc = XhciReadPortsc(Xhci, Port);
    XhciWritePortsc(Xhci, Port, PORTSC_WRITE_MASK(Portsc) & ~PORTSC_PP);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortSuspend(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    ULONG Portsc;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    /* Back to U0 */
    Portsc = XhciReadPortsc(Xhci, Port);
    XhciWritePortsc(Xhci, Port, (PORTSC_WRITE_MASK(Portsc) & ~PORTSC_PLS_MASK) | PORTSC_LWS);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortEnableChange(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    PortWrite(Xhci, Port, PORTSC_PEC);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortConnectChange(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    PortWrite(Xhci, Port, PORTSC_CSC);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortResetChange(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    PortWrite(Xhci, Port, PORTSC_PRC | PORTSC_WRC);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortSuspendChange(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    PortWrite(Xhci, Port, PORTSC_PLC);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RH_ClearFeaturePortOvercurrentChange(IN PVOID MiniPortExtension, IN USHORT Port)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!ValidPort(Xhci, Port))
        return MP_STATUS_FAILURE;
    PortWrite(Xhci, Port, PORTSC_OCC);
    return MP_STATUS_SUCCESS;
}

/* Port changes arrive as events on the event ring, which is always on. */
VOID
NTAPI
XHCI_RH_DisableIrq(IN PVOID MiniPortExtension)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
}

VOID
NTAPI
XHCI_RH_EnableIrq(IN PVOID MiniPortExtension)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
}
