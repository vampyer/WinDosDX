/*
 * PROJECT:     WinDosDX Intel I225 (Foxville) 2.5GbE Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Hardware specific definitions
 *
 * Register offsets and bit fields are sourced from "Intel Ethernet
 * Controller I225 Software User Manual", Revision 1.3 (17-Aug-2020).
 * Section/page numbers in comments refer to that document. The legacy
 * (82542-compatible) descriptor formats and general register block
 * (CTRL/STATUS/MDIC/RCTL/TCTL/TIPG) are confirmed compatible with the
 * existing e1000 driver's layout in this tree - reused directly rather
 * than reinvented. The descriptor RING registers (RDBAL/TDBAL etc.) moved
 * to new offsets on this chip and must NOT be assumed from e1000.
 */

#pragma once

#define IEEE_802_ADDR_LENGTH 6

#define HW_VENDOR_INTEL     0x8086

#define MAX_RESET_ATTEMPTS  10

#define MAXIMUM_MULTICAST_ADDRESSES 16

/* Ethernet frame header */
typedef struct _ETH_HEADER {
    UCHAR Destination[IEEE_802_ADDR_LENGTH];
    UCHAR Source[IEEE_802_ADDR_LENGTH];
    USHORT PayloadType;
} ETH_HEADER, *PETH_HEADER;

C_ASSERT(sizeof(ETH_HEADER) == 14);

typedef enum _I225_RCVBUF_SIZE
{
    I225_RCVBUF_2048 = 0,
    I225_RCVBUF_1024 = 1,
    I225_RCVBUF_512 = 2,
    I225_RCVBUF_256 = 3,
} I225_RCVBUF_SIZE;

#include <pshpack1.h>

/* Legacy Receive Descriptor - Section 7.1.4.1, Table 7-4/7-5 (p254-256).
 * Confirmed byte-identical layout to the existing E1000_RECEIVE_DESCRIPTOR. */

#define I225_RDESC_STATUS_PIF   (1U << 7)   /* Passed imperfect filter only */
#define I225_RDESC_STATUS_IPCS  (1U << 6)   /* IPv4 checksum calculated */
#define I225_RDESC_STATUS_L4CS  (1U << 5)   /* L4 checksum calculated */
#define I225_RDESC_STATUS_UDPCS (1U << 4)   /* UDP checksum calculated */
#define I225_RDESC_STATUS_VP    (1U << 3)   /* 802.1Q packet, VLAN stripped */
#define I225_RDESC_STATUS_EOP   (1U << 1)   /* End of packet */
#define I225_RDESC_STATUS_DD    (1U << 0)   /* Descriptor done */

typedef struct _I225_RECEIVE_DESCRIPTOR
{
    UINT64 Address;

    USHORT Length;
    USHORT Checksum;
    UCHAR Status;
    UCHAR Errors;
    USHORT Special;

} I225_RECEIVE_DESCRIPTOR, *PI225_RECEIVE_DESCRIPTOR;

/* Legacy Transmit Descriptor - Section 7.2.2.1, Table 7-25/7-26 (p279).
 * Confirmed byte-identical layout to the existing E1000_TRANSMIT_DESCRIPTOR.
 * Note: the datasheet explicitly flags legacy Tx descriptors as
 * unsupported-going-forward in favor of the Advanced format (p279), but
 * they remain a documented, working mode and are used here for
 * simplicity/reuse of the existing e1000 driver logic. */

#define I225_TDESC_CMD_IDE      (1U << 7)   /* Interrupt Delay Enable */
#define I225_TDESC_CMD_VLE      (1U << 6)   /* VLAN Insertion Enable */
#define I225_TDESC_CMD_DEXT     (1U << 5)   /* Descriptor Extension (0=legacy) */
#define I225_TDESC_CMD_RS       (1U << 3)   /* Report Status */
#define I225_TDESC_CMD_IC       (1U << 2)   /* Insert Checksum */
#define I225_TDESC_CMD_IFCS     (1U << 1)   /* Insert FCS */
#define I225_TDESC_CMD_EOP      (1U << 0)   /* End Of Packet */

#define I225_TDESC_STATUS_DD    (1U << 0)   /* Descriptor Done */

