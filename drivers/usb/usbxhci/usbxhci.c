/*
 * PROJECT:     WinDosDX xHCI (USB 3) host controller driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Controller, rings, device slots, endpoints and transfers
 *
 * See usbxhci.h for how the port driver's USB 2 model maps onto xHCI.
 * Commands (slot and endpoint setup) are synchronous: the caller waits for
 * the Command Completion event, polling the event ring. Transfer events
 * mark transfers done; the port driver completes them in PollEndpoint.
 */

#include "usbxhci.h"

#define NDEBUG
#include <debug.h>

USBPORT_REGISTRATION_PACKET RegPacket;

/* ---------------------------------------------------------------------- */
/* Registers and memory                                                     */
/* ---------------------------------------------------------------------- */

static ULONG
Rd(PUCHAR Base, ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Base + Offset));
}

static VOID
Wr(PUCHAR Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Base + Offset), Value);
}

static VOID
Wr64(PUCHAR Base, ULONG Offset, ULONG64 Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Base + Offset), (ULONG)Value);
    WRITE_REGISTER_ULONG((PULONG)(Base + Offset + 4), (ULONG)(Value >> 32));
}

/* Physical address of something inside the controller's DMA block */
static ULONG
ResPA(PXHCI_EXTENSION Xhci, PVOID Va)
{
    return Xhci->ResourcesPA + (ULONG)((PUCHAR)Va - (PUCHAR)Xhci->Resources);
}

ULONG
XhciReadPortsc(PXHCI_EXTENSION Xhci, USHORT Port)
{
    return Rd(Xhci->Operational, XHCI_PORTSC(Port));
}

VOID
XhciWritePortsc(PXHCI_EXTENSION Xhci, USHORT Port, ULONG Value)
{
    Wr(Xhci->Operational, XHCI_PORTSC(Port), Value);
}

static BOOLEAN
WaitBits(PUCHAR Base, ULONG Offset, ULONG Mask, ULONG Wanted, ULONG Milliseconds)
{
    ULONG i;

    for (i = 0; i < Milliseconds * 10; i++)
    {
        if ((Rd(Base, Offset) & Mask) == Wanted)
            return TRUE;
        KeStallExecutionProcessor(100);
    }
    return (Rd(Base, Offset) & Mask) == Wanted;
}

/* Contexts of a slot: entry 0 of an input context is the input control
 * context, then the slot context, then endpoint contexts by DCI. */
static PVOID
InputEntry(PXHCI_EXTENSION Xhci, ULONG Slot, ULONG Entry)
{
    return Xhci->Resources->InputContext[Slot - 1] + Entry * Xhci->ContextSize;
}

static PVOID
OutputEntry(PXHCI_EXTENSION Xhci, ULONG Slot, ULONG Entry)
{
    return Xhci->Resources->DeviceContext[Slot - 1] + Entry * Xhci->ContextSize;
}

static VOID
ClearInputContext(PXHCI_EXTENSION Xhci, ULONG Slot)
{
    RtlZeroMemory(Xhci->Resources->InputContext[Slot - 1], 33 * XHCI_CONTEXT_STRIDE);
}

/* ---------------------------------------------------------------------- */
/* Event ring                                                               */
/* ---------------------------------------------------------------------- */

static USBD_STATUS
StatusFromCompletion(ULONG Code)
{
    switch (Code)
    {
        case CC_SUCCESS:
        case CC_SHORT_PACKET:
            return USBD_STATUS_SUCCESS;
        case CC_STALL:
            return USBD_STATUS_STALL_PID;
        case CC_BABBLE:
            return USBD_STATUS_BABBLE_DETECTED;
        case CC_DATA_BUFFER_ERROR:
            return USBD_STATUS_DATA_BUFFER_ERROR;
        default:
            return USBD_STATUS_XACT_ERROR;
    }
}

static VOID
HandleTransferEvent(PXHCI_EXTENSION Xhci, PXHCI_TRB Event)
{
    ULONG Slot = TRB_GET_SLOT(Event->Control);
    ULONG Dci = TRB_GET_EP(Event->Control);
    ULONG Code = TRB_COMPLETION(Event->Status);
    ULONG Residual = TRB_RESIDUAL(Event->Status);
    PXHCI_ENDPOINT Endpoint;
    PXHCI_TRANSFER Transfer;
    ULONG Pa, Index;
    BOOLEAN Last;

    if (Slot == 0 || Slot > XHCI_MAX_SLOTS || Dci == 0 || Dci >= 32)
        return;
    Endpoint = Xhci->Slots[Slot].Endpoints[Dci];
    if (!Endpoint)
        return;

    Pa = (ULONG)Event->Parameter;
    if (Pa < Endpoint->RingPA || Pa >= Endpoint->RingPA + XHCI_RING_TRBS * sizeof(XHCI_TRB))
        return;
    Index = (Pa - Endpoint->RingPA) / sizeof(XHCI_TRB);
    Transfer = Endpoint->TrbOwner[Index];
    if (!Transfer || Transfer->Done)
        return;
    if (Code == CC_STOPPED || Code == CC_STOPPED_LENGTH)
        return;     /* an abort in progress */

    Last = (Index == Transfer->LastTrb);
    if (Code == CC_SUCCESS || Code == CC_SHORT_PACKET)
    {
        if (Code == CC_SHORT_PACKET)
        {
            Transfer->Transferred = Endpoint->TrbOffset[Index] + Endpoint->TrbLength[Index] - Residual;
            Transfer->ShortSeen = TRUE;
        }
        /* A short packet ends a bulk or interrupt TD; a control transfer
         * goes on to its status stage, which reports last. */
        if (Last || (!Transfer->IsControl && Code == CC_SHORT_PACKET))
        {
            if (!Transfer->ShortSeen)
                Transfer->Transferred = Transfer->Requested;
            Transfer->Status = USBD_STATUS_SUCCESS;
            Transfer->Done = TRUE;
        }
    }
    else
    {
        if (!Transfer->ShortSeen)
            Transfer->Transferred = Endpoint->TrbOffset[Index];
        Transfer->Status = StatusFromCompletion(Code);
        Transfer->Done = TRUE;
        if (Code == CC_STALL || Code == CC_BABBLE || Code == CC_TRANSACTION_ERROR)
            Endpoint->Halted = TRUE;
        DPRINT1("usbxhci: slot %lu dci %lu transfer failed, completion code %lu\n", Slot, Dci, Code);
    }
    if (Transfer->Done)
        Endpoint->NeedsPoll = TRUE;
}

/* Called with the lock held. Returns TRUE if any event was handled. */
BOOLEAN
XhciProcessEvents(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT *Invalidate, PULONG InvalidateCount)
{
    ULONG Handled = 0;

    UNREFERENCED_PARAMETER(Invalidate);
    UNREFERENCED_PARAMETER(InvalidateCount);

    if (!Xhci->Resources)
        return FALSE;

    for (;;)
    {
        PXHCI_TRB Event = &Xhci->Resources->EventRing[Xhci->EventDequeue];
        ULONG Control = Event->Control;

        if ((Control & TRB_CYCLE) != Xhci->EventCycle)
            break;
        KeMemoryBarrier();

        switch (TRB_GET_TYPE(Control))
        {
            case TRB_COMMAND_COMPLETION:
                if (Event->Parameter == Xhci->CommandTrbPA)
                {
                    Xhci->CommandCompletion = TRB_COMPLETION(Event->Status);
                    Xhci->CommandSlot = TRB_GET_SLOT(Control);
                    Xhci->CommandDone = TRUE;
                }
                break;

            case TRB_TRANSFER_EVENT:
                HandleTransferEvent(Xhci, Event);
                break;

            case TRB_PORT_STATUS_CHANGE:
                Xhci->PortChanged = TRUE;
                break;

            case TRB_HOST_CONTROLLER:
                DPRINT1("usbxhci: host controller event, code %lu\n", TRB_COMPLETION(Event->Status));
                break;

            default:
                break;
        }

        Handled++;
        if (++Xhci->EventDequeue == XHCI_EVENT_TRBS)
        {
            Xhci->EventDequeue = 0;
            Xhci->EventCycle ^= 1;
        }
    }

    if (Handled)
    {
        Wr64(Xhci->Runtime, XHCI_ERDP,
             ResPA(Xhci, &Xhci->Resources->EventRing[Xhci->EventDequeue]) | ERDP_EHB);
    }
    return Handled != 0;
}

/* After events were handled: let the port driver poll finished endpoints
 * and look at the root hub. Never called with our lock held. */
