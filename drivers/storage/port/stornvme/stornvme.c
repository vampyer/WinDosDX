/*
 * PROJECT:        WinDosDX Storage Stack
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        NVMe (NVM Express) Storport Miniport
 * PROGRAMMERS:    WinDosDX Team
 *
 * A polled NVMe miniport for WinDosDX. Exposes namespace 1 as a single SCSI
 * logical unit so that the boot volume (and any NVMe disk) is usable without
 * the machine falling back to "no disk".
 */

#include "stornvme.h"

/* ------------------------------------------------------------------------ */
/* Register access helpers                                                     */
/* ------------------------------------------------------------------------ */

static __inline ULONG NvmeRead32(PVOID Base, ULONG Offset)
{
    return *(volatile ULONG *)((PUCHAR)Base + Offset);
}

static __inline VOID NvmeWrite32(PVOID Base, ULONG Offset, ULONG Value)
{
    *(volatile ULONG *)((PUCHAR)Base + Offset) = Value;
}

static __inline ULONGLONG NvmeRead64(PVOID Base, ULONG Offset)
{
    ULONGLONG Low, High;
    Low = NvmeRead32(Base, Offset);
    High = NvmeRead32(Base, Offset + 4);
    return Low | (High << 32);
}

static __inline VOID NvmeWrite64(PVOID Base, ULONG Offset, ULONGLONG Value)
{
    NvmeWrite32(Base, Offset, (ULONG)(Value & 0xFFFFFFFFULL));
    NvmeWrite32(Base, Offset + 4, (ULONG)(Value >> 32));
}

/* Doorbell offset for queue Qid (0 == admin). Stride is 4 << CAP.DSTRD. */
static __inline ULONG NvmeDoorbellOffset(PNVME_ADAPTER Adapter, ULONG Qid, BOOLEAN IsCq)
{
    ULONG Index = (Qid * 2) + (IsCq ? 1 : 0);
    ULONG Stride = 4u << Adapter->DoorbellStride;
    return NVME_REG_DOORBELL_BASE + Index * Stride;
}

static __inline VOID NvmeRingDoorbell(PNVME_ADAPTER Adapter, ULONG Qid, BOOLEAN IsCq, ULONG Value)
{
    NvmeWrite32(Adapter->Registers, NvmeDoorbellOffset(Adapter, Qid, IsCq), Value);
    KeMemoryBarrier();
}

static __inline BOOLEAN NvmeWaitCsts(PNVME_ADAPTER Adapter, BOOLEAN WantRdy)
{
    ULONG i;
    for (i = 0; i < 200000; i++)
    {
        ULONG Csts = NvmeRead32(Adapter->Registers, NVME_REG_CSTS);
        if (WantRdy ? ((Csts & NVME_CSTS_RDY) != 0) : ((Csts & NVME_CSTS_RDY) == 0))
            return TRUE;
        KeStallExecutionProcessor(50);
    }
    return FALSE;
}

/* ------------------------------------------------------------------------ */
/* Submission queue management                                                 */
/* ------------------------------------------------------------------------ */

static PNVME_SQE NvmeAllocSq(PNVME_SUBMIT_QUEUE Sq)
{
    PNVME_SQE Sqe = &Sq->Entries[Sq->Tail];
    if (++Sq->Tail == NVME_IO_QUEUE_SIZE)
        Sq->Tail = 0;
    return Sqe;
}

static void NvmeSubmitSq(PNVME_ADAPTER Adapter, PNVME_SUBMIT_QUEUE Sq)
{
    NvmeRingDoorbell(Adapter, Sq->Qid, FALSE, Sq->Tail);
}

/* ------------------------------------------------------------------------ */
/* Completion queue reaping (polled)                                          */
/* ------------------------------------------------------------------------ */

/*
 * Reap the completion queue looking for CID. Returns TRUE if found, and fills
 * *Status with (SCT << 8) | SC so the caller can test for a non-zero status.
 */
static BOOLEAN NvmeWaitCompletion(PNVME_COMPLETION_QUEUE Cq, USHORT Cid, ULONG TimeoutMs, USHORT *Status)
{
    ULONG Spins = (TimeoutMs ? TimeoutMs : 5000) * 20;
    ULONG i;

    for (i = 0; i < Spins; i++)
    {
        while (Cq->Head != Cq->Tail)
        {
            NVME_CQE *Cqe = &Cq->Entries[Cq->Head];
            BOOLEAN Valid = (NvmeCqePhase(Cqe->Flags) == (USHORT)Cq->Phase);
            BOOLEAN Match = (Cqe->CID == Cid);
            ULONG Sc = NvmeCqeSC(Cqe->Flags);
            ULONG Sct = NvmeCqeSCT(Cqe->Flags);

            if (++Cq->Head == NVME_IO_QUEUE_SIZE)
            {
                Cq->Head = 0;
                Cq->Phase ^= 1;
            }

            if (Valid && Match)
            {
                *Status = (USHORT)((Sct << 8) | Sc);
                return TRUE;
            }
        }
        KeStallExecutionProcessor(100);
    }
    return FALSE;
}