typedef struct _I225_TRANSMIT_DESCRIPTOR
{
    UINT64 Address;

    USHORT Length;
    UCHAR ChecksumOffset;
    UCHAR Command;
    UCHAR Status;
    UCHAR ChecksumStartField;
    USHORT Special;

} I225_TRANSMIT_DESCRIPTOR, *PI225_TRANSMIT_DESCRIPTOR;

#include <poppack.h>

C_ASSERT(sizeof(I225_RECEIVE_DESCRIPTOR) == 16);
C_ASSERT(sizeof(I225_TRANSMIT_DESCRIPTOR) == 16);

#define NUM_TRANSMIT_DESCRIPTORS  128
#define NUM_RECEIVE_DESCRIPTORS   128

/* PCI identification - confirmed against real hardware (an Intel I225-V,
 * via Get-PnpDevice's InstanceId: PCI\VEN_8086&DEV_15F3&SUBSYS_...&REV_03) */
#define I225_DEVICE_ID_V  0x15F3
/* Other Foxville SKUs (I225-LM/-I/-K, I226 family) share this register
 * layout per the datasheet's SKU options (Section 1.5.6), but their exact
 * DEV_IDs are not yet confirmed against real hardware - add only once
 * verified against an actual device, not guessed. */

/* ===================== General Registers - Section 8.2 (p373) ===================== */
#define I225_REG_CTRL      0x00000  /* Device Control - RW, Section 8.2.1 p373 */
#define I225_REG_STATUS    0x00008  /* Device Status - RO, Section 8.2.2 p375 */
#define I225_REG_CTRL_EXT  0x00018  /* Extended Device Control - RW, Section 8.2.3 p375-376 */
#define I225_REG_MDIC      0x00020  /* MDI Control - RW, Section 8.2.4 p377 */
#define I225_REG_VET       0x00038  /* VLAN Ether Type - RW, Section 8.2.8 p381 */

/* CTRL (0x00000) bits - Section 8.2.1 (p373-375) */
#define I225_CTRL_FD                 (1U << 0)   /* Full duplex */
#define I225_CTRL_GIO_MASTER_DISABLE (1U << 2)
#define I225_CTRL_SLU                (1U << 6)   /* Set Link Up */
#define I225_CTRL_RFCE               (1U << 27)  /* Receive Flow Control Enable */
#define I225_CTRL_TFCE               (1U << 28)  /* Transmit Flow Control Enable */
#define I225_CTRL_DEV_RST            (1U << 29)  /* Device Reset - self-clearing (SC) */
#define I225_CTRL_VME                (1U << 30)  /* VLAN Mode Enable */
#define I225_CTRL_PHY_RST            (1U << 31)  /* PHY Reset - hold >=100us */

/* CTRL_EXT (0x00018) bits - Section 8.2.3 (p375-376). Note: unlike e1000
 * (where SPEED/RST_DONE live in STATUS), on I225 both are in CTRL_EXT. */
#define I225_CTRL_EXT_SPEED_SHIFT    6
#define I225_CTRL_EXT_SPEED_MASK     (3U << I225_CTRL_EXT_SPEED_SHIFT)
#define I225_CTRL_EXT_SPEED_2P5      (1U << 22)  /* Speed_2P5: link is 2.5Gb/s */
#define I225_CTRL_EXT_GIO_MASTER_EN_STATUS (1U << 19)
#define I225_CTRL_EXT_RST_DONE       (1U << 21)  /* SW reset (CTRL.DEV_RST) completed */

/* STATUS (0x00008) bits - Section 8.2.2 (p375) */
#define I225_STATUS_FD    (1U << 0)  /* Full duplex */
#define I225_STATUS_LU    (1U << 1)  /* Link Up */

/* MDIC (0x00020) bits - Section 8.2.4 (p377-378) */
#define I225_MDIC_REGADD_SHIFT  16
#define I225_MDIC_OP_WRITE      (1U << 26)
#define I225_MDIC_OP_READ       (2U << 26)
#define I225_MDIC_R             (1U << 28)  /* Ready */
#define I225_MDIC_MDI_ERR       (1U << 30)  /* Error */

/* ===================== NVM Registers - Section 8.4 (p381-383) ===================== */
#define I225_REG_EEC   0x12010  /* EEPROM-Mode Control - RW, Section 8.4.1 p381-382 */
#define I225_REG_EERD  0x12014  /* EEPROM-Mode Read - RW, Section 8.4.2 p382-383 */

