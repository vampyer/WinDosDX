/*
 * PROJECT:        WinDosDX Storage Stack
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        NVMe (NVM Express) Storport Miniport - declarations
 * PROGRAMMERS:    WinDosDX Team
 */

#ifndef _STORNVME_H_
#define _STORNVME_H_

#include <ntddk.h>
#include <storport.h>
#include <reactos/drivers/pci/pci.h>

/*
 * A polled, single-I/O-queue NVMe miniport.
 *
 * Design notes:
 *  - The controller is driven through its memory-mapped BAR0 register block,
 *    exactly like storahci drives the AHCI HBA through its ABAR.
 *  - Command execution is synchronous: the SRB is translated to an NVMe
 *    command, submitted, and the completion queue is polled until the
 *    matching CID retires (with a bounded timeout). Interrupts are masked
 *    (INTMS held asserted) so no DPC re-entrancy is required.
 *  - Only namespace 1 is exposed as a single LUN.
 */

#define NVME_PCI_VENDOR_ID          0x1B36

/* Controller register offsets within BAR0 */
#define NVME_REG_CAP                0x00
#define NVME_REG_VS                 0x08
#define NVME_REG_INTMS              0x0C
#define NVME_REG_INTMC              0x10
#define NVME_REG_CC                 0x14
#define NVME_REG_CSTS               0x1C
#define NVME_REG_AQA                0x24
#define NVME_REG_ASQ                0x28
#define NVME_REG_ACQ                0x30
#define NVME_REG_DOORBELL_BASE      0x1000

/* CAP register fields (bit 32+) */
#define NVME_CAP_DSTRD(x)           (((x) >> 32) & 0xF)
#define NVME_CAP_TO                 (((x) >> 48) & 0xFFFF)

/* CC register fields */
#define NVME_CC_EN                  (1u << 0)
#define NVME_CC_CSS(x)              (((x) & 0x7) << 4)
#define NVME_CC_MPS                 (0u << 7)         /* 0 == 4 KiB pages */
#define NVME_CC_AMS(x)              (((x) & 0xF) << 11)
#define NVME_CC_SHN(x)              (((x) & 0x3) << 14)
#define NVME_CC_SQES(x)             (((x) & 0xF) << 16)
#define NVME_CC_CQES(x)             (((x) & 0xF) << 20)

/* CSTS register fields */
#define NVME_CSTS_RDY               (1u << 0)
#define NVME_CSTS_CFS(x)            (((x) >> 1) & 0x3)
#define NVME_CSTS_CFS_NONE          0
#define NVME_CSTS_CFS_OVERFLOW      1
#define NVME_CSTS_CFS_FAILED        2

/* Shutdown notification values for CC.SHN */
#define NVME_CC_SHN_NONE            0
#define NVME_CC_SHN_NORMAL          1
#define NVME_CC_SHN_ABRUPT          2

/* Admin opcodes */
#define NVME_ADMIN_CREATE_IO_SQ     0x01
#define NVME_ADMIN_CREATE_IO_CQ     0x05
#define NVME_ADMIN_IDENTIFY         0x06

/* I/O opcodes */
#define NVME_IO_FLUSH               0x00
#define NVME_IO_WRITE               0x01
#define NVME_IO_READ                0x02

/* Identify CNS values */
#define NVME_IDENTIFY_NS            0x00
#define NVME_IDENTIFY_CTRL          0x01

/* Queue geometry */
#define NVME_ADMIN_QUEUE_SIZE       64
#define NVME_IO_QUEUE_SIZE          64
#define NVME_MAX_OUTSTANDING        64
#define NVME_PRP_SLOT_COUNT         8
#define NVME_PRP_SLOT_ENTRIES       256        /* 256 * 8 bytes = 2048 per slot */
#define NVME_PRP_SLOT_SIZE          (NVME_PRP_SLOT_ENTRIES * sizeof(ULONGLONG))
#define NVME_MAX_PAGES_PER_IO       (NVME_PRP_SLOT_COUNT * NVME_PRP_SLOT_ENTRIES)

#define NVME_PAGE_SIZE              4096
#define NVME_NSID                   1
#define NVME_BLOCK_SIZE             512

/* Capability register (64-bit) */
typedef union _NVME_CAPABILITY_REGS
{
    ULONGLONG Value;
    struct
    {
        ULONGLONG CapVendorSpecific : 32;
        ULONGLONG CapDstrd : 4;
        ULONGLONG CapNssr : 1;
        ULONGLONG CapCss : 8;
        ULONGLONG CapBps : 1;
        ULONGLONG CapMpsMin : 4;
        ULONGLONG CapMpsMax : 4;
        ULONGLONG CapPmrs : 1;
        ULONGLONG CapDss : 1;
        ULONGLONG CapNcmds : 1;
        ULONGLONG CapVsl : 7;
        ULONGLONG CapMqes : 16;
    } Cap;
} NVME_CAPABILITY_REGS;