static VOID
XhciNotifyPort(PXHCI_EXTENSION Xhci)
{
    ULONG Slot, Dci;
    BOOLEAN PortChanged;
    KIRQL OldIrql;

    for (Slot = 1; Slot <= XHCI_MAX_SLOTS; Slot++)
    {
        if (!Xhci->Slots[Slot].InUse)
            continue;
        for (Dci = 1; Dci < 32; Dci++)
        {
            PXHCI_ENDPOINT Endpoint = Xhci->Slots[Slot].Endpoints[Dci];
            if (Endpoint && Endpoint->NeedsPoll)
            {
                Endpoint->NeedsPoll = FALSE;
                RegPacket.UsbPortInvalidateEndpoint(Xhci, Endpoint);
            }
        }
    }

    KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
    PortChanged = Xhci->PortChanged;
    Xhci->PortChanged = FALSE;
    KeReleaseSpinLock(&Xhci->Lock, OldIrql);
    if (PortChanged)
        RegPacket.UsbPortInvalidateRootHub(Xhci);
}

static VOID
XhciService(PXHCI_EXTENSION Xhci)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
    XhciProcessEvents(Xhci, NULL, NULL);
    KeReleaseSpinLock(&Xhci->Lock, OldIrql);
    XhciNotifyPort(Xhci);
}

/* ---------------------------------------------------------------------- */
/* Commands                                                                 */
/* ---------------------------------------------------------------------- */

/*
 * Runs one command and waits for its completion (at most Milliseconds).
 * Returns the completion code (0 on timeout); *Slot gets the slot id the
 * completion reports (Enable Slot).
 */
static ULONG
XhciCommand(PXHCI_EXTENSION Xhci, ULONG64 Parameter, ULONG Status, ULONG Control,
            ULONG Milliseconds, PULONG Slot)
{
    KIRQL OldIrql;
    PXHCI_TRB Trb;
    ULONG i, Code = 0;
    BOOLEAN Done = FALSE;

    /* One command at a time */
    for (i = 0;; i++)
    {
        KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
        if (!Xhci->CommandBusy)
            break;
        KeReleaseSpinLock(&Xhci->Lock, OldIrql);
        if (i > 20000)
            return 0;
        KeStallExecutionProcessor(50);
    }
    Xhci->CommandBusy = TRUE;

    Trb = &Xhci->Resources->CommandRing[Xhci->CommandEnqueue];
    Trb->Parameter = Parameter;
    Trb->Status = Status;
    KeMemoryBarrier();
    Trb->Control = (Control & ~TRB_CYCLE) | Xhci->CommandCycle;
    Xhci->CommandTrbPA = ResPA(Xhci, Trb);
    Xhci->CommandDone = FALSE;

    if (++Xhci->CommandEnqueue == XHCI_COMMAND_TRBS - 1)
    {
        PXHCI_TRB Link = &Xhci->Resources->CommandRing[XHCI_COMMAND_TRBS - 1];
        Link->Control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE | Xhci->CommandCycle;
        Xhci->CommandCycle ^= 1;
        Xhci->CommandEnqueue = 0;
    }
    KeMemoryBarrier();
    WRITE_REGISTER_ULONG(&Xhci->Doorbells[0], 0);
    KeReleaseSpinLock(&Xhci->Lock, OldIrql);

    for (i = 0; i < Milliseconds * 10; i++)
    {
        KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
        XhciProcessEvents(Xhci, NULL, NULL);
        Done = Xhci->CommandDone;
        KeReleaseSpinLock(&Xhci->Lock, OldIrql);
        if (Done)
            break;
        KeStallExecutionProcessor(100);
    }

    KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
    if (Done)
    {
        Code = Xhci->CommandCompletion;
        if (Slot)
            *Slot = Xhci->CommandSlot;
    }
    Xhci->CommandBusy = FALSE;
    KeReleaseSpinLock(&Xhci->Lock, OldIrql);

    if (!Done)
        DPRINT1("usbxhci: command 0x%lx timed out\n", TRB_GET_TYPE(Control));
    else if (Code != CC_SUCCESS)
        DPRINT1("usbxhci: command %lu failed with code %lu\n", TRB_GET_TYPE(Control), Code);
    return Code;
}

/* ---------------------------------------------------------------------- */
/* Controller                                                               */
/* ---------------------------------------------------------------------- */

/* Take the controller from the BIOS (USB legacy support capability). */
static VOID
XhciBiosHandoff(PXHCI_EXTENSION Xhci, ULONG Hcc1)
{
    ULONG Offset = HCC1_XECP(Hcc1) * 4;
    ULONG i;

    while (Offset)
    {
        ULONG Cap = Rd(Xhci->Base, Offset);

        if (XECP_ID(Cap) == XECP_LEGACY)
        {
            if (Cap & LEGACY_BIOS_OWNED)
            {
                Wr(Xhci->Base, Offset, Cap | LEGACY_OS_OWNED);
                for (i = 0; i < 1000 && (Rd(Xhci->Base, Offset) & LEGACY_BIOS_OWNED); i++)
                    KeStallExecutionProcessor(1000);
                if (Rd(Xhci->Base, Offset) & LEGACY_BIOS_OWNED)
                    DPRINT1("usbxhci: the BIOS did not release the controller\n");
            }
            /* No more SMIs from USB events; clear pending ones. */
            Wr(Xhci->Base, Offset + LEGACY_CTLSTS,
               (Rd(Xhci->Base, Offset + LEGACY_CTLSTS) & LEGACY_SMI_MASK) | LEGACY_SMI_MASK);
            Wr(Xhci->Base, Offset + LEGACY_CTLSTS, LEGACY_SMI_MASK);
            return;
        }
        if (!XECP_NEXT(Cap))
            break;
        Offset += XECP_NEXT(Cap) * 4;
    }
}