/* Drain a completion queue without matching a CID. */
static void NvmeDrainCompletion(PNVME_COMPLETION_QUEUE Cq)
{
    while (Cq->Head != Cq->Tail)
    {
        if (++Cq->Head == NVME_IO_QUEUE_SIZE)
        {
            Cq->Head = 0;
            Cq->Phase ^= 1;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* PRP list construction from the storport scatter/gather list                */
/* ------------------------------------------------------------------------ */

static ULONGLONG *NvmeAllocPrpSlot(PNVME_ADAPTER Adapter)
{
    ULONGLONG *Slot = NULL;
    ULONG i;
    STOR_LOCK_HANDLE LockHandle = {0};

    StorPortAcquireSpinLock(Adapter, InterruptLock, Adapter, &LockHandle);
    for (i = 0; i < NVME_PRP_SLOT_COUNT; i++)
    {
        if (Adapter->FreePrpSlots & (1UL << i))
        {
            Adapter->FreePrpSlots &= ~(1UL << i);
            Slot = Adapter->PrpSlots[i];
            break;
        }
    }
    StorPortReleaseSpinLock(Adapter, &LockHandle);
    return Slot;
}

static void NvmeFreePrpSlot(PNVME_ADAPTER Adapter, ULONGLONG *Slot)
{
    ULONG i;
    STOR_LOCK_HANDLE LockHandle = {0};
    if (!Slot)
        return;
    StorPortAcquireSpinLock(Adapter, InterruptLock, Adapter, &LockHandle);
    for (i = 0; i < NVME_PRP_SLOT_COUNT; i++)
    {
        if (Adapter->PrpSlots[i] == Slot)
        {
            Adapter->FreePrpSlots |= (1UL << i);
            break;
        }
    }
    StorPortReleaseSpinLock(Adapter, &LockHandle);
}

/*
 * Walk the SG list, emitting one PRPH per physical page into Slot. Returns
 * the number of PRPs written, or 0 if the list could not be represented.
 */
static ULONG NvmeBuildPrpList(PSTOR_SCATTER_GATHER_LIST Sgl, ULONGLONG *Slot)
{
    ULONG Total = 0;
    ULONG i;

    for (i = 0; i < Sgl->NumberOfElements; i++)
    {
        ULONGLONG Pa = (ULONGLONG)Sgl->List[i].PhysicalAddress.QuadPart;
        ULONG Len = Sgl->List[i].Length;
        ULONG Off = (ULONG)(Pa & (NVME_PAGE_SIZE - 1));
        ULONGLONG Cur = Pa - Off;

        if (Off == 0 && Len >= NVME_PAGE_SIZE)
        {
            /* Page-aligned, possibly multi-page contiguous run. */
            ULONG Pages = Len / NVME_PAGE_SIZE;
            while (Pages--)
            {
                if (Total >= NVME_PRP_SLOT_ENTRIES)
                    return 0;
                Slot[Total++] = Cur;
                Cur += NVME_PAGE_SIZE;
            }
        }
        else
        {
            /*
             * Non page-aligned start (or short run): one PRP covers the page
             * containing the buffer, and subsequent pages follow.
             */
            ULONGLONG PaCur = Pa;
            ULONG Remain = Len;
            ULONG CurOff = Off;
            while (Remain)
            {
                ULONG Chunk = NVME_PAGE_SIZE - CurOff;
                if (Chunk > Remain)
                    Chunk = Remain;
                if (Total >= NVME_PRP_SLOT_ENTRIES)
                    return 0;
                Slot[Total++] = Cur;
                Remain -= Chunk;
                PaCur += Chunk;
                Cur += NVME_PAGE_SIZE;
                CurOff = 0;
            }
        }
    }
    return Total;
}

/* ------------------------------------------------------------------------ */
/* Controller initialization                                                    */
/* ------------------------------------------------------------------------ */

static BOOLEAN NvmeControllerEnable(PNVME_ADAPTER Adapter,
                                    ULONGLONG Asq, ULONGLONG Acq,
                                    ULONG AsqSize, ULONG AcqSize)
{
    /* CC must be 0 while EN=0. Disable interrupts, then stop the controller. */
    NvmeWrite32(Adapter->Registers, NVME_REG_INTMS, 0xFFFFFFFF);
    NvmeWrite32(Adapter->Registers, NVME_REG_CC, NVME_CC_SHN(NVME_CC_SHN_ABRUPT));
    if (!NvmeWaitCsts(Adapter, FALSE))
        return FALSE;

    NvmeWrite32(Adapter->Registers, NVME_REG_AQA,
                (AsqSize - 1) | ((AcqSize - 1) << 16));
    NvmeWrite64(Adapter->Registers, NVME_REG_ASQ, Asq);
    NvmeWrite64(Adapter->Registers, NVME_REG_ACQ, Acq);
    NvmeWrite32(Adapter->Registers, NVME_REG_CC,
                NVME_CC_EN | NVME_CC_CSS(0) | NVME_CC_MPS |
                NVME_CC_SQES(6) | NVME_CC_CQES(4));
    return NvmeWaitCsts(Adapter, TRUE);
}

static BOOLEAN NvmeIdentifyNamespace(PNVME_ADAPTER Adapter)
{
    PNVME_SQE Sqe;
    USHORT Status = 0;
    ULONGLONG *Id;
    ULONGLONG Ncap, Nuse;

    Id = (ULONGLONG *)Adapter->IdentifyBuffer;
    RtlZeroMemory(Id, NVME_PAGE_SIZE);

    Sqe = NvmeAllocSq(Adapter->AdminSq);
    RtlZeroMemory(Sqe, sizeof(*Sqe));
    Sqe->DW0 = NvmeSqeMakeDW0(NVME_ADMIN_IDENTIFY, 0, 0x101);
    Sqe->NSID = 0;
    Sqe->PRP1 = StorPortGetPhysicalAddress(Adapter, NULL,
                                           Adapter->IdentifyBuffer, NULL).QuadPart;
    Sqe->PRP2 = 0;
    Sqe->CDW10 = NVME_IDENTIFY_NS;                /* CNS = 0 */
    Sqe->CDW11 = 0;                                /* CNTID = 0 */

    NvmeSubmitSq(Adapter, Adapter->AdminSq);
    if (!NvmeWaitCompletion(Adapter->AdminCq, 0x101, 3000, &Status) || Status != 0)
    {
        NvmeDebugPrint("identify namespace failed status=%x\n", Status);
        return FALSE;
    }

    /* identify-namespace: NCAP at dword 2, NUSE at dword 32. */
    Ncap = Id[2];
    Nuse = Id[32];
    if (Ncap == 0)
    {
        NvmeDebugPrint("namespace NCAP is zero\n");
        return FALSE;
    }

    Adapter->NamespaceId = NVME_NSID;
    Adapter->BlockCount = Nuse;
    Adapter->BlockSize = NVME_BLOCK_SIZE;
    Adapter->MaxLba = (ULONG)(Ncap - 1);
    Adapter->MaxLbaCycles = (ULONG)((Ncap - 1) >> 32);
    Adapter->NamespaceReady = TRUE;

    NvmeDebugPrint("ns %x ready: NCAP %x:%x NUSE %x, %u byte blocks\n",
                   NVME_NSID, Adapter->MaxLbaCycles, Adapter->MaxLba,
                   (ULONG)Nuse, Adapter->BlockSize);
    return TRUE;
}

static BOOLEAN NvmeCreateIoQueues(PNVME_ADAPTER Adapter)
{
    ULONGLONG IoSqPa, IoCqPa;
    PNVME_SQE Sqe;
    USHORT Status = 0;
    USHORT Cid;

    IoSqPa = StorPortGetPhysicalAddress(Adapter, NULL, Adapter->IoSq, NULL).QuadPart;
    IoCqPa = StorPortGetPhysicalAddress(Adapter, NULL, Adapter->IoCq, NULL).QuadPart;
    if (!IoSqPa || !IoCqPa)
    {
        NvmeDebugPrint("could not get I/O queue physical addresses\n");
        return FALSE;
    }

    /* Create I/O SQ (Qid 1). CDW11 = PCQPR: PC(0) QSIZE(15:1) CQID(31:16).
     * PRP1/PRP2 hold the SQ and CQ base addresses. */
    Cid = 0x201;
    Sqe = NvmeAllocSq(Adapter->AdminSq);
    RtlZeroMemory(Sqe, sizeof(*Sqe));
    Sqe->DW0 = NvmeSqeMakeDW0(NVME_ADMIN_CREATE_IO_SQ, 0, Cid);
    Sqe->NSID = 0;
    Sqe->PRP1 = IoSqPa;
    Sqe->PRP2 = IoCqPa;
    Sqe->CDW11 = (1u << 0) | (6u << 1) | (1u << 16); /* PC=1, QSIZE=6, CQID=1 */
    NvmeSubmitSq(Adapter, Adapter->AdminSq);
    if (!NvmeWaitCompletion(Adapter->AdminCq, Cid, 3000, &Status) || Status != 0)
    {
        NvmeDebugPrint("create IO SQ failed status=%x\n", Status);
        return FALSE;
    }

    /* Create I/O CQ (Qid 1). CDW10 = PCQPR: PC(0) IOCR(31:16). PRP1 = CQ base. */
    Cid = 0x202;
    Sqe = NvmeAllocSq(Adapter->AdminSq);
    RtlZeroMemory(Sqe, sizeof(*Sqe));
    Sqe->DW0 = NvmeSqeMakeDW0(NVME_ADMIN_CREATE_IO_CQ, 0, Cid);
    Sqe->NSID = 0;
    Sqe->PRP1 = IoCqPa;
    Sqe->CDW10 = (1u << 0) | ((ULONG)(NVME_IO_QUEUE_SIZE - 1) << 16); /* PC=1, IOCR=size-1 */
    NvmeSubmitSq(Adapter, Adapter->AdminSq);
    if (!NvmeWaitCompletion(Adapter->AdminCq, Cid, 3000, &Status) || Status != 0)
    {
        NvmeDebugPrint("create IO CQ failed status=%x\n", Status);
        return FALSE;
    }

    /* Ring the new I/O queue doorbells. */
    NvmeRingDoorbell(Adapter, 1, FALSE, 0);
    NvmeRingDoorbell(Adapter, 1, TRUE, 0);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* SCSI helpers                                                                */
/* ------------------------------------------------------------------------ */

static void NvmeFillInquiry(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb)
{
    PINQUIRYDATA Inquiry = (PINQUIRYDATA)Srb->DataBuffer;

    UNREFERENCED_PARAMETER(Adapter);
    NT_ASSERT(Srb->DataTransferLength >= INQUIRYDATABUFFERSIZE);
    RtlZeroMemory(Inquiry, INQUIRYDATABUFFERSIZE);
    Srb->DataTransferLength = INQUIRYDATABUFFERSIZE;

    Inquiry->DeviceType = DIRECT_ACCESS_DEVICE;
    Inquiry->DeviceTypeModifier = 0;
    Inquiry->RemovableMedia = FALSE;
    Inquiry->CommandQueue = 0; /* NCQ not used by this miniport */
    Inquiry->AdditionalLength = INQUIRYDATABUFFERSIZE - 5;

    StorPortCopyMemory(Inquiry->VendorId, "WINDOSDX", sizeof(Inquiry->VendorId) - 1);
    StorPortCopyMemory(Inquiry->ProductId, "WinDosDX NVMe Drive", sizeof(Inquiry->ProductId) - 1);
    StorPortCopyMemory(Inquiry->ProductRevisionLevel, "1.0", sizeof(Inquiry->ProductRevisionLevel) - 1);
    Inquiry->VendorId[sizeof(Inquiry->VendorId) - 1] = '\0';
    Inquiry->ProductId[sizeof(Inquiry->ProductId) - 1] = '\0';
    Inquiry->ProductRevisionLevel[sizeof(Inquiry->ProductRevisionLevel) - 1] = '\0';
}

static void NvmeFillReadCapacity(PNVME_ADAPTER Adapter, PSCSI_REQUEST_BLOCK Srb)
{
    PREAD_CAPACITY_DATA Rc = (PREAD_CAPACITY_DATA)Srb->DataBuffer;
    ULONG MaxLba, BytesPerBlock;

    NT_ASSERT(Srb->DataTransferLength >= sizeof(READ_CAPACITY_DATA));
    Srb->DataTransferLength = sizeof(READ_CAPACITY_DATA);

    BytesPerBlock = Adapter->BlockSize;
    /* For very large disks the 10-byte form saturates; the caller will issue
     * READ CAPACITY(16) for those. */
    MaxLba = (Adapter->MaxLbaCycles != 0) ? (ULONG)-1 : Adapter->MaxLba;

    Rc->LogicalBlockAddress = 0;
    Rc->BytesPerBlock = 0;
    REVERSE_BYTES(&Rc->LogicalBlockAddress, &MaxLba);
    REVERSE_BYTES(&Rc->BytesPerBlock, &BytesPerBlock);
}

/* Parse a 10-byte read/write CDB into a 64-bit LBA and block count. */
static void NvmeDecodeReadWriteCdb(PCDB Cdb, ULONGLONG *Lba, ULONG *Blocks)
{
    ULONGLONG Block = 0;
    Block |= ((ULONGLONG)Cdb->CDB10.LogicalBlockByte0) << 24;
    Block |= ((ULONGLONG)Cdb->CDB10.LogicalBlockByte1) << 16;
    Block |= ((ULONGLONG)Cdb->CDB10.LogicalBlockByte2) << 8;
    Block |= ((ULONGLONG)Cdb->CDB10.LogicalBlockByte3);
    *Lba = Block;
    *Blocks = ((ULONG)Cdb->CDB10.TransferBlocksMsb << 8) | Cdb->CDB10.TransferBlocksLsb;
}

/* Issue one Read/Write and poll for completion. Returns the (SCT<<8)|SC status. */
static USHORT NvmeSubmitReadWrite(PNVME_ADAPTER Adapter,
                                  PSCSI_REQUEST_BLOCK Srb,
                                  PCDB Cdb,
                                  BOOLEAN IsWrite)
{
    PSTOR_SCATTER_GATHER_LIST Sgl;
    ULONGLONG *Slot;
    PNVME_SQE Sqe;
    USHORT Status = 0;
    USHORT Cid;
    ULONGLONG Lba;
    ULONG Blocks;
    ULONG TotalPrp;
    STOR_LOCK_HANDLE LockHandle = {0};
    ULONG Tag = (ULONG)-1;
    ULONG i;

    Sgl = StorPortGetScatterGatherList(Adapter, Srb);
    if (!Sgl || Sgl->NumberOfElements == 0)
        return 0xFFFF;

    NvmeDecodeReadWriteCdb(Cdb, &Lba, &Blocks);
    if (Blocks == 0)
        Blocks = (Srb->DataTransferLength + (Adapter->BlockSize - 1)) / Adapter->BlockSize;

    /* Reserve a tag slot. */
    StorPortAcquireSpinLock(Adapter, InterruptLock, Adapter, &LockHandle);
    for (i = 0; i < NVME_MAX_OUTSTANDING; i++)
    {
        if (Adapter->Outstanding[i] == NULL)
        {
            Adapter->Outstanding[i] = Srb;
            Tag = i;
            break;
        }
    }
    StorPortReleaseSpinLock(Adapter, &LockHandle);
    if (Tag == (ULONG)-1)
        return 0xFFFF;

    Slot = NvmeAllocPrpSlot(Adapter);
    if (!Slot)
    {
        StorPortAcquireSpinLock(Adapter, InterruptLock, Adapter, &LockHandle);
        Adapter->Outstanding[Tag] = NULL;
        StorPortReleaseSpinLock(Adapter, &LockHandle);
        return 0xFFFF;
    }

    TotalPrp = NvmeBuildPrpList(Sgl, Slot);
    if (TotalPrp == 0)
    {
        NvmeFreePrpSlot(Adapter, Slot);
        StorPortAcquireSpinLock(Adapter, InterruptLock, Adapter, &LockHandle);
        Adapter->Outstanding[Tag] = NULL;
        StorPortReleaseSpinLock(Adapter, &LockHandle);
        return 0xFFFF;
    }

    Cid = (USHORT)(0x300 + (Tag & 0xFF));

    Sqe = NvmeAllocSq(Adapter->IoSq);
    RtlZeroMemory(Sqe, sizeof(*Sqe));
    Sqe->DW0 = NvmeSqeMakeDW0(IsWrite ? NVME_IO_WRITE : NVME_IO_READ, 0, Cid);
    Sqe->NSID = (ULONG)Adapter->NamespaceId;
    /* Transfer length is in 4-byte dwords; 0 means 65536. */
    Sqe->DW3 = (Blocks * (Adapter->BlockSize / 4)) & 0xFFFF;

    /* Link the PRP entries. PRPH1/2 = 01b means "PRP1/PRP2 points to a
     * PRP list entry that points to another list entry". For a single page
     * we can point PRP1 straight at the buffer. */
    if (TotalPrp == 1)
    {
        Sqe->PRP1 = Slot[0];
        Sqe->PRP2 = 0;
    }
    else
    {
        ULONGLONG SlotPa = StorPortGetPhysicalAddress(Adapter, NULL, Slot, NULL).QuadPart;
        /* PRPH1=01b: PRP1 points to the first PRP list entry. */
        Sqe->DW2 = (1u << 2) | (1u << 4);
        Sqe->PRP1 = SlotPa;
        /* PRP2 points to the second PRP list entry (the list is contiguous). */
        Sqe->PRP2 = SlotPa + sizeof(ULONGLONG);
    }

    Sqe->CDW10 = (ULONG)Lba;                                   /* SLBA */
    Sqe->CDW11 = (Blocks - 1) << 16;                          /* (NLB-1)<<16, GRPID=0 */
    Sqe->CDW12 = 0;                                            /* DTYPE = PRP (0) */
    Sqe->CDW13 = 0;

    KeMemoryBarrier();
    NvmeSubmitSq(Adapter, Adapter->IoSq);

    if (!NvmeWaitCompletion(Adapter->IoCq, Cid,
                            Srb->TimeOutValue ? Srb->TimeOutValue : 5000, &Status))
    {
        Status = 0xFFFF; /* timeout sentinel */
    }

    NvmeFreePrpSlot(Adapter, Slot);
    StorPortAcquireSpinLock(Adapter, InterruptLock, Adapter, &LockHandle);
    Adapter->Outstanding[Tag] = NULL;
    StorPortReleaseSpinLock(Adapter, &LockHandle);

    return Status;
}

static void NvmeFlush(PNVME_ADAPTER Adapter)
{
    PNVME_SQE Sqe;
    USHORT Status = 0;
    NvmeDrainCompletion(Adapter->IoCq);
    Sqe = NvmeAllocSq(Adapter->IoSq);
    RtlZeroMemory(Sqe, sizeof(*Sqe));
    Sqe->DW0 = NvmeSqeMakeDW0(NVME_IO_FLUSH, 0, 0x2FF);
    Sqe->NSID = (ULONG)Adapter->NamespaceId;
    NvmeSubmitSq(Adapter, Adapter->IoSq);
    NvmeWaitCompletion(Adapter->IoCq, 0x2FF, 5000, &Status);
}

/* ------------------------------------------------------------------------ */
/* Storport entry points                                                        */
/* ------------------------------------------------------------------------ */

static BOOLEAN NTAPI NvmeHwResetBus(PVOID DeviceExtension, ULONG PathId)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(PathId);
    return TRUE;
}

static BOOLEAN NTAPI NvmeHwInterrupt(PVOID DeviceExtension)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;
    /* Polled model: re-assert the mask. */
    NvmeWrite32(Adapter->Registers, NVME_REG_INTMS, 0xFFFFFFFF);
    return TRUE;
}

static BOOLEAN NTAPI NvmeHwStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;
    PCDB Cdb = (PCDB)Srb->Cdb;
    PSCSI_PNP_REQUEST_BLOCK Pnp = (PSCSI_PNP_REQUEST_BLOCK)Srb;

    if (!Adapter->ControllerReady)
    {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        StorPortNotification(RequestComplete, Adapter, Srb);
        return TRUE;
    }

    if (Srb->Function == SRB_FUNCTION_PNP)
    {
        if (Pnp->SrbPnPFlags & SRB_PNP_FLAGS_ADAPTER_REQUEST)
        {
            switch (Pnp->PnPAction)
            {
                case StorRemoveDevice:
                case StorSurpriseRemoval:
                    Adapter->ControllerReady = FALSE;
                    Srb->SrbStatus = SRB_STATUS_SUCCESS;
                    break;
                case StorStopDevice:
                    Srb->SrbStatus = SRB_STATUS_SUCCESS;
                    break;
                default:
                    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
                    break;
            }
        }
        else
        {
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        }
        StorPortNotification(RequestComplete, Adapter, Srb);
        return TRUE;
    }

    if (Srb->Function == SRB_FUNCTION_FLUSH)
    {
        NvmeFlush(Adapter);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        StorPortNotification(RequestComplete, Adapter, Srb);
        return TRUE;
    }

    if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI)
    {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, Adapter, Srb);
        return TRUE;
    }

    switch (Cdb->CDB10.OperationCode)
    {
        case SCSIOP_INQUIRY:
            NvmeFillInquiry(Adapter, Srb);
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;
        case SCSIOP_READ_CAPACITY:
        case SCSIOP_READ_CAPACITY16:
            NvmeFillReadCapacity(Adapter, Srb);
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;
        case SCSIOP_TEST_UNIT_READY:
            Srb->SrbStatus = Adapter->NamespaceReady ? SRB_STATUS_SUCCESS
                                                      : SRB_STATUS_NO_DEVICE;
            break;
        case SCSIOP_MODE_SENSE:
        case SCSIOP_REPORT_LUNS:
            RtlZeroMemory(Srb->DataBuffer, Srb->DataTransferLength);
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;
        case SCSIOP_READ:
        case SCSIOP_WRITE:
        {
            USHORT Status = NvmeSubmitReadWrite(Adapter, Srb, Cdb,
                              Cdb->CDB10.OperationCode == SCSIOP_WRITE);
            if (Status == 0xFFFF)
                Srb->SrbStatus = SRB_STATUS_TIMEOUT;
            else if (Status != 0)
                Srb->SrbStatus = SRB_STATUS_ERROR;
            else
                Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;
        }
        default:
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            break;
    }

    StorPortNotification(RequestComplete, Adapter, Srb);
    return TRUE;
}

