/*
 * PROJECT:        WinDosDX Storage Stack
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        NVMe (NVM Express) miniport
 * PROGRAMMERS:    WinDosDX Team
 *
 * Exposes NVMe namespace 1 as a SCSI disk through scsiport, so that the boot
 * volume (and any NVMe disk) is usable. See stornvme.h for the design.
 */

#include "stornvme.h"

VOID NTAPI NvmeTimer(PVOID DeviceExtension);

#define NVME_PA(Address) ((ULONGLONG)(Address).QuadPart)

/* ------------------------------------------------------------------------ */
/* Register access                                                          */
/* ------------------------------------------------------------------------ */

static ULONG NvmeRead32(PNVME_ADAPTER Adapter, ULONG Offset)
{
    return ScsiPortReadRegisterUlong((PULONG)(Adapter->Registers + Offset));
}

static VOID NvmeWrite32(PNVME_ADAPTER Adapter, ULONG Offset, ULONG Value)
{
    ScsiPortWriteRegisterUlong((PULONG)(Adapter->Registers + Offset), Value);
}

static ULONGLONG NvmeRead64(PNVME_ADAPTER Adapter, ULONG Offset)
{
    ULONGLONG Low = NvmeRead32(Adapter, Offset);
    ULONGLONG High = NvmeRead32(Adapter, Offset + 4);
    return Low | (High << 32);
}

static VOID NvmeWrite64(PNVME_ADAPTER Adapter, ULONG Offset, ULONGLONG Value)
{
    NvmeWrite32(Adapter, Offset, (ULONG)Value);
    NvmeWrite32(Adapter, Offset + 4, (ULONG)(Value >> 32));
}

static VOID NvmeRingDoorbell(PNVME_ADAPTER Adapter, PNVME_QUEUE Queue, BOOLEAN IsCq)
{
    ULONG Offset = NVME_REG_DOORBELL_BASE +
                   (2 * Queue->Qid + (IsCq ? 1 : 0)) * Adapter->DoorbellStride;
    NvmeWrite32(Adapter, Offset, Queue->Index);
}

/* ------------------------------------------------------------------------ */
/* Queues                                                                   */
/* ------------------------------------------------------------------------ */

static VOID NvmeSubmit(PNVME_ADAPTER Adapter, PNVME_QUEUE Sq, PNVME_SQE Command)
{
    PNVME_SQE Slot = (PNVME_SQE)Sq->Entries + Sq->Index;

    ScsiPortMoveMemory(Slot, Command, sizeof(*Slot));
    if (++Sq->Index == NVME_QUEUE_DEPTH)
        Sq->Index = 0;
    KeMemoryBarrier();
    NvmeRingDoorbell(Adapter, Sq, FALSE);
}

/* Take the next completion from Cq if the controller has posted one. */
static BOOLEAN NvmeNextCompletion(PNVME_QUEUE Cq, PUSHORT Cid, PUSHORT Status)
{
    volatile NVME_CQE *Cqe = (volatile NVME_CQE *)Cq->Entries + Cq->Index;
    USHORT Raw = Cqe->Status;

    if (NVME_CQE_PHASE(Raw) != Cq->Phase)
        return FALSE;

    KeMemoryBarrier();
    *Cid = Cqe->Cid;
    *Status = (USHORT)NVME_CQE_STATUS(Raw);
    if (++Cq->Index == NVME_QUEUE_DEPTH)
    {
        Cq->Index = 0;
        Cq->Phase ^= 1;
    }
    return TRUE;
}

/* Run one admin command and wait for it. Used only while starting up, with
 * the controller's interrupt masked. Returns the NVMe status (0 = success),
 * or 0xFFFF on a timeout. */
static USHORT NvmeAdminCommand(PNVME_ADAPTER Adapter, PNVME_SQE Command)
{
    USHORT Cid = Adapter->NextCid++;
    USHORT DoneCid, Status;
    ULONG Waited;

    Command->Cdw0 = (Command->Cdw0 & 0xFFFF) | ((ULONG)Cid << 16);
    NvmeSubmit(Adapter, &Adapter->AdminSq, Command);

    for (Waited = 0; Waited < NVME_ADMIN_TIMEOUT_US; Waited += 10)
    {
        while (NvmeNextCompletion(&Adapter->AdminCq, &DoneCid, &Status))
        {
            NvmeRingDoorbell(Adapter, &Adapter->AdminCq, TRUE);
            if (DoneCid == Cid)
                return Status;
        }
        ScsiPortStallExecution(10);
    }

    NvmeDebugPrint("admin opcode %02lx timed out\n", Command->Cdw0 & 0xFF);
    return 0xFFFF;
}

/* ------------------------------------------------------------------------ */
/* Controller start-up                                                      */
/* ------------------------------------------------------------------------ */

static BOOLEAN NvmeWaitReady(PNVME_ADAPTER Adapter, BOOLEAN Ready)
{
    ULONG Waited;

    for (Waited = 0; Waited < Adapter->ReadyTimeoutUs; Waited += 1000)
    {
        ULONG Csts = NvmeRead32(Adapter, NVME_REG_CSTS);

        if (Csts == 0xFFFFFFFF)
            return FALSE;
        if (Ready && (Csts & NVME_CSTS_CFS))
            return FALSE;
        if (((Csts & NVME_CSTS_RDY) != 0) == Ready)
            return TRUE;
        ScsiPortStallExecution(1000);
    }
    return FALSE;
}