static MPSTATUS
NTAPI
XHCI_StartController(IN PVOID MiniPortExtension, IN PUSBPORT_RESOURCES Resources)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PXHCI_HC_RESOURCES Res;
    ULONG Hcs1, Hcs2, Hcc1, i, Port;

    if ((Resources->ResourcesTypes & (USBPORT_RESOURCES_MEMORY | USBPORT_RESOURCES_INTERRUPT)) !=
        (USBPORT_RESOURCES_MEMORY | USBPORT_RESOURCES_INTERRUPT))
    {
        DPRINT1("usbxhci: missing resources (0x%lx)\n", Resources->ResourcesTypes);
        return MP_STATUS_ERROR;
    }

    KeInitializeSpinLock(&Xhci->Lock);
    Xhci->Base = Resources->ResourceBase;
    Xhci->Operational = Xhci->Base + (Rd(Xhci->Base, XHCI_CAPLENGTH) & 0xFF);
    Xhci->Runtime = Xhci->Base + (Rd(Xhci->Base, XHCI_RTSOFF) & ~0x1F);
    Xhci->Doorbells = (PULONG)(Xhci->Base + (Rd(Xhci->Base, XHCI_DBOFF) & ~0x3));

    Hcs1 = Rd(Xhci->Base, XHCI_HCSPARAMS1);
    Hcs2 = Rd(Xhci->Base, XHCI_HCSPARAMS2);
    Hcc1 = Rd(Xhci->Base, XHCI_HCCPARAMS1);
    Xhci->MaxPorts = HCS1_MAX_PORTS(Hcs1);
    Xhci->MaxSlots = min(HCS1_MAX_SLOTS(Hcs1), XHCI_MAX_SLOTS);
    Xhci->ContextSize = (Hcc1 & HCC1_CSZ) ? 64 : 32;
    Xhci->ScratchpadCount = HCS2_MAX_SCRATCHPADS(Hcs2);

    DPRINT1("usbxhci: version %x, %lu ports, %lu slots, %lu-byte contexts, %lu scratchpads\n",
            Rd(Xhci->Base, XHCI_CAPLENGTH) >> 16, Xhci->MaxPorts, HCS1_MAX_SLOTS(Hcs1),
            Xhci->ContextSize, Xhci->ScratchpadCount);

    if (Xhci->ScratchpadCount > XHCI_MAX_SCRATCHPADS)
    {
        DPRINT1("usbxhci: needs %lu scratchpad pages, has room for %u\n",
                Xhci->ScratchpadCount, XHCI_MAX_SCRATCHPADS);
        return MP_STATUS_ERROR;
    }
    if (Xhci->ScratchpadCount && !(Rd(Xhci->Operational, XHCI_PAGESIZE) & 1))
    {
        DPRINT1("usbxhci: page size other than 4 KB is not supported\n");
        return MP_STATUS_ERROR;
    }
    if ((Resources->StartPA & 0xFFF) || !Resources->StartVA)
    {
        DPRINT1("usbxhci: DMA block not page aligned\n");
        return MP_STATUS_ERROR;
    }

    XhciBiosHandoff(Xhci, Hcc1);

    /* Stop and reset the controller */
    WaitBits(Xhci->Operational, XHCI_USBSTS, USBSTS_CNR, 0, 1000);
    Wr(Xhci->Operational, XHCI_USBCMD, Rd(Xhci->Operational, XHCI_USBCMD) & ~USBCMD_RS);
    if (!WaitBits(Xhci->Operational, XHCI_USBSTS, USBSTS_HCH, USBSTS_HCH, 100))
        DPRINT1("usbxhci: controller did not halt\n");
    Wr(Xhci->Operational, XHCI_USBCMD, USBCMD_HCRST);
    if (!WaitBits(Xhci->Operational, XHCI_USBCMD, USBCMD_HCRST, 0, 1000) ||
        !WaitBits(Xhci->Operational, XHCI_USBSTS, USBSTS_CNR, 0, 1000))
    {
        DPRINT1("usbxhci: controller reset failed\n");
        return MP_STATUS_ERROR;
    }

    /* Memory */
    Res = Xhci->Resources = (PXHCI_HC_RESOURCES)Resources->StartVA;
    Xhci->ResourcesPA = Resources->StartPA;
    RtlZeroMemory(Res, sizeof(*Res));

    for (i = 0; i < Xhci->ScratchpadCount; i++)
        Res->ScratchpadArray[i] = ResPA(Xhci, Res->Scratchpad[i]);
    if (Xhci->ScratchpadCount)
        Res->Dcbaa[0] = ResPA(Xhci, Res->ScratchpadArray);

    Wr(Xhci->Operational, XHCI_CONFIG, Xhci->MaxSlots);
    Wr64(Xhci->Operational, XHCI_DCBAAP, ResPA(Xhci, Res->Dcbaa));

    /* Command ring: the last TRB links back to the first */
    Res->CommandRing[XHCI_COMMAND_TRBS - 1].Parameter = ResPA(Xhci, Res->CommandRing);
    Res->CommandRing[XHCI_COMMAND_TRBS - 1].Control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    Xhci->CommandEnqueue = 0;
    Xhci->CommandCycle = 1;
    Wr64(Xhci->Operational, XHCI_CRCR, ResPA(Xhci, Res->CommandRing) | CRCR_RCS);

    /* Event ring: one segment, interrupter 0 */
    Res->Erst[0].Base = ResPA(Xhci, Res->EventRing);
    Res->Erst[0].Size = XHCI_EVENT_TRBS;
    Xhci->EventDequeue = 0;
    Xhci->EventCycle = 1;
    Wr(Xhci->Runtime, XHCI_ERSTSZ, 1);
    Wr64(Xhci->Runtime, XHCI_ERDP, ResPA(Xhci, Res->EventRing));
    Wr64(Xhci->Runtime, XHCI_ERSTBA, ResPA(Xhci, Res->Erst));
    Wr(Xhci->Runtime, XHCI_IMOD, 4000);     /* at most one interrupt per ms */
    Wr(Xhci->Runtime, XHCI_IMAN, IMAN_IE | IMAN_IP);

    /* Run */
    Wr(Xhci->Operational, XHCI_USBCMD, USBCMD_RS | USBCMD_INTE | USBCMD_HSEE);
    if (!WaitBits(Xhci->Operational, XHCI_USBSTS, USBSTS_HCH, 0, 100))
    {
        DPRINT1("usbxhci: controller did not start\n");
        return MP_STATUS_ERROR;
    }

    /* Power the ports where software controls the power */
    if (Hcc1 & HCC1_PPC)
    {
        for (Port = 1; Port <= Xhci->MaxPorts; Port++)
        {
            ULONG Portsc = XhciReadPortsc(Xhci, (USHORT)Port);
            if (!(Portsc & PORTSC_PP))
                XhciWritePortsc(Xhci, (USHORT)Port, PORTSC_WRITE_MASK(Portsc) | PORTSC_PP);
        }
    }

    DPRINT1("usbxhci: controller running\n");
    return MP_STATUS_SUCCESS;
}

static VOID
NTAPI
XHCI_StopController(IN PVOID MiniPortExtension, IN BOOLEAN DisableInterrupts)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    UNREFERENCED_PARAMETER(DisableInterrupts);
    if (!Xhci->Operational)
        return;
    Wr(Xhci->Operational, XHCI_USBCMD, Rd(Xhci->Operational, XHCI_USBCMD) & ~(USBCMD_RS | USBCMD_INTE));
    WaitBits(Xhci->Operational, XHCI_USBSTS, USBSTS_HCH, USBSTS_HCH, 100);
}

static VOID
NTAPI
XHCI_SuspendController(IN PVOID MiniPortExtension)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
}

static MPSTATUS
NTAPI
XHCI_ResumeController(IN PVOID MiniPortExtension)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    return MP_STATUS_SUCCESS;
}

static BOOLEAN
NTAPI
XHCI_InterruptService(IN PVOID MiniPortExtension)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    ULONG Status, Iman;

    if (!Xhci->Operational)
        return FALSE;
    Status = Rd(Xhci->Operational, XHCI_USBSTS);
    Iman = Rd(Xhci->Runtime, XHCI_IMAN);
    if (Status == 0xFFFFFFFF)
        return FALSE;   /* gone */
    if (!(Status & (USBSTS_EINT | USBSTS_HSE)) && !(Iman & IMAN_IP))
        return FALSE;

    /* Both are write-1-to-clear */
    Wr(Xhci->Operational, XHCI_USBSTS, Status & (USBSTS_EINT | USBSTS_HSE | USBSTS_PCD));
    Wr(Xhci->Runtime, XHCI_IMAN, Iman | IMAN_IP);
    if (Status & USBSTS_HSE)
        DPRINT1("usbxhci: host system error\n");
    return TRUE;
}

static VOID
NTAPI
XHCI_InterruptDpc(IN PVOID MiniPortExtension, IN BOOLEAN EnableInterrupts)
{
    UNREFERENCED_PARAMETER(EnableInterrupts);
    XhciService(MiniPortExtension);
}

static VOID
NTAPI
XHCI_PollController(IN PVOID MiniPortExtension)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    USHORT Port;
    KIRQL OldIrql;

    if (!Xhci->Resources)
        return;
    /* Port changes can go by without an event (a missed interrupt, or a
     * change before the controller ran): look at the ports too. */
    for (Port = 1; Port <= Xhci->MaxPorts; Port++)
    {
        if (XhciReadPortsc(Xhci, Port) & PORTSC_CHANGE_BITS)
        {
            KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
            Xhci->PortChanged = TRUE;
            KeReleaseSpinLock(&Xhci->Lock, OldIrql);
            break;
        }
    }
    XhciService(Xhci);
}

static VOID
NTAPI
XHCI_CheckController(IN PVOID MiniPortExtension)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    ULONG Status;

    if (!Xhci->Operational)
        return;
    Status = Rd(Xhci->Operational, XHCI_USBSTS);
    if (Status == 0xFFFFFFFF || (Status & (USBSTS_HCE | USBSTS_HSE)))
        DPRINT1("usbxhci: controller error, status 0x%lx\n", Status);
}

static ULONG
NTAPI
XHCI_Get32BitFrameNumber(IN PVOID MiniPortExtension)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    ULONG Frame;

    if (!Xhci->Runtime)
        return 0;
    /* MFINDEX counts 125 us microframes in 14 bits: 2048 frames */
    Frame = (Rd(Xhci->Runtime, XHCI_MFINDEX) & 0x3FFF) >> 3;
    if (Frame < Xhci->LastFrame)
        Xhci->FrameHigh += 0x800;
    Xhci->LastFrame = Frame;
    return Xhci->FrameHigh + Frame;
}

static VOID
NTAPI
XHCI_InterruptNextSOF(IN PVOID MiniPortExtension)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
}

static VOID
NTAPI
XHCI_EnableInterrupts(IN PVOID MiniPortExtension)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!Xhci->Operational)
        return;
    Wr(Xhci->Runtime, XHCI_IMAN, IMAN_IE);
    Wr(Xhci->Operational, XHCI_USBCMD, Rd(Xhci->Operational, XHCI_USBCMD) | USBCMD_INTE);
}

static VOID
NTAPI
XHCI_DisableInterrupts(IN PVOID MiniPortExtension)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;

    if (!Xhci->Operational)
        return;
    Wr(Xhci->Operational, XHCI_USBCMD, Rd(Xhci->Operational, XHCI_USBCMD) & ~USBCMD_INTE);
}

static VOID
NTAPI
XHCI_ResetController(IN PVOID MiniPortExtension)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
}

static VOID
NTAPI
XHCI_FlushInterrupts(IN PVOID MiniPortExtension)
{
    XhciService(MiniPortExtension);
}

static VOID
NTAPI
XHCI_TakePortControl(IN PVOID MiniPortExtension)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
}

/* ---------------------------------------------------------------------- */
/* Endpoints                                                                */
/* ---------------------------------------------------------------------- */