static BOOLEAN NTAPI NvmeHwInitialize(PVOID DeviceExtension)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;
    STOR_LOCK_HANDLE LockHandle = {0};

    /* Prime the spin lock and mark the interrupt masks as ready, matching the
     * pattern storport miniports use before handing out the adapter. */
    StorPortAcquireSpinLock(Adapter, InterruptLock, Adapter, &LockHandle);
    StorPortReleaseSpinLock(Adapter, &LockHandle);
    Adapter->InterruptMasksInitialized = 1;

    Adapter->ControllerReady = TRUE;
    NvmeDebugPrint("HwInitialize: controller ready\n");
    return TRUE;
}

static ULONG NTAPI NvmeHwFindAdapter(PVOID DeviceExtension,
                                     PVOID HwContext,
                                     PVOID BusInformation,
                                     PCHAR ArgumentString,
                                     PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                                     PBOOLEAN Again)
{
    PNVME_ADAPTER Adapter = (PNVME_ADAPTER)DeviceExtension;
    UCHAR PciCfg[sizeof(PCI_COMMON_CONFIG) + sizeof(PCI_CARD_DESCRIPTOR)];
    ULONG PciCfgLen;
    PPCI_COMMON_CONFIG PciConfig;
    ACCESS_RANGE *Ranges;
    PVOID Mmio = NULL;
    ULONGLONG Cap;
    ULONGLONG AdminSqPa, AdminCqPa;
    ULONG Offset;
    ULONG i;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);
    *Again = FALSE;

    PciCfgLen = StorPortGetBusData(Adapter, PCIConfiguration,
                                   ConfigInfo->SystemIoBusNumber,
                                   ConfigInfo->SlotNumber,
                                   PciCfg, sizeof(PciCfg));
    if (PciCfgLen < sizeof(PCI_COMMON_CONFIG))
    {
        NvmeDebugPrint("no PCI config data\n");
        return SP_RETURN_ERROR;
    }

    PciConfig = (PPCI_COMMON_CONFIG)PciCfg;
    Adapter->VendorId = PciConfig->VendorID;
    Adapter->DeviceId = PciConfig->DeviceID;
    Adapter->RevisionId = PciConfig->RevisionID;
    Adapter->SystemIoBusNumber = ConfigInfo->SystemIoBusNumber;
    Adapter->SlotNumber = ConfigInfo->SlotNumber;
    Adapter->MmioBase = PciConfig->u.type0.BaseAddresses[0] & 0xFFFFFFFFFFFFFFF0ULL;

    NvmeDebugPrint("NVMe %04x:%04x rev %02x BAR0=%llx\n",
                   Adapter->VendorId, Adapter->DeviceId, Adapter->RevisionId,
                   Adapter->MmioBase);

    /* Map BAR0 (the memory-mapped controller registers). */
    if (ConfigInfo->NumberOfAccessRanges > 0)
    {
        Ranges = (ACCESS_RANGE *)ConfigInfo->AccessRanges;
        for (i = 0; i < ConfigInfo->NumberOfAccessRanges; i++)
        {
            if (Ranges[i].RangeStart.QuadPart == (LONGLONG)Adapter->MmioBase)
            {
                Mmio = StorPortGetDeviceBase(Adapter,
                                             ConfigInfo->AdapterInterfaceType,
                                             ConfigInfo->SystemIoBusNumber,
                                             Ranges[i].RangeStart,
                                             Ranges[i].RangeLength,
                                             !Ranges[i].RangeInMemory);
                break;
            }
        }
    }
    if (!Mmio)
    {
        NvmeDebugPrint("could not map BAR0\n");
        return SP_RETURN_ERROR;
    }
    Adapter->Registers = (volatile PUCHAR)Mmio;

    Cap = NvmeRead64(Adapter->Registers, NVME_REG_CAP);
    Adapter->DoorbellStride = (ULONG)NVME_CAP_DSTRD(Cap);
    NvmeDebugPrint("CAP=%08x%08x DSTRD=%u\n",
                   (ULONG)(Cap >> 32), (ULONG)Cap, Adapter->DoorbellStride);

    /*
     * Lay out the uncached extension (physically contiguous, page aligned):
     *   admin SQ, IO SQ, identify buffer, PRP slots.
     */
    Adapter->NonCachedExtensionSize =
        sizeof(NVME_SUBMIT_QUEUE) * 2 + NVME_PAGE_SIZE +
        NVME_PRP_SLOT_COUNT * NVME_PRP_SLOT_SIZE + 0xFFF;

    Adapter->NonCachedExtension =
        StorPortGetUncachedExtension(Adapter, ConfigInfo, Adapter->NonCachedExtensionSize);
    if (!Adapter->NonCachedExtension)
    {
        NvmeDebugPrint("could not allocate uncached extension\n");
        return SP_RETURN_ERROR;
    }
    /* Page-align the start. */
    Adapter->NonCachedExtension =
        (PVOID)(((ULONG_PTR)Adapter->NonCachedExtension + NVME_PAGE_SIZE - 1) &
                ~((ULONG_PTR)NVME_PAGE_SIZE - 1));

    Offset = 0;
    Adapter->AdminSq = (PNVME_SUBMIT_QUEUE)((PUCHAR)Adapter->NonCachedExtension + Offset);
    Offset += sizeof(NVME_SUBMIT_QUEUE);
    Adapter->IoSq = (PNVME_SUBMIT_QUEUE)((PUCHAR)Adapter->NonCachedExtension + Offset);
    Offset += sizeof(NVME_SUBMIT_QUEUE);
    Adapter->IdentifyBuffer = (PUCHAR)Adapter->NonCachedExtension + Offset;
    Offset += NVME_PAGE_SIZE;
    for (i = 0; i < NVME_PRP_SLOT_COUNT; i++)
    {
        Adapter->PrpSlots[i] = (ULONGLONG *)((PUCHAR)Adapter->NonCachedExtension + Offset);
        Offset += NVME_PRP_SLOT_SIZE;
    }
    Adapter->FreePrpSlots = (1UL << NVME_PRP_SLOT_COUNT) - 1;
    Adapter->AdminSq->Qid = 0;
    Adapter->IoSq->Qid = 1;
    Adapter->AdminCq->Qid = 0;
    Adapter->IoCq->Qid = 1;
    Adapter->AdminCq->Head = Adapter->AdminCq->Tail = 0;
    Adapter->IoCq->Head = Adapter->IoCq->Tail = 0;
    Adapter->AdminCq->Phase = 1;
    Adapter->IoCq->Phase = 1;
    RtlZeroMemory(Adapter->AdminSq, sizeof(NVME_SUBMIT_QUEUE));
    RtlZeroMemory(Adapter->IoSq, sizeof(NVME_SUBMIT_QUEUE));
    RtlZeroMemory(Adapter->AdminCq, sizeof(NVME_COMPLETION_QUEUE));
    RtlZeroMemory(Adapter->IoCq, sizeof(NVME_COMPLETION_QUEUE));

    /* Enable the controller with the admin queue programmed. */
    AdminSqPa = StorPortGetPhysicalAddress(Adapter, NULL, Adapter->AdminSq, NULL).QuadPart;
    AdminCqPa = StorPortGetPhysicalAddress(Adapter, NULL, Adapter->AdminCq, NULL).QuadPart;
    if (!AdminSqPa || !AdminCqPa)
    {
        NvmeDebugPrint("could not get admin queue physical addresses\n");
        return SP_RETURN_ERROR;
    }
    if (!NvmeControllerEnable(Adapter, AdminSqPa, AdminCqPa,
                              NVME_ADMIN_QUEUE_SIZE, NVME_ADMIN_QUEUE_SIZE))
    {
        NvmeDebugPrint("controller enable failed, CSTS=%08x\n",
                       NvmeRead32(Adapter->Registers, NVME_REG_CSTS));
        return SP_RETURN_ERROR;
    }

    /* Ring the admin doorbells (0) so the admin queue is live. */
    NvmeRingDoorbell(Adapter, 0, FALSE, 0);
    NvmeRingDoorbell(Adapter, 0, TRUE, 0);

    if (!NvmeIdentifyNamespace(Adapter))
        return SP_RETURN_ERROR;

    if (!NvmeCreateIoQueues(Adapter))
        return SP_RETURN_ERROR;

    return SP_RETURN_FOUND;
}

ULONG NTAPI DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    HW_INITIALIZATION_DATA HwInit = {0};

    HwInit.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
    HwInit.HwInitialize = NvmeHwInitialize;
    HwInit.HwStartIo = NvmeHwStartIo;
    HwInit.HwInterrupt = NvmeHwInterrupt;
    HwInit.HwResetBus = NvmeHwResetBus;
    HwInit.HwFindAdapter = NvmeHwFindAdapter;

    HwInit.TaggedQueuing = FALSE;
    HwInit.AutoRequestSense = TRUE;
    HwInit.MultipleRequestPerLu = FALSE;
    HwInit.NeedPhysicalAddresses = TRUE;
    HwInit.NumberOfAccessRanges = 1;
    HwInit.AdapterInterfaceType = PCIBus;
    HwInit.MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;

    HwInit.SrbExtensionSize = sizeof(NVME_SRB_EXTENSION);
    HwInit.DeviceExtensionSize = sizeof(NVME_ADAPTER);

    return StorPortInitialize(DriverObject, RegistryPath, &HwInit, NULL);
}