static BOOLEAN NvmeEnableController(PNVME_ADAPTER Adapter)
{
    ULONG Cc = NvmeRead32(Adapter, NVME_REG_CC);

    /* Reset the controller if firmware left it running. */
    if (Cc & NVME_CC_EN)
    {
        NvmeWrite32(Adapter, NVME_REG_CC, Cc & ~NVME_CC_EN);
    }
    if (!NvmeWaitReady(Adapter, FALSE))
    {
        NvmeDebugPrint("controller did not stop, CSTS=%08lx\n",
                       NvmeRead32(Adapter, NVME_REG_CSTS));
        return FALSE;
    }

    NvmeWrite32(Adapter, NVME_REG_AQA,
                ((NVME_QUEUE_DEPTH - 1) << 16) | (NVME_QUEUE_DEPTH - 1));
    NvmeWrite64(Adapter, NVME_REG_ASQ, Adapter->AdminSq.Physical);
    NvmeWrite64(Adapter, NVME_REG_ACQ, Adapter->AdminCq.Physical);
    NvmeWrite32(Adapter, NVME_REG_CC,
                NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_AMS_RR |
                NVME_CC_IOSQES(6) | NVME_CC_IOCQES(4));

    if (!NvmeWaitReady(Adapter, TRUE))
    {
        NvmeDebugPrint("controller did not start, CSTS=%08lx\n",
                       NvmeRead32(Adapter, NVME_REG_CSTS));
        return FALSE;
    }

    /* Polled until HwInitialize unmasks it. */
    NvmeWrite32(Adapter, NVME_REG_INTMS, 1);
    return TRUE;
}

static VOID NvmeCopyIdString(PCHAR Dest, const UCHAR *Source, ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++)
        Dest[i] = (Source[i] >= 0x20 && Source[i] < 0x7F) ? (CHAR)Source[i] : ' ';
    Dest[Length] = '\0';
    while (Length > 0 && Dest[Length - 1] == ' ')
        Dest[--Length] = '\0';
}

static BOOLEAN NvmeIdentify(PNVME_ADAPTER Adapter)
{
    NVME_SQE Command;
    PUCHAR Id = Adapter->IdentifyBuffer;
    USHORT Status;
    ULONG Flbas, Lbaf, Lbads;
    ULONGLONG Nsze;

    /* Identify Controller: names and the largest transfer. */
    RtlZeroMemory(&Command, sizeof(Command));
    Command.Cdw0 = NVME_MAKE_CDW0(NVME_ADMIN_IDENTIFY, 0);
    Command.Prp1 = Adapter->IdentifyPhysical;
    Command.Cdw10 = NVME_IDENTIFY_CNS_CTRL;
    RtlZeroMemory(Id, NVME_PAGE_SIZE);
    Status = NvmeAdminCommand(Adapter, &Command);
    if (Status != 0)
    {
        NvmeDebugPrint("identify controller failed, status %x\n", Status);
        return FALSE;
    }

    NvmeCopyIdString(Adapter->Serial, Id + 4, 20);
    NvmeCopyIdString(Adapter->Model, Id + 24, 40);
    NvmeCopyIdString(Adapter->Firmware, Id + 64, 8);
    Adapter->MaxTransfer = NVME_MAX_TRANSFER;
    if (Id[77] != 0 && Id[77] < 20 &&
        (NVME_PAGE_SIZE << Id[77]) < Adapter->MaxTransfer)
    {
        Adapter->MaxTransfer = NVME_PAGE_SIZE << Id[77];
    }

    /* Identify Namespace 1: size and block format. */
    RtlZeroMemory(&Command, sizeof(Command));
    Command.Cdw0 = NVME_MAKE_CDW0(NVME_ADMIN_IDENTIFY, 0);
    Command.Nsid = NVME_NSID;
    Command.Prp1 = Adapter->IdentifyPhysical;
    Command.Cdw10 = NVME_IDENTIFY_CNS_NS;
    RtlZeroMemory(Id, NVME_PAGE_SIZE);
    Status = NvmeAdminCommand(Adapter, &Command);
    if (Status != 0)
    {
        NvmeDebugPrint("identify namespace failed, status %x\n", Status);
        return FALSE;
    }

    Nsze = *(ULONGLONG UNALIGNED *)(Id + 0);
    Flbas = Id[26] & 0xF;
    Lbaf = *(ULONG UNALIGNED *)(Id + 128 + 4 * Flbas);
    Lbads = (Lbaf >> 16) & 0xFF;
    if (Nsze == 0 || Lbads < 9 || Lbads > 12)
    {
        NvmeDebugPrint("namespace 1 unusable: %I64u blocks, LBADS %lu\n", Nsze, Lbads);
        return FALSE;
    }

    Adapter->BlockCount = Nsze;
    Adapter->BlockSize = 1UL << Lbads;
    NvmeDebugPrint("%s (%s, fw %s): %I64u blocks of %lu bytes, max transfer %lu\n",
                   Adapter->Model, Adapter->Serial, Adapter->Firmware,
                   Adapter->BlockCount, Adapter->BlockSize, Adapter->MaxTransfer);
    return TRUE;
}