static UCHAR
EndpointDci(PUSBPORT_ENDPOINT_PROPERTIES Properties)
{
    ULONG Number = Properties->EndpointAddress & 0x0F;

    if (Properties->TransferType == USBPORT_TRANSFER_TYPE_CONTROL)
        return (UCHAR)(Number * 2 + 1);
    return (UCHAR)(Number * 2 + ((Properties->EndpointAddress & 0x80) ? 1 : 0));
}

static ULONG
Log2(ULONG Value)
{
    ULONG Result = 0;
    while (Value > 1)
    {
        Value >>= 1;
        Result++;
    }
    return Result;
}

/* An empty transfer ring whose last TRB links back to the first */
static VOID
InitRing(PXHCI_ENDPOINT Endpoint)
{
    RtlZeroMemory(Endpoint->Ring, XHCI_RING_TRBS * sizeof(XHCI_TRB));
    RtlZeroMemory(Endpoint->TrbOwner, sizeof(Endpoint->TrbOwner));
    Endpoint->Ring[XHCI_RING_TRBS - 1].Parameter = Endpoint->RingPA;
    Endpoint->Ring[XHCI_RING_TRBS - 1].Control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
    Endpoint->Enqueue = 0;
    Endpoint->Cycle = 1;
    Endpoint->TrbsInUse = 0;
}

static ULONG
DefaultMaxPacket0(UCHAR Speed)
{
    switch (Speed)
    {
        case XHCI_SPEED_SUPER:
            return 512;
        case XHCI_SPEED_HIGH:
            return 64;
        default:
            /* Low speed is always 8; full speed is learned from the device
             * descriptor (see XHCI_PollEndpoint), 8 works until then. */
            return 8;
    }
}

/* The port a new device is on: the one the hub driver reset last, or else
 * the first enabled port without a device yet. */
static USHORT
NewDevicePort(PXHCI_EXTENSION Xhci)
{
    USHORT Port;
    ULONG Slot;

    if (Xhci->LastResetPort)
        return Xhci->LastResetPort;
    for (Port = 1; Port <= Xhci->MaxPorts; Port++)
    {
        BOOLEAN Used = FALSE;
        if (!(XhciReadPortsc(Xhci, Port) & PORTSC_PED))
            continue;
        for (Slot = 1; Slot <= XHCI_MAX_SLOTS; Slot++)
        {
            if (Xhci->Slots[Slot].InUse && Xhci->Slots[Slot].RootPort == Port)
                Used = TRUE;
        }
        if (!Used)
            return Port;
    }
    return 1;
}

static VOID
FreeSlot(PXHCI_EXTENSION Xhci, ULONG Slot)
{
    XhciCommand(Xhci, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(Slot), 200, NULL);
    Xhci->Resources->Dcbaa[Slot] = 0;
    if (Xhci->AddressToSlot[Xhci->Slots[Slot].UsbAddress & 0x7F] == Slot)
        Xhci->AddressToSlot[Xhci->Slots[Slot].UsbAddress & 0x7F] = 0;
    RtlZeroMemory(&Xhci->Slots[Slot], sizeof(Xhci->Slots[Slot]));
    DPRINT1("usbxhci: slot %lu freed\n", Slot);
}

/*
 * The port driver reopens a default pipe by closing and opening it (after
 * SET_ADDRESS, for instance), so closing one does not free the slot at
 * once. A slot whose pipe was not reopened belongs to a device that went
 * away; it is freed when the next device arrives.
 */
static VOID
FreePendingSlots(PXHCI_EXTENSION Xhci)
{
    ULONG Slot;

    for (Slot = 1; Slot <= XHCI_MAX_SLOTS; Slot++)
    {
        if (Xhci->Slots[Slot].InUse && Xhci->Slots[Slot].PendingFree)
            FreeSlot(Xhci, Slot);
    }
}

/* Defined below: a new EP0 max packet size learned from the device. */
static VOID
SetMaxPacket0(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT Endpoint, ULONG MaxPacket);

/* A default pipe opened again for a device that has its slot: the ring may
 * be new, so point the controller at it. */
static MPSTATUS
ReattachDefaultPipe(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT Endpoint, ULONG Slot)
{
    PXHCI_ENDPOINT_CONTEXT Ep0 = OutputEntry(Xhci, Slot, 1);
    ULONG CurrentMaxPacket = (Ep0->Info2 >> 16) & 0xFFFF;

    Xhci->Slots[Slot].PendingFree = FALSE;
    Xhci->Slots[Slot].Endpoints[1] = Endpoint;
    Endpoint->Slot = (UCHAR)Slot;

    /* The endpoint is Stopped for the dequeue pointer to move */
    XhciCommand(Xhci, 0, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | TRB_SLOT(Slot) | TRB_EP(1), 200, NULL);
    XhciCommand(Xhci, Endpoint->RingPA | Endpoint->Cycle, 0,
                TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(Slot) | TRB_EP(1), 200, NULL);

    /* SuperSpeed EP0 stays at 512 (the stack is told 64, see PollEndpoint) */
    if (Xhci->Slots[Slot].Speed == XHCI_SPEED_SUPER)
        Endpoint->Properties.MaxPacketSize = CurrentMaxPacket;
    else if ((Endpoint->Properties.MaxPacketSize & 0x7FF) != CurrentMaxPacket)
        SetMaxPacket0(Xhci, Endpoint, Endpoint->Properties.MaxPacketSize & 0x7FF);
    return MP_STATUS_SUCCESS;
}

/* A new device: a slot, addressed but without SET_ADDRESS (BSR). */
static MPSTATUS
OpenDefaultPipe(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT Endpoint)
{
    PXHCI_INPUT_CONTROL_CONTEXT Control;
    PXHCI_SLOT_CONTEXT SlotContext;
    PXHCI_ENDPOINT_CONTEXT Ep0;
    ULONG Slot = 0, Code, MaxPacket;
    USHORT Port;
    UCHAR Speed;

    FreePendingSlots(Xhci);
    Port = NewDevicePort(Xhci);
    Speed = (UCHAR)PORTSC_SPEED(XhciReadPortsc(Xhci, Port));

    Code = XhciCommand(Xhci, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), 500, &Slot);
    if (Code != CC_SUCCESS || Slot == 0 || Slot > Xhci->MaxSlots)
    {
        DPRINT1("usbxhci: no device slot (code %lu, slot %lu)\n", Code, Slot);
        return MP_STATUS_NO_RESOURCES;
    }

    MaxPacket = DefaultMaxPacket0(Speed);
    RtlZeroMemory(Xhci->Resources->DeviceContext[Slot - 1], 32 * XHCI_CONTEXT_STRIDE);
    Xhci->Resources->Dcbaa[Slot] = ResPA(Xhci, Xhci->Resources->DeviceContext[Slot - 1]);

    ClearInputContext(Xhci, Slot);
    Control = InputEntry(Xhci, Slot, 0);
    Control->AddFlags = (1 << 0) | (1 << 1);     /* slot and EP0 */
    SlotContext = InputEntry(Xhci, Slot, 1);
    SlotContext->Info1 = SLOT_ROUTE(0) | SLOT_SPEED(Speed) | SLOT_ENTRIES(1);
    SlotContext->Info2 = SLOT_ROOT_PORT(Port);
    Ep0 = InputEntry(Xhci, Slot, 2);
    Ep0->Info2 = EP_CERR(3) | EP_TYPE(EPT_CONTROL) | EP_MAX_PACKET(MaxPacket);
    Ep0->Dequeue = Endpoint->RingPA | Endpoint->Cycle;
    Ep0->TxInfo = EP_AVG_TRB_LENGTH(8);

    Code = XhciCommand(Xhci, ResPA(Xhci, Xhci->Resources->InputContext[Slot - 1]), 0,
                       TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_BSR | TRB_SLOT(Slot), 500, NULL);
    if (Code != CC_SUCCESS)
    {
        XhciCommand(Xhci, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(Slot), 200, NULL);
        Xhci->Resources->Dcbaa[Slot] = 0;
        return MP_STATUS_FAILURE;
    }

    RtlZeroMemory(&Xhci->Slots[Slot], sizeof(Xhci->Slots[Slot]));
    Xhci->Slots[Slot].InUse = TRUE;
    Xhci->Slots[Slot].Speed = Speed;
    Xhci->Slots[Slot].RootPort = (UCHAR)Port;
    Xhci->Slots[Slot].Endpoints[1] = Endpoint;
    Endpoint->Slot = (UCHAR)Slot;
    Endpoint->Properties.MaxPacketSize = MaxPacket;
    Xhci->LastResetPort = 0;

    DPRINT1("usbxhci: port %u, speed %u: slot %lu\n", Port, Speed, Slot);
    return MP_STATUS_SUCCESS;
}