#define I225_EEC_EE_PRES  (1U << 8)  /* NVM present with valid signature */
#define I225_EEC_AUTO_RD  (1U << 9)  /* NVM auto-read by hardware done */

/* ===================== Interrupt Registers - Section 8.8 (p398+) ===================== */
#define I225_REG_ICR   0x1500  /* Interrupt Cause Read - RC/W1C */
#define I225_REG_ICS   0x1504  /* Interrupt Cause Set - WO */
#define I225_REG_IMS   0x1508  /* Interrupt Mask Set/Read - RW */
#define I225_REG_IMC   0x150C  /* Interrupt Mask Clear - WO */
#define I225_REG_EIMC  0x1528  /* Extended Interrupt Mask Clear - WO, used during init (Section 4.7.4, p118) */

/* IMS/ICR/IMC bits - same well-known Intel NIC interrupt-cause bit
 * positions as e1000 (general register block is stable across this NIC
 * family); TXQE at bit 1 not separately confirmed for I225 in the
 * sections read so far, kept consistent with e1000 pending verification. */
#define I225_IMS_TXDW    (1U << 0)   /* Transmit Descriptor Written Back */
#define I225_IMS_TXQE    (1U << 1)   /* Transmit Queue Empty */
#define I225_IMS_LSC     (1U << 2)   /* Link Status Change */
#define I225_IMS_RXDMT0  (1U << 4)   /* Receive Descriptor Minimum Threshold */
#define I225_IMS_RXT0    (1U << 7)   /* Receiver Timer Interrupt */
#define I225_IMS_TXD_LOW (1U << 15)  /* Transmit Descriptor Low Threshold */

/* ===================== Receive Registers - Section 8.9 (p417+) ===================== */
#define I225_REG_RCTL   0x0100   /* Receive Control - RW, Section 8.9.1 p417-420 */
#define I225_REG_RXCSUM 0x5000   /* Receive Checksum Control - RW, Section 8.9.10 p424 */
#define I225_REG_MTA    0x5200   /* Multicast Table Array (+4*n, n=0..127) - Section 8.9.13 p427 */
#define I225_REG_RAL    0x5400   /* Receive Address Low (+8*n, n=0..15) - Section 8.9.14 p428 */
#define I225_REG_RAH    0x5404   /* Receive Address High (+8*n, n=0..15) - Section 8.9.15 p428 */

/* Per-queue Rx descriptor ring registers (n=0..3) - Section 8.9.3-8.9.9
 * (p421-423). NOTE: unlike e1000's RDBAL @ 0x2800, this moved to 0xC000. */
#define I225_REG_RDBAL(n)   (0xC000 + 0x40 * (n))
#define I225_REG_RDBAH(n)   (0xC004 + 0x40 * (n))
#define I225_REG_RDLEN(n)   (0xC008 + 0x40 * (n))
#define I225_REG_SRRCTL(n)  (0xC00C + 0x40 * (n))
#define I225_REG_RDH(n)     (0xC010 + 0x40 * (n))
#define I225_REG_RDT(n)     (0xC018 + 0x40 * (n))
#define I225_REG_RXDCTL(n)  (0xC028 + 0x40 * (n))

/* RXDCTL bits - Section 8.9.9 (p425), confirmed */
#define I225_RXDCTL_ENABLE  (1U << 25)

/* RCTL (0x0100) bits - Section 8.9.1 (p419-420) */
#define I225_RCTL_RXEN        (1U << 1)
#define I225_RCTL_SBP         (1U << 2)   /* Store Bad Packets */
#define I225_RCTL_UPE         (1U << 3)   /* Unicast Promiscuous */
#define I225_RCTL_MPE         (1U << 4)   /* Multicast Promiscuous */
#define I225_RCTL_LPE         (1U << 5)   /* Long Packet Enable */
#define I225_RCTL_BAM         (1U << 15)  /* Broadcast Accept Mode */
#define I225_RCTL_BSIZE_SHIFT 16
#define I225_RCTL_BSIZE_MASK  (3U << I225_RCTL_BSIZE_SHIFT)
#define I225_RCTL_VFE         (1U << 18)  /* VLAN Filter Enable */
#define I225_RCTL_PSP         (1U << 21)  /* Pad Small Receive Packets */
#define I225_RCTL_DPF         (1U << 22)  /* Discard Pause Frames */
#define I225_RCTL_PMCF        (1U << 23)  /* Pass MAC Control Frames */