static BOOLEAN NvmeCreateIoQueues(PNVME_ADAPTER Adapter)
{
    NVME_SQE Command;
    USHORT Status;

    /* The completion queue first: the submission queue names it. */
    RtlZeroMemory(&Command, sizeof(Command));
    Command.Cdw0 = NVME_MAKE_CDW0(NVME_ADMIN_CREATE_IO_CQ, 0);
    Command.Prp1 = Adapter->IoCq.Physical;
    Command.Cdw10 = ((NVME_QUEUE_DEPTH - 1) << 16) | Adapter->IoCq.Qid;
    Command.Cdw11 = (0 << 16) | (1 << 1) | 1;     /* vector 0, IEN, PC */
    Status = NvmeAdminCommand(Adapter, &Command);
    if (Status != 0)
    {
        NvmeDebugPrint("create I/O completion queue failed, status %x\n", Status);
        return FALSE;
    }

    RtlZeroMemory(&Command, sizeof(Command));
    Command.Cdw0 = NVME_MAKE_CDW0(NVME_ADMIN_CREATE_IO_SQ, 0);
    Command.Prp1 = Adapter->IoSq.Physical;
    Command.Cdw10 = ((NVME_QUEUE_DEPTH - 1) << 16) | Adapter->IoSq.Qid;
    Command.Cdw11 = ((ULONG)Adapter->IoCq.Qid << 16) | 1;   /* CQID, PC */
    Status = NvmeAdminCommand(Adapter, &Command);
    if (Status != 0)
    {
        NvmeDebugPrint("create I/O submission queue failed, status %x\n", Status);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* SCSI emulation helpers                                                   */
/* ------------------------------------------------------------------------ */

static VOID NvmeSetSense(PSCSI_REQUEST_BLOCK Srb, UCHAR Key, UCHAR Asc, UCHAR Ascq)
{
    PSENSE_DATA Sense = (PSENSE_DATA)Srb->SenseInfoBuffer;

    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    Srb->DataTransferLength = 0;

    if (Sense && Srb->SenseInfoBufferLength >= sizeof(SENSE_DATA) &&
        !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE))
    {
        RtlZeroMemory(Sense, sizeof(SENSE_DATA));
        Sense->ErrorCode = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
        Sense->SenseKey = Key;
        Sense->AdditionalSenseLength = sizeof(SENSE_DATA) - 8;
        Sense->AdditionalSenseCode = Asc;
        Sense->AdditionalSenseCodeQualifier = Ascq;
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }
}

static VOID NvmeInvalidField(PSCSI_REQUEST_BLOCK Srb)
{
    NvmeSetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
}

/* Copy emulated data to the caller's buffer, truncated to what it asked for. */
static VOID NvmeReturnData(PSCSI_REQUEST_BLOCK Srb, const VOID *Data, ULONG Length)
{
    if (Length > Srb->DataTransferLength)
        Length = Srb->DataTransferLength;
    if (Length)
        ScsiPortMoveMemory(Srb->DataBuffer, (PVOID)Data, Length);
    Srb->DataTransferLength = Length;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
}

static VOID NvmeCopyPadded(PUCHAR Dest, PCSTR Source, ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++)
        Dest[i] = (UCHAR)(*Source ? *Source++ : ' ');
}

static VOID NvmeInquiry(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR Cdb = Srb->Cdb;
    UCHAR Data[64];

    RtlZeroMemory(Data, sizeof(Data));

    if (Cdb[1] & 1)
    {
        /* Vital product data */
        switch (Cdb[2])
        {
            case 0x00:  /* supported pages */
                Data[3] = 2;
                Data[4] = 0x00;
                Data[5] = 0x80;
                NvmeReturnData(Srb, Data, 6);
                return;

            case 0x80:  /* unit serial number */
            {
                ULONG Length = (ULONG)strlen(Adapter->Serial);
                Data[1] = 0x80;
                Data[3] = (UCHAR)Length;
                ScsiPortMoveMemory(Data + 4, Adapter->Serial, Length);
                NvmeReturnData(Srb, Data, 4 + Length);
                return;
            }

            default:
                NvmeInvalidField(Srb);
                return;
        }
    }

    if (Cdb[2] != 0)
    {
        NvmeInvalidField(Srb);
        return;
    }

    Data[0] = DIRECT_ACCESS_DEVICE;
    Data[2] = 5;                    /* SPC-3 */
    Data[3] = 2;                    /* response data format */
    Data[4] = 36 - 5;               /* additional length */
    NvmeCopyPadded(Data + 8, "NVMe", 8);
    NvmeCopyPadded(Data + 16, Adapter->Model, 16);
    NvmeCopyPadded(Data + 32, Adapter->Firmware, 4);
    NvmeReturnData(Srb, Data, 36);
}

static VOID NvmeReadCapacity(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb, BOOLEAN Long)
{
    ULONGLONG LastLba = Adapter->BlockCount - 1;
    ULONG BlockSize = Adapter->BlockSize;
    UCHAR Data[32];
    ULONG i;

    RtlZeroMemory(Data, sizeof(Data));

    if (!Long)
    {
        ULONG Last = (LastLba > 0xFFFFFFFFULL) ? 0xFFFFFFFF : (ULONG)LastLba;
        for (i = 0; i < 4; i++)
        {
            Data[i] = (UCHAR)(Last >> (24 - 8 * i));
            Data[4 + i] = (UCHAR)(BlockSize >> (24 - 8 * i));
        }
        NvmeReturnData(Srb, Data, 8);
        return;
    }

    for (i = 0; i < 8; i++)
        Data[i] = (UCHAR)(LastLba >> (56 - 8 * i));
    for (i = 0; i < 4; i++)
        Data[8 + i] = (UCHAR)(BlockSize >> (24 - 8 * i));
    NvmeReturnData(Srb, Data, 32);
}