/* Another endpoint of a configured device: Configure Endpoint. */
static MPSTATUS
OpenOtherPipe(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT Endpoint)
{
    PUSBPORT_ENDPOINT_PROPERTIES Properties = &Endpoint->Properties;
    ULONG Slot = Xhci->AddressToSlot[Properties->DeviceAddress & 0x7F];
    PXHCI_INPUT_CONTROL_CONTEXT Control;
    PXHCI_SLOT_CONTEXT SlotContext;
    PXHCI_ENDPOINT_CONTEXT EpContext;
    ULONG Type, Interval = 0, MaxPacket, Burst = 0, Code, Entries;
    BOOLEAN In = (Properties->EndpointAddress & 0x80) != 0;

    if (!Slot || !Xhci->Slots[Slot].InUse)
    {
        DPRINT1("usbxhci: no slot for address %u\n", Properties->DeviceAddress);
        return MP_STATUS_FAILURE;
    }

    switch (Properties->TransferType)
    {
        case USBPORT_TRANSFER_TYPE_BULK:
            Type = In ? EPT_BULK_IN : EPT_BULK_OUT;
            break;
        case USBPORT_TRANSFER_TYPE_INTERRUPT:
            Type = In ? EPT_INTERRUPT_IN : EPT_INTERRUPT_OUT;
            /* The period in ms, as 2^Interval microframes */
            Interval = Log2(max(Properties->Period, 1) * 8);
            if (Xhci->Slots[Slot].Speed <= XHCI_SPEED_LOW)
                Interval = max(3, min(Interval, 10));
            break;
        case USBPORT_TRANSFER_TYPE_CONTROL:
            Type = EPT_CONTROL;
            break;
        default:
            return MP_STATUS_NOT_SUPPORTED;     /* isochronous: not yet */
    }

    MaxPacket = Properties->MaxPacketSize & 0x7FF;
    if (Xhci->Slots[Slot].Speed == XHCI_SPEED_HIGH && Properties->TransactionPerMicroframe > 1)
        Burst = Properties->TransactionPerMicroframe - 1;

    ClearInputContext(Xhci, Slot);
    Control = InputEntry(Xhci, Slot, 0);
    Control->AddFlags = (1 << 0) | (1 << Endpoint->Dci);
    SlotContext = InputEntry(Xhci, Slot, 1);
    RtlCopyMemory(SlotContext, OutputEntry(Xhci, Slot, 0), sizeof(XHCI_SLOT_CONTEXT));
    SlotContext->State = 0;
    Entries = max(SLOT_GET_ENTRIES(SlotContext->Info1), Endpoint->Dci);
    SlotContext->Info1 = (SlotContext->Info1 & ~SLOT_ENTRIES(0x1F)) | SLOT_ENTRIES(Entries);

    EpContext = InputEntry(Xhci, Slot, 1 + Endpoint->Dci);
    EpContext->Info1 = EP_INTERVAL(Interval);
    EpContext->Info2 = EP_CERR(3) | EP_TYPE(Type) | EP_MAX_BURST(Burst) | EP_MAX_PACKET(MaxPacket);
    EpContext->Dequeue = Endpoint->RingPA | Endpoint->Cycle;
    if (Properties->TransferType == USBPORT_TRANSFER_TYPE_INTERRUPT)
        EpContext->TxInfo = EP_AVG_TRB_LENGTH(MaxPacket) | EP_MAX_ESIT(MaxPacket * (Burst + 1));
    else
        EpContext->TxInfo = EP_AVG_TRB_LENGTH(Properties->TransferType == USBPORT_TRANSFER_TYPE_CONTROL ? 8 : 3072);

    Code = XhciCommand(Xhci, ResPA(Xhci, Xhci->Resources->InputContext[Slot - 1]), 0,
                       TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | TRB_SLOT(Slot), 500, NULL);
    if (Code != CC_SUCCESS)
        return MP_STATUS_FAILURE;

    Endpoint->Slot = (UCHAR)Slot;
    Xhci->Slots[Slot].Endpoints[Endpoint->Dci] = Endpoint;
    return MP_STATUS_SUCCESS;
}

static MPSTATUS
NTAPI
XHCI_OpenEndpoint(IN PVOID MiniPortExtension,
                  IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                  IN PVOID MiniPortEndpoint)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PXHCI_ENDPOINT Endpoint = MiniPortEndpoint;

    RtlZeroMemory(Endpoint, sizeof(*Endpoint));
    Endpoint->Properties = *EndpointProperties;
    Endpoint->Dci = EndpointDci(EndpointProperties);
    Endpoint->State = USBPORT_ENDPOINT_PAUSED;
    Endpoint->Ring = (PXHCI_TRB)EndpointProperties->BufferVA;
    Endpoint->RingPA = EndpointProperties->BufferPA;
    InitializeListHead(&Endpoint->Transfers);

    if (!Endpoint->Ring || (Endpoint->RingPA & 0x3F) ||
        EndpointProperties->BufferLength < XHCI_RING_TRBS * sizeof(XHCI_TRB))
    {
        DPRINT1("usbxhci: endpoint buffer unusable\n");
        return MP_STATUS_NO_RESOURCES;
    }
    InitRing(Endpoint);

    if (EndpointProperties->TransferType == USBPORT_TRANSFER_TYPE_CONTROL && Endpoint->Dci == 1)
    {
        ULONG Slot = Xhci->AddressToSlot[EndpointProperties->DeviceAddress & 0x7F];

        if (EndpointProperties->DeviceAddress == 0)
            return OpenDefaultPipe(Xhci, Endpoint);
        /* The default pipe of a device that already has a slot */
        if (!Slot || !Xhci->Slots[Slot].InUse)
            return MP_STATUS_FAILURE;
        return ReattachDefaultPipe(Xhci, Endpoint, Slot);
    }
    return OpenOtherPipe(Xhci, Endpoint);
}

/* A new EP0 max packet size (learned from the device descriptor). */
static VOID
SetMaxPacket0(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT Endpoint, ULONG MaxPacket)
{
    ULONG Slot = Endpoint->Slot;
    PXHCI_INPUT_CONTROL_CONTEXT Control;
    PXHCI_ENDPOINT_CONTEXT Ep0;

    ClearInputContext(Xhci, Slot);
    Control = InputEntry(Xhci, Slot, 0);
    Control->AddFlags = 1 << 1;
    Ep0 = InputEntry(Xhci, Slot, 2);
    RtlCopyMemory(Ep0, OutputEntry(Xhci, Slot, 1), sizeof(XHCI_ENDPOINT_CONTEXT));
    Ep0->Info2 = (Ep0->Info2 & 0xFFFF) | EP_MAX_PACKET(MaxPacket);
    if (XhciCommand(Xhci, ResPA(Xhci, Xhci->Resources->InputContext[Slot - 1]), 0,
                    TRB_TYPE(TRB_EVALUATE_CONTEXT) | TRB_SLOT(Slot), 500, NULL) == CC_SUCCESS)
    {
        Endpoint->Properties.MaxPacketSize = MaxPacket;
    }
}

static MPSTATUS
NTAPI
XHCI_ReopenEndpoint(IN PVOID MiniPortExtension,
                    IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                    IN PVOID MiniPortEndpoint)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PXHCI_ENDPOINT Endpoint = MiniPortEndpoint;
    ULONG MaxPacket = Endpoint->Properties.MaxPacketSize;

    /* The slot and ring stay; the port driver's address may change. */
    Endpoint->Properties.DeviceAddress = EndpointProperties->DeviceAddress;
    if (Endpoint->Dci == 1 && Endpoint->Slot)
    {
        Xhci->AddressToSlot[EndpointProperties->DeviceAddress & 0x7F] = Endpoint->Slot;
        Xhci->Slots[Endpoint->Slot].UsbAddress = (UCHAR)EndpointProperties->DeviceAddress;
    }
    UNREFERENCED_PARAMETER(MaxPacket);
    return MP_STATUS_SUCCESS;
}

static VOID
NTAPI
XHCI_QueryEndpointRequirements(IN PVOID MiniPortExtension,
                               IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                               IN PUSBPORT_ENDPOINT_REQUIREMENTS EndpointRequirements)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);

    EndpointRequirements->HeaderBufferSize = XHCI_RING_TRBS * sizeof(XHCI_TRB);
    EndpointRequirements->MaxTransferSize =
        EndpointProperties->TransferType == USBPORT_TRANSFER_TYPE_INTERRUPT ? 0x1000 : XHCI_MAX_TRANSFER;
}

