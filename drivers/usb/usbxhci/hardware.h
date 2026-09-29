/*
 * PROJECT:     WinDosDX xHCI (USB 3) host controller driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     xHCI registers, TRBs and contexts (xHCI specification 1.2)
 */

#pragma once

/* Capability registers (at the start of BAR 0) */
#define XHCI_CAPLENGTH          0x00    /* 8 bits; HCIVERSION at 0x02 */
#define XHCI_HCSPARAMS1         0x04
#define XHCI_HCSPARAMS2         0x08
#define XHCI_HCSPARAMS3         0x0C
#define XHCI_HCCPARAMS1         0x10
#define XHCI_DBOFF              0x14
#define XHCI_RTSOFF             0x18
#define XHCI_HCCPARAMS2         0x1C

#define HCS1_MAX_SLOTS(p)       ((p) & 0xFF)
#define HCS1_MAX_INTRS(p)       (((p) >> 8) & 0x7FF)
#define HCS1_MAX_PORTS(p)       (((p) >> 24) & 0xFF)
#define HCS2_MAX_SCRATCHPADS(p) ((((p) >> 21) & 0x1F) << 5 | (((p) >> 27) & 0x1F))
#define HCC1_AC64               (1 << 0)
#define HCC1_CSZ                (1 << 2)    /* 64-byte contexts */
#define HCC1_PPC                (1 << 3)    /* port power control */
#define HCC1_XECP(p)            (((p) >> 16) & 0xFFFF)

/* Operational registers (at CAPLENGTH) */
#define XHCI_USBCMD             0x00
#define XHCI_USBSTS             0x04
#define XHCI_PAGESIZE           0x08
#define XHCI_DNCTRL             0x14
#define XHCI_CRCR               0x18    /* 64 bits */
#define XHCI_DCBAAP             0x30    /* 64 bits */
#define XHCI_CONFIG             0x38
#define XHCI_PORTSC(n)          (0x400 + 0x10 * ((n) - 1))  /* n from 1 */

#define USBCMD_RS               (1 << 0)
#define USBCMD_HCRST            (1 << 1)
#define USBCMD_INTE             (1 << 2)
#define USBCMD_HSEE             (1 << 3)

#define USBSTS_HCH              (1 << 0)
#define USBSTS_HSE              (1 << 2)
#define USBSTS_EINT             (1 << 3)
#define USBSTS_PCD              (1 << 4)
#define USBSTS_CNR              (1 << 11)
#define USBSTS_HCE              (1 << 12)

#define CRCR_RCS                (1 << 0)

/* PORTSC */
#define PORTSC_CCS              (1 << 0)
#define PORTSC_PED              (1 << 1)    /* RW1C: writing 1 disables */
#define PORTSC_OCA              (1 << 3)
#define PORTSC_PR               (1 << 4)
#define PORTSC_PLS_MASK         (0xF << 5)
#define PORTSC_PLS(p)           (((p) >> 5) & 0xF)
#define PORTSC_PP               (1 << 9)
#define PORTSC_SPEED(p)         (((p) >> 10) & 0xF)
#define PORTSC_LWS              (1 << 16)
#define PORTSC_CSC              (1 << 17)
#define PORTSC_PEC              (1 << 18)
#define PORTSC_WRC              (1 << 19)
#define PORTSC_OCC              (1 << 20)
#define PORTSC_PRC              (1 << 21)
#define PORTSC_PLC              (1 << 22)
#define PORTSC_CEC              (1 << 23)
#define PORTSC_WPR              (1U << 31)
#define PORTSC_CHANGE_BITS      (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | \
                                 PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)
/* Bits to leave out when writing PORTSC back (RW1C and PED) */
#define PORTSC_WRITE_MASK(p)    ((p) & ~(PORTSC_PED | PORTSC_CHANGE_BITS | PORTSC_LWS))

#define PLS_U3                  3           /* suspended */

#define XHCI_SPEED_FULL         1
#define XHCI_SPEED_LOW          2
#define XHCI_SPEED_HIGH         3
#define XHCI_SPEED_SUPER        4

/* Runtime registers (at RTSOFF): interrupter 0 */
#define XHCI_MFINDEX            0x00
#define XHCI_IMAN               0x20
#define XHCI_IMOD               0x24
#define XHCI_ERSTSZ             0x28
#define XHCI_ERSTBA             0x30    /* 64 bits */
#define XHCI_ERDP               0x38    /* 64 bits */

#define IMAN_IP                 (1 << 0)
#define IMAN_IE                 (1 << 1)
#define ERDP_EHB                (1 << 3)

/* Extended capabilities */
#define XECP_ID(p)              ((p) & 0xFF)
#define XECP_NEXT(p)            (((p) >> 8) & 0xFF)
#define XECP_LEGACY             1
#define LEGACY_BIOS_OWNED       (1 << 16)
#define LEGACY_OS_OWNED         (1 << 24)
#define LEGACY_CTLSTS           4       /* offset of USBLEGCTLSTS */
#define LEGACY_SMI_MASK         0xE0000000 /* RW1C status bits */

/* Transfer Request Blocks */
typedef struct _XHCI_TRB
{
    ULONG64 Parameter;
    ULONG Status;
    ULONG Control;
} XHCI_TRB, *PXHCI_TRB;

C_ASSERT(sizeof(XHCI_TRB) == 16);