static VOID NvmeModeSense(PSCSI_REQUEST_BLOCK Srb, BOOLEAN Ten)
{
    UCHAR Data[8];

    /* A header with no block descriptor and no pages: not write
       protected, and no pages the class driver can change. */
    RtlZeroMemory(Data, sizeof(Data));
    if (Ten)
    {
        Data[1] = 6;
        NvmeReturnData(Srb, Data, 8);
    }
    else
    {
        Data[0] = 3;
        NvmeReturnData(Srb, Data, 4);
    }
}

static ULONG NvmeBe32(const UCHAR *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static ULONGLONG NvmeBe64(const UCHAR *p)
{
    return ((ULONGLONG)NvmeBe32(p) << 32) | NvmeBe32(p + 4);
}

/* Decode a READ/WRITE/VERIFY/SYNCHRONIZE CACHE CDB. */
static BOOLEAN NvmeDecodeRange(PUCHAR Cdb, PULONGLONG Lba, PULONG Blocks)
{
    switch (Cdb[0])
    {
        case SCSIOP_READ6:
        case SCSIOP_WRITE6:
            *Lba = ((ULONG)(Cdb[1] & 0x1F) << 16) | ((ULONG)Cdb[2] << 8) | Cdb[3];
            *Blocks = Cdb[4] ? Cdb[4] : 256;
            return TRUE;

        case SCSIOP_READ:
        case SCSIOP_WRITE:
        case SCSIOP_VERIFY:
        case SCSIOP_SYNCHRONIZE_CACHE:
            *Lba = NvmeBe32(Cdb + 2);
            *Blocks = ((ULONG)Cdb[7] << 8) | Cdb[8];
            return TRUE;

        case SCSIOP_READ12:
        case SCSIOP_WRITE12:
            *Lba = NvmeBe32(Cdb + 2);
            *Blocks = NvmeBe32(Cdb + 6);
            return TRUE;

        case SCSIOP_READ16:
        case SCSIOP_WRITE16:
        case SCSIOP_VERIFY16:
        case SCSIOP_SYNCHRONIZE_CACHE16:
            *Lba = NvmeBe64(Cdb + 2);
            *Blocks = NvmeBe32(Cdb + 10);
            return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------ */
/* I/O                                                                      */
/* ------------------------------------------------------------------------ */

/* Fill in PRP1/PRP2 for Length bytes of the SRB's data buffer. */
static BOOLEAN NvmeBuildPrps(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb,
                             ULONG Length, PNVME_SQE Command)
{
    PUCHAR Va = (PUCHAR)Srb->DataBuffer;
    ULONG ElementLength;
    ULONGLONG Pa;
    ULONG Offset, Count = 0;

    Pa = NVME_PA(
             ScsiPortGetPhysicalAddress(Adapter, Srb, Va, &ElementLength));
    Command->Prp1 = Pa;
    Offset = NVME_PAGE_SIZE - (ULONG)(Pa & (NVME_PAGE_SIZE - 1));

    /* Every later PRP names a whole page. */
    while (Offset < Length)
    {
        Pa = NVME_PA(
                 ScsiPortGetPhysicalAddress(Adapter, Srb, Va + Offset, &ElementLength));
        if ((Pa & (NVME_PAGE_SIZE - 1)) != 0 || Count >= NVME_MAX_PRP)
        {
            NvmeDebugPrint("buffer at %p+%lx not page aligned (%I64x)\n", Va, Offset, Pa);
            return FALSE;
        }
        Adapter->PrpList[Count++] = Pa;
        Offset += NVME_PAGE_SIZE;
    }

    if (Count == 0)
        Command->Prp2 = 0;
    else if (Count == 1)
        Command->Prp2 = Adapter->PrpList[0];
    else
        Command->Prp2 = Adapter->PrpListPhysical;
    return TRUE;
}

static VOID NvmeStartCommand(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb, PNVME_SQE Command)
{
    Adapter->CurrentSrb = Srb;
    if (++Adapter->NextCid == 0xFFFF)
        Adapter->NextCid = 0;
    Command->Cdw0 = (Command->Cdw0 & 0xFFFF) | ((ULONG)Adapter->NextCid << 16);
    Command->Nsid = NVME_NSID;
    NvmeSubmit(Adapter, &Adapter->IoSq, Command);
    ScsiPortNotification(RequestTimerCall, Adapter, NvmeTimer, NVME_POLL_INTERVAL_US);
}

/* Queue a read or write. Returns FALSE when the SRB was completed here. */
static BOOLEAN NvmeReadWrite(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb, BOOLEAN Write)
{
    NVME_SQE Command;
    ULONGLONG Lba;
    ULONG Blocks, Length;

    NvmeDecodeRange(Srb->Cdb, &Lba, &Blocks);

    if (Lba >= Adapter->BlockCount || Blocks > Adapter->BlockCount - Lba)
    {
        NvmeSetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_BLOCK, 0);
        return FALSE;
    }
    if (Blocks == 0)
    {
        Srb->DataTransferLength = 0;
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        return FALSE;
    }

    Length = Blocks * Adapter->BlockSize;
    if (Length > Srb->DataTransferLength || Length > Adapter->MaxTransfer ||
        Blocks > 0x10000)
    {
        NvmeInvalidField(Srb);
        return FALSE;
    }

    RtlZeroMemory(&Command, sizeof(Command));
    Command.Cdw0 = NVME_MAKE_CDW0(Write ? NVME_NVM_WRITE : NVME_NVM_READ, 0);
    Command.Cdw10 = (ULONG)Lba;
    Command.Cdw11 = (ULONG)(Lba >> 32);
    Command.Cdw12 = Blocks - 1;
    if (!NvmeBuildPrps(Adapter, Srb, Length, &Command))
    {
        Srb->SrbStatus = SRB_STATUS_ERROR;
        return FALSE;
    }

    Srb->DataTransferLength = Length;
    NvmeStartCommand(Adapter, Srb, &Command);
    return TRUE;
}

static BOOLEAN NvmeFlush(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb)
{
    NVME_SQE Command;

    RtlZeroMemory(&Command, sizeof(Command));
    Command.Cdw0 = NVME_MAKE_CDW0(NVME_NVM_FLUSH, 0);
    Srb->DataTransferLength = 0;
    NvmeStartCommand(Adapter, Srb, &Command);
    return TRUE;
}

static VOID NvmeCompleteSrb(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb)
{
    ScsiPortNotification(RequestComplete, Adapter, Srb);
    ScsiPortNotification(NextRequest, Adapter, NULL);
}

/*
 * Retire what the controller posted on the I/O completion queue. Called from
 * the interrupt and from the miniport timer; the Reaping flag keeps the two
 * from running at once on different processors.
 */
static BOOLEAN NvmeReapIoQueue(PNVME_ADAPTER Adapter)
{
    PSCSI_REQUEST_BLOCK Srb;
    USHORT Cid, Status;
    BOOLEAN Found = FALSE;

    if (InterlockedCompareExchange(&Adapter->Reaping, 1, 0) != 0)
        return FALSE;

    while (NvmeNextCompletion(&Adapter->IoCq, &Cid, &Status))
    {
        Found = TRUE;
        Srb = Adapter->CurrentSrb;
        if (!Srb || Cid != Adapter->NextCid)
            continue;

        Adapter->CurrentSrb = NULL;
        if (Status == 0)
        {
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
        }
        else
        {
            NvmeDebugPrint("command %02x failed, status %x\n", Srb->Cdb[0], Status);
            NvmeSetSense(Srb, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0);
        }
        ScsiPortNotification(RequestTimerCall, Adapter, NvmeTimer, 0);
        NvmeCompleteSrb(Adapter, Srb);
    }

    if (Found)
        NvmeRingDoorbell(Adapter, &Adapter->IoCq, TRUE);

    InterlockedExchange(&Adapter->Reaping, 0);
    return Found;
}

VOID NTAPI NvmeTimer(PVOID DeviceExtension)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;

    if (!Adapter->CurrentSrb)
        return;

    /* The timer runs at DISPATCH_LEVEL, not synchronized with the interrupt.
       Mask the controller's interrupt meanwhile: an interrupt that found the
       queue busy would otherwise keep a level-triggered line asserted on
       this processor and never let the timer finish. */
    NvmeWrite32(Adapter, NVME_REG_INTMS, 1);
    NvmeReapIoQueue(Adapter);
    NvmeWrite32(Adapter, NVME_REG_INTMC, 1);
    if (Adapter->CurrentSrb)
        ScsiPortNotification(RequestTimerCall, Adapter, NvmeTimer, NVME_POLL_INTERVAL_US);
}

/* ------------------------------------------------------------------------ */
/* scsiport entry points                                                    */
/* ------------------------------------------------------------------------ */

static BOOLEAN NTAPI NvmeHwInterrupt(PVOID DeviceExtension)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;

    if (!Adapter->Ready)
        return FALSE;
    return NvmeReapIoQueue(Adapter);
}