static VOID
NTAPI
XHCI_CloseEndpoint(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint, IN BOOLEAN IsDoDisablePeriodic)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PXHCI_ENDPOINT Endpoint = MiniPortEndpoint;
    ULONG Slot = Endpoint->Slot;

    UNREFERENCED_PARAMETER(IsDoDisablePeriodic);

    if (!Slot || !Xhci->Slots[Slot].InUse || Xhci->Slots[Slot].Endpoints[Endpoint->Dci] != Endpoint)
        return;

    if (Endpoint->Dci == 1)
    {
        /* Freed later unless the pipe is reopened (see FreePendingSlots) */
        Xhci->Slots[Slot].PendingFree = TRUE;
        Xhci->Slots[Slot].Endpoints[1] = NULL;
    }
    else
    {
        PXHCI_INPUT_CONTROL_CONTEXT Control;
        PXHCI_SLOT_CONTEXT SlotContext;

        ClearInputContext(Xhci, Slot);
        Control = InputEntry(Xhci, Slot, 0);
        Control->DropFlags = 1 << Endpoint->Dci;
        Control->AddFlags = 1 << 0;
        SlotContext = InputEntry(Xhci, Slot, 1);
        RtlCopyMemory(SlotContext, OutputEntry(Xhci, Slot, 0), sizeof(XHCI_SLOT_CONTEXT));
        SlotContext->State = 0;
        XhciCommand(Xhci, ResPA(Xhci, Xhci->Resources->InputContext[Slot - 1]), 0,
                    TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | TRB_SLOT(Slot), 200, NULL);
        Xhci->Slots[Slot].Endpoints[Endpoint->Dci] = NULL;
    }
}

static ULONG
NTAPI
XHCI_GetEndpointState(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    return ((PXHCI_ENDPOINT)MiniPortEndpoint)->State;
}

static VOID
NTAPI
XHCI_SetEndpointState(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint, IN ULONG EndpointState)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    ((PXHCI_ENDPOINT)MiniPortEndpoint)->State = EndpointState;
}

/* Point the endpoint's dequeue pointer at our enqueue position: whatever
 * was queued is dropped. */
static VOID
ResyncDequeue(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT Endpoint)
{
    ULONG64 Dequeue = (Endpoint->RingPA + Endpoint->Enqueue * sizeof(XHCI_TRB)) | Endpoint->Cycle;

    XhciCommand(Xhci, Dequeue, 0,
                TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(Endpoint->Slot) | TRB_EP(Endpoint->Dci),
                200, NULL);
}

static ULONG
NTAPI
XHCI_GetEndpointStatus(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    return ((PXHCI_ENDPOINT)MiniPortEndpoint)->Halted ? USBPORT_ENDPOINT_HALT : USBPORT_ENDPOINT_RUN;
}

static VOID
NTAPI
XHCI_SetEndpointStatus(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint, IN ULONG EndpointStatus)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PXHCI_ENDPOINT Endpoint = MiniPortEndpoint;

    if (EndpointStatus == USBPORT_ENDPOINT_RUN && Endpoint->Halted && Endpoint->Slot)
    {
        /* Clear the halt and start after what was queued */
        XhciCommand(Xhci, 0, 0,
                    TRB_TYPE(TRB_RESET_ENDPOINT) | TRB_SLOT(Endpoint->Slot) | TRB_EP(Endpoint->Dci),
                    200, NULL);
        ResyncDequeue(Xhci, Endpoint);
        Endpoint->Halted = FALSE;
    }
}

static VOID
NTAPI
XHCI_SetEndpointDataToggle(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint, IN ULONG DataToggle)
{
    /* The controller keeps the toggles; a Reset Endpoint clears them. */
    UNREFERENCED_PARAMETER(MiniPortExtension);
    UNREFERENCED_PARAMETER(MiniPortEndpoint);
    UNREFERENCED_PARAMETER(DataToggle);
}

static VOID
NTAPI
XHCI_RebalanceEndpoint(IN PVOID MiniPortExtension,
                       IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                       IN PVOID MiniPortEndpoint)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    UNREFERENCED_PARAMETER(EndpointProperties);
    UNREFERENCED_PARAMETER(MiniPortEndpoint);
}

/* ---------------------------------------------------------------------- */
/* Transfers                                                                */
/* ---------------------------------------------------------------------- */

/* Queues one TRB; the TD's first TRB is made visible last (see Publish). */
static ULONG
RingPut(PXHCI_ENDPOINT Endpoint, PXHCI_TRANSFER Transfer, ULONG64 Parameter, ULONG Status,
        ULONG Control, ULONG Offset, ULONG Length, BOOLEAN First)
{
    ULONG Index = Endpoint->Enqueue;
    PXHCI_TRB Trb = &Endpoint->Ring[Index];

    Trb->Parameter = Parameter;
    Trb->Status = Status;
    Endpoint->TrbOwner[Index] = Transfer;
    Endpoint->TrbOffset[Index] = Offset;
    Endpoint->TrbLength[Index] = Length;
    KeMemoryBarrier();
    /* The first TRB keeps the old cycle bit until the TD is complete */
    Trb->Control = (Control & ~TRB_CYCLE) | (First ? (Endpoint->Cycle ^ 1) : Endpoint->Cycle);
    Transfer->TrbCount++;
    Endpoint->TrbsInUse++;

    if (++Endpoint->Enqueue == XHCI_RING_TRBS - 1)
    {
        PXHCI_TRB Link = &Endpoint->Ring[XHCI_RING_TRBS - 1];
        /* Inside a TD the link carries the chain bit on */
        Link->Control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE | (Control & TRB_CHAIN) | Endpoint->Cycle;
        Endpoint->TrbOwner[XHCI_RING_TRBS - 1] = NULL;
        Endpoint->Cycle ^= 1;
        Endpoint->Enqueue = 0;
        Transfer->TrbCount++;
        Endpoint->TrbsInUse++;
    }
    return Index;
}

static VOID
Publish(PXHCI_EXTENSION Xhci, PXHCI_ENDPOINT Endpoint, PXHCI_TRANSFER Transfer)
{
    PXHCI_TRB First = &Endpoint->Ring[Transfer->FirstTrb];

    KeMemoryBarrier();
    First->Control ^= TRB_CYCLE;
    KeMemoryBarrier();
    WRITE_REGISTER_ULONG(&Xhci->Doorbells[Endpoint->Slot], Endpoint->Dci);
}

/* The data of a transfer as TRBs: one per scatter/gather piece, split so
 * none crosses a 64 KB boundary. Returns the index of the last one. */
static ULONG
QueueData(PXHCI_ENDPOINT Endpoint, PXHCI_TRANSFER Transfer, PUSBPORT_SCATTER_GATHER_LIST SgList,
          ULONG FirstType, ULONG FirstFlags, ULONG LastFlags, BOOLEAN FirstIsTdStart)
{
    ULONG Total = Transfer->Requested, Offset = 0, Last = 0, i;
    ULONG MaxPacket = max(Endpoint->Properties.MaxPacketSize & 0x7FF, 1);
    BOOLEAN FirstTrb = TRUE;

    for (i = 0; i < SgList->SgElementCount; i++)
    {
        ULONG64 Pa = SgList->SgElement[i].SgPhysicalAddress.QuadPart;
        ULONG Length = SgList->SgElement[i].SgTransferLength;

        while (Length)
        {
            ULONG Chunk = min(Length, 0x10000 - (ULONG)(Pa & 0xFFFF));
            ULONG Remaining = Total - (Offset + Chunk);
            BOOLEAN LastTrb = (Remaining == 0);
            ULONG Control = TRB_TYPE(FirstTrb ? FirstType : TRB_NORMAL) | TRB_ISP |
                            (FirstTrb ? FirstFlags : 0) | (LastTrb ? LastFlags : TRB_CHAIN);
            ULONG Index = RingPut(Endpoint, Transfer, Pa,
                                  TRB_LENGTH(Chunk) | TRB_TD_SIZE((Remaining + MaxPacket - 1) / MaxPacket),
                                  Control, Offset, Chunk, FirstTrb && FirstIsTdStart);
            if (FirstTrb && FirstIsTdStart)
                Transfer->FirstTrb = Index;
            Last = Index;
            FirstTrb = FALSE;
            Pa += Chunk;
            Offset += Chunk;
            Length -= Chunk;
        }
    }
    return Last;
}

