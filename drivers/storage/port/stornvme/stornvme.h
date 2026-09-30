/*
 * PROJECT:        WinDosDX Storage Stack
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        NVMe (NVM Express) miniport - declarations
 * PROGRAMMERS:    WinDosDX Team
 */

#ifndef _STORNVME_H_
#define _STORNVME_H_

#include <ntddk.h>
#include <srb.h>
#include <scsi.h>

/*
 * An NVMe miniport for scsiport.
 *
 * Design notes:
 *  - The controller is driven through its memory-mapped BAR0 register block.
 *  - One admin queue pair and one I/O queue pair live in the uncached
 *    extension. Queue memory is page aligned, as the specification requires.
 *  - scsiport hands the miniport one request at a time. It is submitted from
 *    HwStartIo and completed from the interrupt (legacy INTx, vector 0). A
 *    short miniport timer also reaps the completion queue, so a request still
 *    completes if the interrupt is not delivered.
 *  - Namespace 1 is exposed as target 0, LUN 0 of a single bus.
 */

/* Controller registers (BAR0) */
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

/* CAP fields */
#define NVME_CAP_MQES(c)            ((ULONG)((c) & 0xFFFF))
#define NVME_CAP_TO(c)              ((ULONG)(((c) >> 24) & 0xFF))   /* 500 ms units */
#define NVME_CAP_DSTRD(c)           ((ULONG)(((c) >> 32) & 0xF))
#define NVME_CAP_MPSMIN(c)          ((ULONG)(((c) >> 48) & 0xF))

/* CC fields */
#define NVME_CC_EN                  (1u << 0)
#define NVME_CC_CSS_NVM             (0u << 4)
#define NVME_CC_MPS_4K              (0u << 7)
#define NVME_CC_AMS_RR              (0u << 11)
#define NVME_CC_SHN_MASK            (3u << 14)
#define NVME_CC_SHN_NORMAL          (1u << 14)
#define NVME_CC_IOSQES(x)           ((ULONG)(x) << 16)
#define NVME_CC_IOCQES(x)           ((ULONG)(x) << 20)

/* CSTS fields */
#define NVME_CSTS_RDY               (1u << 0)
#define NVME_CSTS_CFS               (1u << 1)
#define NVME_CSTS_SHST_MASK         (3u << 2)
#define NVME_CSTS_SHST_DONE         (2u << 2)

/* Admin opcodes */
#define NVME_ADMIN_DELETE_IO_SQ     0x00
#define NVME_ADMIN_CREATE_IO_SQ     0x01
#define NVME_ADMIN_DELETE_IO_CQ     0x04
#define NVME_ADMIN_CREATE_IO_CQ     0x05
#define NVME_ADMIN_IDENTIFY         0x06

/* NVM opcodes */
#define NVME_NVM_FLUSH              0x00
#define NVME_NVM_WRITE              0x01
#define NVME_NVM_READ               0x02

/* Identify CNS values */
#define NVME_IDENTIFY_CNS_NS        0x00
#define NVME_IDENTIFY_CNS_CTRL      0x01

/* Geometry */
#define NVME_PAGE_SIZE              0x1000
#define NVME_PAGE_SHIFT             12
#define NVME_QUEUE_DEPTH            16          /* entries per queue */
#define NVME_NSID                   1
#define NVME_MAX_TRANSFER           (128 * 1024)
#define NVME_MAX_PRP                (NVME_MAX_TRANSFER / NVME_PAGE_SIZE + 1)
#define NVME_ADMIN_TIMEOUT_US       (5 * 1000 * 1000)
#define NVME_POLL_INTERVAL_US       5000

/* 64-byte submission queue entry */
typedef struct _NVME_SQE
{
    ULONG Cdw0;                 /* opcode(7:0) fuse(9:8) psdt(15:14) cid(31:16) */
    ULONG Nsid;
    ULONG Cdw2;
    ULONG Cdw3;
    ULONGLONG Mptr;
    ULONGLONG Prp1;
    ULONGLONG Prp2;
    ULONG Cdw10;
    ULONG Cdw11;
    ULONG Cdw12;
    ULONG Cdw13;
    ULONG Cdw14;
    ULONG Cdw15;
} NVME_SQE, *PNVME_SQE;

C_ASSERT(sizeof(NVME_SQE) == 64);

/* 16-byte completion queue entry */
typedef struct _NVME_CQE
{
    ULONG Dw0;                  /* command specific */
    ULONG Dw1;
    USHORT SqHead;
    USHORT SqId;
    USHORT Cid;
    USHORT Status;              /* phase(0) sc(8:1) sct(11:9) crd(13:12) m(14) dnr(15) */
} NVME_CQE, *PNVME_CQE;

C_ASSERT(sizeof(NVME_CQE) == 16);

#define NVME_CQE_PHASE(s)           ((s) & 1)
#define NVME_CQE_STATUS(s)          (((s) >> 1) & 0x7FF)    /* sct:sc, 0 = success */

#define NVME_MAKE_CDW0(Opcode, Cid) ((ULONG)(Opcode) | ((ULONG)(Cid) << 16))

typedef struct _NVME_QUEUE
{
    PVOID Entries;              /* SQE or CQE array */
    ULONGLONG Physical;
    USHORT Qid;
    USHORT Index;               /* SQ: tail, CQ: head */
    USHORT Phase;               /* CQ only */
} NVME_QUEUE, *PNVME_QUEUE;

typedef struct _NVME_SRB_EXTENSION
{
    ULONG Reserved;
} NVME_SRB_EXTENSION, *PNVME_SRB_EXTENSION;

typedef struct _NVME_ADAPTER
{
    volatile PUCHAR Registers;
    ULONG DoorbellStride;       /* bytes */
    ULONG ReadyTimeoutUs;

    /* Uncached memory: queues, identify buffer, PRP list */
    NVME_QUEUE AdminSq;
    NVME_QUEUE AdminCq;
    NVME_QUEUE IoSq;
    NVME_QUEUE IoCq;
    PUCHAR IdentifyBuffer;
    ULONGLONG IdentifyPhysical;
    PULONGLONG PrpList;
    ULONGLONG PrpListPhysical;

    /* Namespace 1 */
    ULONGLONG BlockCount;
    ULONG BlockSize;
    ULONG MaxTransfer;
    CHAR Serial[21];
    CHAR Model[41];
    CHAR Firmware[9];

    /* The request in flight on the I/O queue */
    PSCSI_REQUEST_BLOCK CurrentSrb;
    USHORT NextCid;
    volatile LONG Reaping;
    BOOLEAN Ready;
} NVME_ADAPTER, *PNVME_ADAPTER;

#if DBG
#define NvmeDebugPrint(...) DbgPrint("stornvme: " __VA_ARGS__)
#else
#define NvmeDebugPrint(...) do { } while (0)
#endif

#endif /* _STORNVME_H_ */