static BOOLEAN NTAPI NvmeHwStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;
    PUCHAR Cdb = Srb->Cdb;
    BOOLEAN Queued = FALSE;
    ULONGLONG Lba;
    ULONG Blocks;

    Srb->ScsiStatus = SCSISTAT_GOOD;

    switch (Srb->Function)
    {
        case SRB_FUNCTION_EXECUTE_SCSI:
            break;

        case SRB_FUNCTION_FLUSH:
        case SRB_FUNCTION_SHUTDOWN:
            if (Adapter->Ready)
                Queued = NvmeFlush(Adapter, Srb);
            else
                Srb->SrbStatus = SRB_STATUS_SUCCESS;
            goto Done;

        case SRB_FUNCTION_RESET_BUS:
        case SRB_FUNCTION_RESET_DEVICE:
        case SRB_FUNCTION_ABORT_COMMAND:
        case SRB_FUNCTION_RELEASE_QUEUE:
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            goto Done;

        default:
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            goto Done;
    }

    if (Srb->PathId != 0 || Srb->TargetId != 0 || Srb->Lun != 0 || !Adapter->Ready)
    {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        goto Done;
    }

    switch (Cdb[0])
    {
        case SCSIOP_INQUIRY:
            NvmeInquiry(Adapter, Srb);
            break;

        case SCSIOP_READ_CAPACITY:
            NvmeReadCapacity(Adapter, Srb, FALSE);
            break;

        case SCSIOP_READ_CAPACITY16:        /* SERVICE ACTION IN (16) */
            if ((Cdb[1] & 0x1F) == 0x10)
                NvmeReadCapacity(Adapter, Srb, TRUE);
            else
                NvmeInvalidField(Srb);
            break;

        case SCSIOP_READ6:
        case SCSIOP_READ:
        case SCSIOP_READ12:
        case SCSIOP_READ16:
            Queued = NvmeReadWrite(Adapter, Srb, FALSE);
            break;

        case SCSIOP_WRITE6:
        case SCSIOP_WRITE:
        case SCSIOP_WRITE12:
        case SCSIOP_WRITE16:
            Queued = NvmeReadWrite(Adapter, Srb, TRUE);
            break;

        case SCSIOP_SYNCHRONIZE_CACHE:
        case SCSIOP_SYNCHRONIZE_CACHE16:
            Queued = NvmeFlush(Adapter, Srb);
            break;

        case SCSIOP_VERIFY:
        case SCSIOP_VERIFY16:
            NvmeDecodeRange(Cdb, &Lba, &Blocks);
            if (Lba >= Adapter->BlockCount || Blocks > Adapter->BlockCount - Lba)
                NvmeSetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_BLOCK, 0);
            else
                Srb->SrbStatus = SRB_STATUS_SUCCESS;
            Srb->DataTransferLength = 0;
            break;

        case SCSIOP_TEST_UNIT_READY:
        case SCSIOP_START_STOP_UNIT:
        case SCSIOP_MEDIUM_REMOVAL:
            Srb->DataTransferLength = 0;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;

        case SCSIOP_MODE_SENSE:
            NvmeModeSense(Srb, FALSE);
            break;

        case SCSIOP_MODE_SENSE10:
            NvmeModeSense(Srb, TRUE);
            break;

        case SCSIOP_REQUEST_SENSE:
        {
            SENSE_DATA Sense;
            RtlZeroMemory(&Sense, sizeof(Sense));
            Sense.ErrorCode = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
            Sense.AdditionalSenseLength = sizeof(Sense) - 8;
            NvmeReturnData(Srb, &Sense, sizeof(Sense));
            break;
        }

        case SCSIOP_REPORT_LUNS:
        {
            UCHAR Data[16];
            RtlZeroMemory(Data, sizeof(Data));
            Data[3] = 8;                /* one LUN, LUN 0 */
            NvmeReturnData(Srb, Data, sizeof(Data));
            break;
        }

        default:
            NvmeSetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_COMMAND, 0);
            break;
    }