static MPSTATUS
NTAPI
XHCI_SubmitTransfer(IN PVOID MiniPortExtension,
                    IN PVOID MiniPortEndpoint,
                    IN PUSBPORT_TRANSFER_PARAMETERS TransferParameters,
                    IN PVOID MiniPortTransfer,
                    IN PUSBPORT_SCATTER_GATHER_LIST SgList)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PXHCI_ENDPOINT Endpoint = MiniPortEndpoint;
    PXHCI_TRANSFER Transfer = MiniPortTransfer;
    ULONG Needed, Length = TransferParameters->TransferBufferLength;
    KIRQL OldIrql;

    RtlZeroMemory(Transfer, sizeof(*Transfer));
    Transfer->Parameters = TransferParameters;
    Transfer->SgList = SgList;
    Transfer->Requested = Length;
    Transfer->IsControl = (Endpoint->Properties.TransferType == USBPORT_TRANSFER_TYPE_CONTROL);

    if (!Endpoint->Slot)
        return MP_STATUS_FAILURE;

    /* SET_ADDRESS is the controller's job: Address Device. */
    if (Transfer->IsControl && TransferParameters->SetupPacket.bmRequestType.B == 0 &&
        TransferParameters->SetupPacket.bRequest == USB_REQUEST_SET_ADDRESS)
    {
        ULONG Slot = Endpoint->Slot, Code;
        PXHCI_INPUT_CONTROL_CONTEXT Control;

        /* The input context of the BSR Address Device is still there;
         * only the flags need setting again. */
        Control = InputEntry(Xhci, Slot, 0);
        RtlZeroMemory(Control, sizeof(*Control));
        Control->AddFlags = (1 << 0) | (1 << 1);
        Code = XhciCommand(Xhci, ResPA(Xhci, Xhci->Resources->InputContext[Slot - 1]), 0,
                           TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(Slot), 500, NULL);

        KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
        if (Code == CC_SUCCESS)
        {
            UCHAR Address = (UCHAR)(TransferParameters->SetupPacket.wValue.W & 0x7F);
            Xhci->AddressToSlot[Address] = (UCHAR)Slot;
            Xhci->Slots[Slot].UsbAddress = Address;
        }
        Transfer->Done = TRUE;
        Transfer->Status = (Code == CC_SUCCESS) ? USBD_STATUS_SUCCESS : USBD_STATUS_XACT_ERROR;
        InsertTailList(&Endpoint->Transfers, &Transfer->Link);
        Endpoint->NeedsPoll = TRUE;
        KeReleaseSpinLock(&Xhci->Lock, OldIrql);
        DPRINT1("usbxhci: slot %lu addressed as %u (code %lu)\n", Slot,
                TransferParameters->SetupPacket.wValue.W, Code);
        return MP_STATUS_SUCCESS;
    }

    /* Room: data TRBs (a piece may split once), setup, status, two links */
    Needed = SgList->SgElementCount * 2 + 4;
    KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
    if (Endpoint->TrbsInUse + Needed >= XHCI_RING_TRBS - 1)
    {
        KeReleaseSpinLock(&Xhci->Lock, OldIrql);
        return MP_STATUS_FAILURE;
    }

    if (Transfer->IsControl)
    {
        PUSB_DEFAULT_PIPE_SETUP_PACKET Setup = &TransferParameters->SetupPacket;
        BOOLEAN In = (Setup->bmRequestType.B & 0x80) != 0;
        ULONG64 SetupData;
        ULONG Trt = Length ? (In ? 3 : 2) : 0;

        RtlCopyMemory(&SetupData, Setup, sizeof(SetupData));
        Transfer->FirstTrb = RingPut(Endpoint, Transfer, SetupData, TRB_LENGTH(8),
                                     TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT(Trt), 0, 0, TRUE);
        if (Length)
            QueueData(Endpoint, Transfer, SgList, TRB_DATA, In ? TRB_DIR_IN : 0, 0, FALSE);
        /* Status stage: the other direction (IN when there is no data) */
        Transfer->LastTrb = RingPut(Endpoint, Transfer, 0, 0,
                                    TRB_TYPE(TRB_STATUS) | TRB_IOC | ((!Length || !In) ? TRB_DIR_IN : 0),
                                    Length, 0, FALSE);
    }
    else if (Length)
    {
        Transfer->LastTrb = QueueData(Endpoint, Transfer, SgList, TRB_NORMAL, 0, TRB_IOC, TRUE);
    }
    else
    {
        Transfer->FirstTrb = Transfer->LastTrb =
            RingPut(Endpoint, Transfer, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_IOC, 0, 0, TRUE);
    }

    InsertTailList(&Endpoint->Transfers, &Transfer->Link);
    Publish(Xhci, Endpoint, Transfer);
    KeReleaseSpinLock(&Xhci->Lock, OldIrql);
    return MP_STATUS_SUCCESS;
}

static MPSTATUS
NTAPI
XHCI_SubmitIsoTransfer(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint,
                       IN PUSBPORT_TRANSFER_PARAMETERS TransferParameters,
                       IN PVOID MiniPortTransfer, IN PVOID IsoTransfer)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    UNREFERENCED_PARAMETER(MiniPortEndpoint);
    UNREFERENCED_PARAMETER(TransferParameters);
    UNREFERENCED_PARAMETER(MiniPortTransfer);
    UNREFERENCED_PARAMETER(IsoTransfer);
    return MP_STATUS_NOT_SUPPORTED;
}

static VOID
ReleaseTrbs(PXHCI_ENDPOINT Endpoint, PXHCI_TRANSFER Transfer)
{
    ULONG i;

    for (i = 0; i < XHCI_RING_TRBS; i++)
    {
        if (Endpoint->TrbOwner[i] == Transfer)
            Endpoint->TrbOwner[i] = NULL;
    }
    Endpoint->TrbsInUse -= min(Endpoint->TrbsInUse, Transfer->TrbCount);
}

static VOID
NTAPI
XHCI_AbortTransfer(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint,
                   IN PVOID MiniPortTransfer, IN PULONG CompletedLength)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PXHCI_ENDPOINT Endpoint = MiniPortEndpoint;
    PXHCI_TRANSFER Transfer = MiniPortTransfer;
    KIRQL OldIrql;

    /* Stop the endpoint and skip what is queued. The port driver aborts a
     * pipe's transfers together, so dropping all of them is what it asks. */
    if (Endpoint->Slot && !Transfer->Done)
    {
        XhciCommand(Xhci, 0, 0,
                    TRB_TYPE(TRB_STOP_ENDPOINT) | TRB_SLOT(Endpoint->Slot) | TRB_EP(Endpoint->Dci),
                    200, NULL);
        ResyncDequeue(Xhci, Endpoint);
    }

    KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
    if (Transfer->Link.Flink)
        RemoveEntryList(&Transfer->Link);
    ReleaseTrbs(Endpoint, Transfer);
    Transfer->Done = TRUE;
    KeReleaseSpinLock(&Xhci->Lock, OldIrql);

    *CompletedLength = Transfer->Transferred;
}

static VOID
NTAPI
XHCI_PollEndpoint(IN PVOID MiniPortExtension, IN PVOID MiniPortEndpoint)
{
    PXHCI_EXTENSION Xhci = MiniPortExtension;
    PXHCI_ENDPOINT Endpoint = MiniPortEndpoint;
    LIST_ENTRY Finished;
    KIRQL OldIrql;

    InitializeListHead(&Finished);

    KeAcquireSpinLock(&Xhci->Lock, &OldIrql);
    XhciProcessEvents(Xhci, NULL, NULL);
    while (!IsListEmpty(&Endpoint->Transfers))
    {
        PXHCI_TRANSFER Transfer = CONTAINING_RECORD(Endpoint->Transfers.Flink, XHCI_TRANSFER, Link);
        if (!Transfer->Done)
            break;
        RemoveEntryList(&Transfer->Link);
        ReleaseTrbs(Endpoint, Transfer);
        InsertTailList(&Finished, &Transfer->Link);
    }
    KeReleaseSpinLock(&Xhci->Lock, OldIrql);

    while (!IsListEmpty(&Finished))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&Finished);
        PXHCI_TRANSFER Transfer = CONTAINING_RECORD(Entry, XHCI_TRANSFER, Link);
        PUSB_DEFAULT_PIPE_SETUP_PACKET Setup = &Transfer->Parameters->SetupPacket;

        /* SuperSpeed devices give bMaxPacketSize0 as a power of two (9 for
         * 512 bytes); the stack, which sees them as high speed, gets 64.
         * The controller keeps using 512. */
        if (Endpoint->Dci == 1 && Transfer->IsControl && Transfer->Status == USBD_STATUS_SUCCESS &&
            Setup->bmRequestType.B == 0x80 && Setup->bRequest == USB_REQUEST_GET_DESCRIPTOR &&
            (Setup->wValue.W >> 8) == USB_DEVICE_DESCRIPTOR_TYPE && Transfer->Transferred >= 8 &&
            Transfer->SgList && Transfer->SgList->MappedSystemVa &&
            Xhci->Slots[Endpoint->Slot].Speed == XHCI_SPEED_SUPER)
        {
            PUCHAR Descriptor = Transfer->SgList->MappedSystemVa;
            if (Descriptor[7] == 9)
                Descriptor[7] = 64;
        }

        /* The first 8 bytes of the device descriptor give EP0's real max
         * packet size; full-speed devices started with 8. */
        if (Endpoint->Dci == 1 && Transfer->IsControl && Transfer->Status == USBD_STATUS_SUCCESS &&
            Setup->bmRequestType.B == 0x80 && Setup->bRequest == USB_REQUEST_GET_DESCRIPTOR &&
            (Setup->wValue.W >> 8) == USB_DEVICE_DESCRIPTOR_TYPE && Transfer->Transferred >= 8 &&
            Transfer->SgList && Transfer->SgList->MappedSystemVa &&
            Xhci->Slots[Endpoint->Slot].Speed != XHCI_SPEED_SUPER)
        {
            ULONG MaxPacket = ((PUCHAR)Transfer->SgList->MappedSystemVa)[7];
            if ((MaxPacket == 8 || MaxPacket == 16 || MaxPacket == 32 || MaxPacket == 64) &&
                MaxPacket != Endpoint->Properties.MaxPacketSize)
            {
                SetMaxPacket0(Xhci, Endpoint, MaxPacket);
            }
        }

        RegPacket.UsbPortCompleteTransfer(Xhci, Endpoint, Transfer->Parameters,
                                          Transfer->Status, Transfer->Transferred);
    }
}