#define I225_RCTL_FILTER_BITS (I225_RCTL_SBP | I225_RCTL_UPE | I225_RCTL_MPE | I225_RCTL_BAM | I225_RCTL_PMCF)

/* RAH (Receive Address High) - Section 8.9.15 (p428). Bit position of the
 * Address Valid flag not yet individually confirmed for I225's RAH layout
 * (only the register offset was read) - reusing e1000's documented AV
 * position (bit 31) as a starting point since this field is conventionally
 * stable across the Intel NIC family; verify against Section 8.9.15's full
 * text before trusting for anything beyond the primary address (RAH[0]). */
#define I225_RAH_AV  (1U << 31)

/* ===================== Transmit Registers - Section 8.11 (p436+) ===================== */
#define I225_REG_TCTL      0x0400  /* Transmit Control - RW, Section 8.11.1 p436-437 */
#define I225_REG_TCTL_EXT  0x0404  /* Transmit Control Extended - RW, Section 8.11.2 p438 */
#define I225_REG_TIPG      0x0410  /* Transmit IPG - RW, offset matches e1000, bit layout not yet re-confirmed for I225 */

/* Per-queue Tx descriptor ring registers (n=0..3) - Section 8.11.10-8.11.15
 * (p441-442). NOTE: unlike e1000's TDBAL @ 0x3800, this moved to 0xE000. */
#define I225_REG_TDBAL(n)   (0xE000 + 0x40 * (n))
#define I225_REG_TDBAH(n)   (0xE004 + 0x40 * (n))
#define I225_REG_TDLEN(n)   (0xE008 + 0x40 * (n))
#define I225_REG_TDH(n)     (0xE010 + 0x40 * (n))
#define I225_REG_TDT(n)     (0xE018 + 0x40 * (n))
#define I225_REG_TXDCTL(n)  (0xE028 + 0x40 * (n))

/* TXDCTL bits - Section 8.11.15 (p443-444), confirmed */
#define I225_TXDCTL_ENABLE  (1U << 25)

/* TCTL (0x0400) bits - Section 8.11.1 (p437) */
#define I225_TCTL_EN      (1U << 1)   /* Transmit Enable */
#define I225_TCTL_PSP     (1U << 3)   /* Pad Short Packets */

/* Transmit IPG defaults - offset confirmed (0x0410, matches e1000's TIPG
 * exactly), but the IPGT/IPGR1/IPGR2 field layout for I225 was not
 * individually re-extracted from the datasheet in this session; e1000's
 * default field shifts (0/10/20) are reused as a starting assumption since
 * TIPG's field layout has been stable across this whole NIC generation,
 * but this should be verified against Section 8.11.3 before relying on it
 * for anything beyond a first bring-up attempt. */
#define I225_TIPG_IPGT_DEF   (10U << 0)
#define I225_TIPG_IPGR1_DEF  (10U << 10)
#define I225_TIPG_IPGR2_DEF  (10U << 20)

/* ===================== Still not extracted from the datasheet =====================
 * Do not guess these - pull them from the same PDF (repo root) before use:
 *   - RXDCTL/TXDCTL PTHRESH/HTHRESH/WTHRESH threshold field values - this
 *     driver only sets ENABLE (confirmed bit 25 for both, Section
 *     8.9.9/8.11.15) and leaves the threshold fields at their reset
 *     defaults, which is valid but not performance-tuned
 *   - RXPBSIZE/TXPBSIZE full field layout - Section 8.3 (p382), offsets
 *     0x2404/0x3404 seen but fields not cross-referenced against this
 *     driver's actual packet-buffer sizing needs (relying on reset
 *     defaults for now)
 *   - EERD full bit layout beyond CMDV/DONE (address/data shift positions)
 *     - Section 8.4.2 (p382-383) - not needed yet since this driver reads
 *     the station address from RAL0/RAH0 (NVM auto-loaded) rather than
 *     doing a software EERD read, but would be needed for NVM checksum
 *     validation like e1000's NICPowerOn does
 *   - PHY-specific MDIC register numbers/bit meanings for link
 *     speed/duplex/status beyond what CTRL_EXT.SPEED already reports
 *   - RCTL.SECRC (strip-CRC) equivalent bit for I225 not individually
 *     re-verified (e1000 has it at RCTL bit 26) - omitted from this
 *     driver's RCTL programming rather than guessed
 */