Done:
    if (!Queued)
        NvmeCompleteSrb(Adapter, Srb);
    return TRUE;
}

static BOOLEAN NTAPI NvmeHwResetBus(PVOID DeviceExtension, ULONG PathId)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;

    NvmeDebugPrint("bus reset\n");
    if (Adapter->CurrentSrb)
    {
        Adapter->CurrentSrb = NULL;
        ScsiPortNotification(RequestTimerCall, Adapter, NvmeTimer, 0);
        ScsiPortCompleteRequest(Adapter, (UCHAR)PathId, SP_UNTAGGED, SP_UNTAGGED,
                                SRB_STATUS_BUS_RESET);
    }
    ScsiPortNotification(NextRequest, Adapter, NULL);
    return TRUE;
}

static BOOLEAN NTAPI NvmeHwInitialize(PVOID DeviceExtension)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;

    /* Start taking completion interrupts. */
    NvmeWrite32(Adapter, NVME_REG_INTMC, 1);
    Adapter->Ready = TRUE;
    return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS NTAPI
NvmeHwAdapterControl(PVOID DeviceExtension,
                     SCSI_ADAPTER_CONTROL_TYPE ControlType,
                     PVOID Parameters)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;

    switch (ControlType)
    {
        case ScsiQuerySupportedControlTypes:
        {
            PSCSI_SUPPORTED_CONTROL_TYPE_LIST List = Parameters;
            if (List->MaxControlType > ScsiQuerySupportedControlTypes)
                List->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
            if (List->MaxControlType > ScsiStopAdapter)
                List->SupportedTypeList[ScsiStopAdapter] = TRUE;
            return ScsiAdapterControlSuccess;
        }

        case ScsiStopAdapter:
        {
            /* Normal shutdown notification, so the drive saves its state. */
            ULONG Waited;
            Adapter->Ready = FALSE;
            NvmeWrite32(Adapter, NVME_REG_INTMS, 1);
            NvmeWrite32(Adapter, NVME_REG_CC,
                        (NvmeRead32(Adapter, NVME_REG_CC) & ~NVME_CC_SHN_MASK) |
                        NVME_CC_SHN_NORMAL);
            for (Waited = 0; Waited < 2000; Waited++)
            {
                if ((NvmeRead32(Adapter, NVME_REG_CSTS) & NVME_CSTS_SHST_MASK) ==
                    NVME_CSTS_SHST_DONE)
                    break;
                ScsiPortStallExecution(1000);
            }
            return ScsiAdapterControlSuccess;
        }

        default:
            return ScsiAdapterControlUnsuccessful;
    }
}

static VOID NvmeInitQueue(PNVME_QUEUE Queue, PVOID Entries, ULONGLONG Physical, USHORT Qid)
{
    Queue->Entries = Entries;
    Queue->Physical = Physical;
    Queue->Qid = Qid;
    Queue->Index = 0;
    Queue->Phase = 1;
}