/* ---------------------------------------------------------------------- */
/* Unsupported miniport requests                                            */
/* ---------------------------------------------------------------------- */

static MPSTATUS
NTAPI
XHCI_StartSendOnePacket(IN PVOID MiniPortExtension, IN PVOID PacketParameters, IN PVOID Data,
                        IN PULONG pDataLength, IN PVOID BufferVA, IN PVOID BufferPA,
                        IN ULONG BufferLength, IN USBD_STATUS *pUSBDStatus)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    UNREFERENCED_PARAMETER(PacketParameters);
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(pDataLength);
    UNREFERENCED_PARAMETER(BufferVA);
    UNREFERENCED_PARAMETER(BufferPA);
    UNREFERENCED_PARAMETER(BufferLength);
    UNREFERENCED_PARAMETER(pUSBDStatus);
    return MP_STATUS_NOT_SUPPORTED;
}

static MPSTATUS
NTAPI
XHCI_EndSendOnePacket(IN PVOID MiniPortExtension, IN PVOID PacketParameters, IN PVOID Data,
                      IN PULONG pDataLength, IN PVOID BufferVA, IN PVOID BufferPA,
                      IN ULONG BufferLength, IN USBD_STATUS *pUSBDStatus)
{
    return XHCI_StartSendOnePacket(MiniPortExtension, PacketParameters, Data, pDataLength,
                                   BufferVA, BufferPA, BufferLength, pUSBDStatus);
}

static MPSTATUS
NTAPI
XHCI_PassThru(IN PVOID MiniPortExtension, IN PVOID PassThruGuid, IN ULONG ParameterLength,
              IN PVOID Parameters)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    UNREFERENCED_PARAMETER(PassThruGuid);
    UNREFERENCED_PARAMETER(ParameterLength);
    UNREFERENCED_PARAMETER(Parameters);
    return MP_STATUS_NOT_SUPPORTED;
}

static MPSTATUS
NTAPI
XHCI_RH_ChirpRootPort(IN PVOID MiniPortExtension, IN USHORT Port)
{
    UNREFERENCED_PARAMETER(MiniPortExtension);
    UNREFERENCED_PARAMETER(Port);
    return MP_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Registration                                                             */
/* ---------------------------------------------------------------------- */

static VOID
NTAPI
XHCI_Unload(IN PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    if (USBPORT_GetHciMn() != USBPORT_HCI_MN)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(&RegPacket, sizeof(USBPORT_REGISTRATION_PACKET));

    /* A USB 2 root hub to the rest of the stack (see usbxhci.h) */
    RegPacket.MiniPortVersion = USB_MINIPORT_VERSION_EHCI;
    RegPacket.MiniPortFlags = USB_MINIPORT_FLAGS_INTERRUPT |
                              USB_MINIPORT_FLAGS_MEMORY_IO |
                              USB_MINIPORT_FLAGS_USB2 |
                              USB_MINIPORT_FLAGS_POLLING;
    RegPacket.MiniPortBusBandwidth = TOTAL_USB20_BUS_BANDWIDTH;
    RegPacket.MiniPortExtensionSize = sizeof(XHCI_EXTENSION);
    RegPacket.MiniPortEndpointSize = sizeof(XHCI_ENDPOINT);
    RegPacket.MiniPortTransferSize = sizeof(XHCI_TRANSFER);
    RegPacket.MiniPortResourcesSize = sizeof(XHCI_HC_RESOURCES);

    RegPacket.OpenEndpoint = XHCI_OpenEndpoint;
    RegPacket.ReopenEndpoint = XHCI_ReopenEndpoint;
    RegPacket.QueryEndpointRequirements = XHCI_QueryEndpointRequirements;
    RegPacket.CloseEndpoint = XHCI_CloseEndpoint;
    RegPacket.StartController = XHCI_StartController;
    RegPacket.StopController = XHCI_StopController;
    RegPacket.SuspendController = XHCI_SuspendController;
    RegPacket.ResumeController = XHCI_ResumeController;
    RegPacket.InterruptService = XHCI_InterruptService;
    RegPacket.InterruptDpc = XHCI_InterruptDpc;
    RegPacket.SubmitTransfer = XHCI_SubmitTransfer;
    RegPacket.SubmitIsoTransfer = XHCI_SubmitIsoTransfer;
    RegPacket.AbortTransfer = XHCI_AbortTransfer;
    RegPacket.GetEndpointState = XHCI_GetEndpointState;
    RegPacket.SetEndpointState = XHCI_SetEndpointState;
    RegPacket.PollEndpoint = XHCI_PollEndpoint;
    RegPacket.CheckController = XHCI_CheckController;
    RegPacket.Get32BitFrameNumber = XHCI_Get32BitFrameNumber;
    RegPacket.InterruptNextSOF = XHCI_InterruptNextSOF;
    RegPacket.EnableInterrupts = XHCI_EnableInterrupts;
    RegPacket.DisableInterrupts = XHCI_DisableInterrupts;
    RegPacket.PollController = XHCI_PollController;
    RegPacket.SetEndpointDataToggle = XHCI_SetEndpointDataToggle;
    RegPacket.GetEndpointStatus = XHCI_GetEndpointStatus;
    RegPacket.SetEndpointStatus = XHCI_SetEndpointStatus;
    RegPacket.ResetController = XHCI_ResetController;
    RegPacket.RH_GetRootHubData = XHCI_RH_GetRootHubData;
    RegPacket.RH_GetStatus = XHCI_RH_GetStatus;
    RegPacket.RH_GetPortStatus = XHCI_RH_GetPortStatus;
    RegPacket.RH_GetHubStatus = XHCI_RH_GetHubStatus;
    RegPacket.RH_SetFeaturePortReset = XHCI_RH_SetFeaturePortReset;
    RegPacket.RH_SetFeaturePortPower = XHCI_RH_SetFeaturePortPower;
    RegPacket.RH_SetFeaturePortEnable = XHCI_RH_SetFeaturePortEnable;
    RegPacket.RH_SetFeaturePortSuspend = XHCI_RH_SetFeaturePortSuspend;
    RegPacket.RH_ClearFeaturePortEnable = XHCI_RH_ClearFeaturePortEnable;
    RegPacket.RH_ClearFeaturePortPower = XHCI_RH_ClearFeaturePortPower;
    RegPacket.RH_ClearFeaturePortSuspend = XHCI_RH_ClearFeaturePortSuspend;
    RegPacket.RH_ClearFeaturePortEnableChange = XHCI_RH_ClearFeaturePortEnableChange;
    RegPacket.RH_ClearFeaturePortConnectChange = XHCI_RH_ClearFeaturePortConnectChange;
    RegPacket.RH_ClearFeaturePortResetChange = XHCI_RH_ClearFeaturePortResetChange;
    RegPacket.RH_ClearFeaturePortSuspendChange = XHCI_RH_ClearFeaturePortSuspendChange;
    RegPacket.RH_ClearFeaturePortOvercurrentChange = XHCI_RH_ClearFeaturePortOvercurrentChange;
    RegPacket.RH_DisableIrq = XHCI_RH_DisableIrq;
    RegPacket.RH_EnableIrq = XHCI_RH_EnableIrq;
    RegPacket.StartSendOnePacket = XHCI_StartSendOnePacket;
    RegPacket.EndSendOnePacket = XHCI_EndSendOnePacket;
    RegPacket.PassThru = XHCI_PassThru;
    RegPacket.RebalanceEndpoint = XHCI_RebalanceEndpoint;
    RegPacket.FlushInterrupts = XHCI_FlushInterrupts;
    RegPacket.RH_ChirpRootPort = XHCI_RH_ChirpRootPort;
    RegPacket.TakePortControl = XHCI_TakePortControl;

    DriverObject->DriverUnload = XHCI_Unload;

    return USBPORT_RegisterUSBPortDriver(DriverObject,
                                         USB20_MINIPORT_INTERFACE_VERSION,
                                         &RegPacket);
}