/*
 * 64-byte Submission Queue Entry, byte-exact per NVMe 1.4 spec Figure 280.
 *   +0x00 DW0  opcode(7:0) fuse(9:8) rsvd(11:10) psdt(14:11) rsvd(15) cid(31:16)
 *   +0x04 NSID
 *   +0x08 DW2  rsvd(1:0) prph1(3:2) prph2(5:4) prph3(7:6) meta(23:8)
 *   +0x0C DW3  dlen(15:0) rsvd
 *   +0x10 DW4  command dword 4 (CDW4)
 *   +0x14 DW5  command dword 5 (CDW5)
 *   +0x18 PRP1 (64-bit)
 *   +0x20 PRP2 (64-bit)
 *   +0x28 CDW10, +0x2C CDW11, +0x30 CDW12, +0x34 CDW13
 */
typedef struct _NVME_SQE
{
    ULONG DW0;        /* opcode | psdt | cid */
    ULONG NSID;
    ULONG DW2;        /* prph1/2/3 + meta */
    ULONG DW3;        /* transfer length in 4-byte dwords */
    ULONG DW4;
    ULONG DW5;
    ULONGLONG PRP1;
    ULONGLONG PRP2;
    ULONG CDW10;
    ULONG CDW11;
    ULONG CDW12;
    ULONG CDW13;
    ULONG Reserved[6];
} NVME_SQE, *PNVME_SQE;

#define NvmeSqeOpcode(dw0)   ((dw0) & 0xFF)
#define NvmeSqeCid(dw0)      (((dw0) >> 16) & 0xFFFF)
#define NvmeSqeMakeDW0(op, psdt, cid) \
    ((ULONG)((op) | (((psdt) & 0x7) << 11) | (((ULONG)(cid) & 0xFFFF) << 16)))

/* Set PRPH1=linked, PRPH2=linked (a PRP list is chained via PRP2). */
#define NvmeSqeSetPrphLinked(dw2) \
    ((dw2) = ((((dw2) >> 0) & 0x00000003u) | \
              (1u << 2) | /* PRPH1 = 01b (linked) */ \
              (1u << 4)   /* PRPH2 = 01b (linked) */))

/* 16-byte Completion Queue Entry per NVMe 1.4 Figure 281. */
typedef struct _NVME_CQE
{
    USHORT Result;     /* +0x00 DW0 result */
    USHORT Reserved0;  /* +0x02 */
    USHORT SQHD;       /* +0x04 */
    USHORT SQID;       /* +0x06 */
    USHORT CID;        /* +0x08 */
    USHORT Flags;      /* +0x0A: P(0) sc(8:1) sct(11:9) crd(13:12) m(14) dnr(15) */
} NVME_CQE, *PNVME_CQE;

#define NvmeCqePhase(f)   ((f) & 0x1)
#define NvmeCqeSC(f)      (((f) >> 1) & 0xFF)
#define NvmeCqeSCT(f)     (((f) >> 9) & 0x7)

typedef struct _NVME_SUBMIT_QUEUE
{
    NVME_SQE Entries[NVME_IO_QUEUE_SIZE];
    ULONG Tail;
    ULONG Phase;
    ULONG Qid;
} NVME_SUBMIT_QUEUE, *PNVME_SUBMIT_QUEUE;

typedef struct _NVME_COMPLETION_QUEUE
{
    NVME_CQE Entries[NVME_IO_QUEUE_SIZE];
    volatile ULONG Head;
    volatile ULONG Tail;
    ULONG Phase;
    ULONG Qid;
} NVME_COMPLETION_QUEUE, *PNVME_COMPLETION_QUEUE;

typedef struct _NVME_SRB_EXTENSION
{
    ULONG Tag;
} NVME_SRB_EXTENSION, *PNVME_SRB_EXTENSION;

typedef struct _NVME_ADAPTER
{
    /* Bus / identity */
    ULONG SystemIoBusNumber;
    ULONG SlotNumber;
    USHORT VendorId;
    USHORT DeviceId;
    USHORT RevisionId;
    ULONGLONG MmioBase;

    /* Controller register block (BAR0) */
    volatile PUCHAR Registers;
    ULONG DoorbellStride;

    /* Uncached (physically contiguous, page aligned) scratch area */
    PVOID NonCachedExtension;
    ULONG NonCachedExtensionSize;

    PNVME_SUBMIT_QUEUE AdminSq;
    PNVME_SUBMIT_QUEUE IoSq;
    PNVME_COMPLETION_QUEUE AdminCq;
    PNVME_COMPLETION_QUEUE IoCq;

    PUCHAR IdentifyBuffer;         /* 4 KiB */
    ULONGLONG *PrpSlots[NVME_PRP_SLOT_COUNT];
    volatile ULONG FreePrpSlots;    /* bitmask */

    /* Outstanding command slots: Tag -> SRB (kept for diagnostics). */
    PSCSI_REQUEST_BLOCK Outstanding[NVME_MAX_OUTSTANDING];

    /* Namespace geometry */
    ULONGLONG NamespaceId;
    ULONGLONG BlockCount;
    ULONG BlockSize;
    ULONG MaxLba;
    ULONG MaxLbaCycles;
    BOOLEAN NamespaceReady;

    /* Synchronization */
    ULONG InterruptMasksInitialized;

    BOOLEAN ControllerReady;
} NVME_ADAPTER, *PNVME_ADAPTER;

#define NvmeDebugPrint(...) \
    DbgPrint("stornvme: " __VA_ARGS__)

#endif /* _STORNVME_H_ */