static ULONG NTAPI NvmeHwFindAdapter(PVOID DeviceExtension,
                                     PVOID HwContext,
                                     PVOID BusInformation,
                                     PCHAR ArgumentString,
                                     PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                                     PBOOLEAN Again)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;
    PACCESS_RANGE Range = NULL;
    ULONGLONG Cap, Physical;
    PUCHAR Memory;
    ULONG i, Length;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);
    *Again = FALSE;

    /* BAR0 is the first memory range. It is often a 64-bit BAR placed above
       4 GB, so take it from the translated resources, not from config space. */
    for (i = 0; i < ConfigInfo->NumberOfAccessRanges; i++)
    {
        PACCESS_RANGE Candidate = &(*ConfigInfo->AccessRanges)[i];
        if (Candidate->RangeInMemory && Candidate->RangeLength >= 0x1000 &&
            NVME_PA(Candidate->RangeStart) != 0)
        {
            Range = Candidate;
            break;
        }
    }
    if (!Range)
    {
        NvmeDebugPrint("no register range\n");
        return SP_RETURN_NOT_FOUND;
    }

    /* scsiport's legacy PCI scan assigns resources but does not turn on bus
       mastering, and the controller cannot fetch a single command without it. */
    {
        PCI_COMMON_CONFIG Pci;

        if (ScsiPortGetBusData(Adapter, PCIConfiguration, ConfigInfo->SystemIoBusNumber,
                               ConfigInfo->SlotNumber, &Pci, PCI_COMMON_HDR_LENGTH) >=
            PCI_COMMON_HDR_LENGTH &&
            (Pci.Command & (PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER)) !=
            (PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER))
        {
            Pci.Command |= PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER;
            ScsiPortSetBusDataByOffset(Adapter, PCIConfiguration, ConfigInfo->SystemIoBusNumber,
                                       ConfigInfo->SlotNumber, &Pci.Command,
                                       FIELD_OFFSET(PCI_COMMON_CONFIG, Command),
                                       sizeof(Pci.Command));
        }
    }

    Adapter->Registers = ScsiPortGetDeviceBase(Adapter,
                                               ConfigInfo->AdapterInterfaceType,
                                               ConfigInfo->SystemIoBusNumber,
                                               Range->RangeStart,
                                               Range->RangeLength,
                                               FALSE);
    if (!Adapter->Registers)
    {
        NvmeDebugPrint("could not map the registers\n");
        return SP_RETURN_ERROR;
    }

    Cap = NvmeRead64(Adapter, NVME_REG_CAP);
    NvmeDebugPrint("registers at %I64x, CAP %I64x, version %08lx\n",
                   NVME_PA(Range->RangeStart),
                   Cap, NvmeRead32(Adapter, NVME_REG_VS));
    if (Cap == ~0ULL || NVME_CAP_MPSMIN(Cap) != 0 || NVME_CAP_MQES(Cap) + 1 < NVME_QUEUE_DEPTH)
    {
        NvmeDebugPrint("unsupported controller\n");
        return SP_RETURN_NOT_FOUND;
    }
    Adapter->DoorbellStride = 4UL << NVME_CAP_DSTRD(Cap);
    Adapter->ReadyTimeoutUs = max(NVME_CAP_TO(Cap), 1) * 500 * 1000;

    /* Describe the adapter before asking for DMA memory. */
    ConfigInfo->MaximumTransferLength = NVME_MAX_TRANSFER;
    ConfigInfo->NumberOfPhysicalBreaks = NVME_MAX_PRP;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->Master = TRUE;
    ConfigInfo->CachesData = FALSE;
    ConfigInfo->AlignmentMask = 3;
    ConfigInfo->Dma32BitAddresses = TRUE;
    ConfigInfo->Dma64BitAddresses = SCSI_DMA64_MINIPORT_SUPPORTED;
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->MaximumNumberOfTargets = 1;
    ConfigInfo->MaximumNumberOfLogicalUnits = 1;
    ConfigInfo->InitiatorBusId[0] = 1;
    ConfigInfo->BufferAccessScsiPortControlled = TRUE;

    /* Queues, identify buffer and PRP list, one page each, page aligned. */
    Length = 7 * NVME_PAGE_SIZE;
    Memory = ScsiPortGetUncachedExtension(Adapter, ConfigInfo, Length);
    if (!Memory)
    {
        NvmeDebugPrint("no uncached memory\n");
        return SP_RETURN_ERROR;
    }
    Memory = (PUCHAR)(((ULONG_PTR)Memory + NVME_PAGE_SIZE - 1) & ~((ULONG_PTR)NVME_PAGE_SIZE - 1));
    RtlZeroMemory(Memory, 6 * NVME_PAGE_SIZE);

    for (i = 0; i < 6; i++)
    {
        PUCHAR Page = Memory + i * NVME_PAGE_SIZE;
        ULONG PageLength;

        Physical = NVME_PA(
                       ScsiPortGetPhysicalAddress(Adapter, NULL, Page, &PageLength));
        if (Physical == 0 || (Physical & (NVME_PAGE_SIZE - 1)) || PageLength < NVME_PAGE_SIZE)
        {
            NvmeDebugPrint("uncached page %lu unusable (%I64x)\n", i, Physical);
            return SP_RETURN_ERROR;
        }

        switch (i)
        {
            case 0: NvmeInitQueue(&Adapter->AdminSq, Page, Physical, 0); break;
            case 1: NvmeInitQueue(&Adapter->AdminCq, Page, Physical, 0); break;
            case 2: NvmeInitQueue(&Adapter->IoSq, Page, Physical, 1); break;
            case 3: NvmeInitQueue(&Adapter->IoCq, Page, Physical, 1); break;
            case 4: Adapter->IdentifyBuffer = Page; Adapter->IdentifyPhysical = Physical; break;
            case 5: Adapter->PrpList = (PULONGLONG)Page; Adapter->PrpListPhysical = Physical; break;
        }
    }

    if (!NvmeEnableController(Adapter) ||
        !NvmeIdentify(Adapter) ||
        !NvmeCreateIoQueues(Adapter))
    {
        return SP_RETURN_ERROR;
    }

    ConfigInfo->MaximumTransferLength = Adapter->MaxTransfer;
    ConfigInfo->NumberOfPhysicalBreaks = Adapter->MaxTransfer / NVME_PAGE_SIZE + 1;
    return SP_RETURN_FOUND;
}