#define TRB_CYCLE               (1 << 0)
#define TRB_TOGGLE_CYCLE        (1 << 1)    /* Link TRB */
#define TRB_ENT                 (1 << 1)
#define TRB_ISP                 (1 << 2)
#define TRB_CHAIN               (1 << 4)
#define TRB_IOC                 (1 << 5)
#define TRB_IDT                 (1 << 6)
#define TRB_BSR                 (1 << 9)    /* Address Device: block SET_ADDRESS */
#define TRB_DIR_IN              (1 << 16)   /* Data and Status stage */
#define TRB_TYPE(t)             ((ULONG)(t) << 10)
#define TRB_GET_TYPE(c)         (((c) >> 10) & 0x3F)
#define TRB_SLOT(s)             ((ULONG)(s) << 24)
#define TRB_GET_SLOT(c)         (((c) >> 24) & 0xFF)
#define TRB_EP(e)               ((ULONG)(e) << 16)
#define TRB_GET_EP(c)           (((c) >> 16) & 0x1F)
#define TRB_TRT(t)              ((ULONG)(t) << 16)  /* Setup: 0 none, 2 OUT, 3 IN */
#define TRB_LENGTH(l)           ((l) & 0x1FFFF)
#define TRB_TD_SIZE(n)          ((ULONG)((n) > 31 ? 31 : (n)) << 17)
#define TRB_COMPLETION(s)       (((s) >> 24) & 0xFF)
#define TRB_RESIDUAL(s)         ((s) & 0xFFFFFF)

/* TRB types */
#define TRB_NORMAL              1
#define TRB_SETUP               2
#define TRB_DATA                3
#define TRB_STATUS              4
#define TRB_LINK                6
#define TRB_ENABLE_SLOT         9
#define TRB_DISABLE_SLOT        10
#define TRB_ADDRESS_DEVICE      11
#define TRB_CONFIGURE_ENDPOINT  12
#define TRB_EVALUATE_CONTEXT    13
#define TRB_RESET_ENDPOINT      14
#define TRB_STOP_ENDPOINT       15
#define TRB_SET_TR_DEQUEUE      16
#define TRB_NOOP_COMMAND        23
#define TRB_TRANSFER_EVENT      32
#define TRB_COMMAND_COMPLETION  33
#define TRB_PORT_STATUS_CHANGE  34
#define TRB_HOST_CONTROLLER     37

/* Completion codes */
#define CC_SUCCESS              1
#define CC_DATA_BUFFER_ERROR    2
#define CC_BABBLE               3
#define CC_TRANSACTION_ERROR    4
#define CC_TRB_ERROR            5
#define CC_STALL                6
#define CC_SHORT_PACKET         13
#define CC_STOPPED              26
#define CC_STOPPED_LENGTH       27

/* Contexts: 32-byte entries, or 64 with CSZ. Only the first 32 bytes of
 * each are used; the rest is reserved. */
typedef struct _XHCI_SLOT_CONTEXT
{
    ULONG Info1;        /* route 19:0, speed 23:20, hub 26, entries 31:27 */
    ULONG Info2;        /* root port 23:16, ports 31:24 */
    ULONG TtInfo;
    ULONG State;        /* address 7:0, slot state 31:27 */
    ULONG Reserved[4];
} XHCI_SLOT_CONTEXT, *PXHCI_SLOT_CONTEXT;

typedef struct _XHCI_ENDPOINT_CONTEXT
{
    ULONG Info1;        /* state 2:0, mult 9:8, interval 23:16 */
    ULONG Info2;        /* CErr 2:1, type 5:3, max burst 15:8, max packet 31:16 */
    ULONG64 Dequeue;    /* DCS in bit 0 */
    ULONG TxInfo;       /* average TRB length 15:0, max ESIT payload 31:16 */
    ULONG Reserved[3];
} XHCI_ENDPOINT_CONTEXT, *PXHCI_ENDPOINT_CONTEXT;

typedef struct _XHCI_INPUT_CONTROL_CONTEXT
{
    ULONG DropFlags;
    ULONG AddFlags;
    ULONG Reserved[6];
} XHCI_INPUT_CONTROL_CONTEXT, *PXHCI_INPUT_CONTROL_CONTEXT;

#define SLOT_ROUTE(r)           ((r) & 0xFFFFF)
#define SLOT_SPEED(s)           ((ULONG)(s) << 20)
#define SLOT_ENTRIES(n)         ((ULONG)(n) << 27)
#define SLOT_GET_ENTRIES(i)     (((i) >> 27) & 0x1F)
#define SLOT_ROOT_PORT(p)       ((ULONG)(p) << 16)
#define SLOT_GET_ADDRESS(s)     ((s) & 0xFF)

#define EP_INTERVAL(i)          ((ULONG)(i) << 16)
#define EP_CERR(n)              ((ULONG)(n) << 1)
#define EP_TYPE(t)              ((ULONG)(t) << 3)
#define EP_MAX_BURST(b)         ((ULONG)(b) << 8)
#define EP_MAX_PACKET(m)        ((ULONG)(m) << 16)
#define EP_GET_STATE(i)         ((i) & 0x7)
#define EP_AVG_TRB_LENGTH(l)    ((l) & 0xFFFF)
#define EP_MAX_ESIT(p)          ((ULONG)(p) << 16)

#define EP_STATE_HALTED         2

/* Endpoint context types */
#define EPT_ISOCH_OUT           1
#define EPT_BULK_OUT            2
#define EPT_INTERRUPT_OUT       3
#define EPT_CONTROL             4
#define EPT_ISOCH_IN            5
#define EPT_BULK_IN             6
#define EPT_INTERRUPT_IN        7

/* Event Ring Segment Table entry */
typedef struct _XHCI_ERST_ENTRY
{
    ULONG64 Base;
    ULONG Size;
    ULONG Reserved;
} XHCI_ERST_ENTRY, *PXHCI_ERST_ENTRY;