/*
 * scsiport only drives legacy miniports, which it finds by scanning PCI for a
 * vendor and device ID. NVMe controllers are recognised by class code
 * (01/08/02) instead, so find them here and hand scsiport each distinct ID.
 */
#define NVME_MAX_IDS 8

static VOID NvmeHex4(PCHAR Out, USHORT Value)
{
    static const CHAR Digits[] = "0123456789abcdef";
    ULONG i;

    for (i = 0; i < 4; i++)
        Out[i] = Digits[(Value >> (12 - 4 * i)) & 0xF];
    Out[4] = '\0';
}

ULONG NTAPI DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    static CHAR VendorIds[NVME_MAX_IDS][5], DeviceIds[NVME_MAX_IDS][5];
    HW_INITIALIZATION_DATA HwInit;
    PCI_SLOT_NUMBER Slot;
    UCHAR Header[16];
    ULONG Bus, Device, Function, i, Count = 0;
    ULONG Status, Result = (ULONG)STATUS_NO_SUCH_DEVICE;

    for (Bus = 0; Bus < 256; Bus++)
    {
        for (Device = 0; Device < PCI_MAX_DEVICES; Device++)
        {
            for (Function = 0; Function < PCI_MAX_FUNCTION; Function++)
            {
                USHORT VendorId, DeviceId;
                CHAR Vendor[5], Dev[5];
                ULONG Length;

                Slot.u.AsULONG = 0;
                Slot.u.bits.DeviceNumber = Device;
                Slot.u.bits.FunctionNumber = Function;
                Length = HalGetBusData(PCIConfiguration, Bus, Slot.u.AsULONG,
                                       Header, sizeof(Header));
                if (Length == 0)
                    goto BusesDone;     /* no such bus */
                VendorId = *(PUSHORT)&Header[0];
                DeviceId = *(PUSHORT)&Header[2];
                if (Length < sizeof(Header) || VendorId == PCI_INVALID_VENDORID)
                {
                    if (Function == 0)
                        break;
                    continue;
                }
                if (Header[11] == 0x01 && Header[10] == 0x08 && Header[9] == 0x02)
                {
                    /* Same format scsiport uses when it compares */
                    NvmeHex4(Vendor, VendorId);
                    NvmeHex4(Dev, DeviceId);
                    for (i = 0; i < Count; i++)
                    {
                        if (!strcmp(VendorIds[i], Vendor) && !strcmp(DeviceIds[i], Dev))
                            break;
                    }
                    if (i == Count && Count < NVME_MAX_IDS)
                    {
                        strcpy(VendorIds[Count], Vendor);
                        strcpy(DeviceIds[Count], Dev);
                        Count++;
                    }
                }
                if (Function == 0 && !(Header[14] & 0x80))
                    break;              /* single-function device */
            }
        }
    }
BusesDone:

    for (i = 0; i < Count; i++)
    {
        NvmeDebugPrint("scanning for NVMe controllers %s:%s\n", VendorIds[i], DeviceIds[i]);

        RtlZeroMemory(&HwInit, sizeof(HwInit));
        HwInit.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
        HwInit.AdapterInterfaceType = PCIBus;
        HwInit.HwInitialize = NvmeHwInitialize;
        HwInit.HwStartIo = NvmeHwStartIo;
        HwInit.HwInterrupt = NvmeHwInterrupt;
        HwInit.HwFindAdapter = NvmeHwFindAdapter;
        HwInit.HwResetBus = NvmeHwResetBus;
        HwInit.HwAdapterControl = NvmeHwAdapterControl;
        HwInit.DeviceExtensionSize = sizeof(NVME_ADAPTER);
        HwInit.SrbExtensionSize = sizeof(NVME_SRB_EXTENSION);
        HwInit.NumberOfAccessRanges = 6;
        HwInit.MapBuffers = TRUE;
        HwInit.NeedPhysicalAddresses = TRUE;
        HwInit.TaggedQueuing = FALSE;
        HwInit.AutoRequestSense = TRUE;
        HwInit.MultipleRequestPerLu = FALSE;
        HwInit.VendorId = VendorIds[i];
        HwInit.VendorIdLength = 4;
        HwInit.DeviceId = DeviceIds[i];
        HwInit.DeviceIdLength = 4;

        Status = ScsiPortInitialize(DriverObject, RegistryPath, &HwInit, NULL);
        if (Status == STATUS_SUCCESS)
            Result = STATUS_SUCCESS;
    }

    return Result;
}
