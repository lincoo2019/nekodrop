// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT7921 / MT7922 (Connac2) register map, WFDMA descriptor layout,
 * MCU command/event headers, and TXD/RXD bitfields.
 * Ported from Linux drivers/net/wireless/mediatek/mt76 (mt7921 / mt76_connac2).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace mtk {

/* Bit manipulation helpers matching Linux BIT() / GENMASK() / FIELD_PREP(). */
constexpr uint32_t bit(unsigned n) { return 1u << n; }
constexpr uint32_t genmask(unsigned hi, unsigned lo) {
  return ((~0u - (1u << lo) + 1u) & (~0u >> (31u - hi)));
}
constexpr uint32_t field_prep(uint32_t mask, uint32_t val) {
  unsigned shift = 0;
  uint32_t m = mask;
  while (m && !(m & 1u)) {
    m >>= 1;
    ++shift;
  }
  return (val << shift) & mask;
}
constexpr uint32_t field_get(uint32_t mask, uint32_t val) {
  unsigned shift = 0;
  uint32_t m = mask;
  while (m && !(m & 1u)) {
    m >>= 1;
    ++shift;
  }
  return (val & mask) >> shift;
}

inline uint32_t bswap32(uint32_t v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}
inline uint16_t bswap16(uint16_t v) {
  return static_cast<uint16_t>((v << 8) | (v >> 8));
}

/* --------------------------------------------------------------------------
 * PCIe Device IDs (MediaTek / AMD RZ)
 * -------------------------------------------------------------------------- */
constexpr uint16_t PCI_VENDOR_MEDIATEK = 0x14C3;
constexpr uint16_t PCI_DEV_MT7922_RZ616 = 0x0616;
constexpr uint16_t PCI_DEV_MT7922 = 0x7922;
constexpr uint16_t PCI_DEV_MT7921 = 0x7961;
constexpr uint16_t PCI_DEV_MT7921_RZ608 = 0x0608;

/* --------------------------------------------------------------------------
 * Register Address Translation (mt7921/pci.c __mt7921_reg_addr)
 * -------------------------------------------------------------------------- */
struct RegMapEntry {
  uint32_t phys;
  uint32_t maps;
  uint32_t size;
};

constexpr RegMapEntry kFixedRegMap[] = {
    {0x820d0000, 0x30000, 0x10000}, /* WF_LMAC_TOP (WF_WTBLON) */
    {0x820ed000, 0x24800, 0x00800}, /* WF_LMAC_TOP BN0 (WF_MIB) */
    {0x820e4000, 0x21000, 0x00400}, /* WF_LMAC_TOP BN0 (WF_TMAC) */
    {0x820e7000, 0x21e00, 0x00200}, /* WF_LMAC_TOP BN0 (WF_DMA) */
    {0x820eb000, 0x24200, 0x00400}, /* WF_LMAC_TOP BN0 (WF_LPON) */
    {0x820e2000, 0x20800, 0x00400}, /* WF_LMAC_TOP BN0 (WF_AGG) */
    {0x820e3000, 0x20c00, 0x00400}, /* WF_LMAC_TOP BN0 (WF_ARB) */
    {0x820e5000, 0x21400, 0x00800}, /* WF_LMAC_TOP BN0 (WF_RMAC) */
    {0x00400000, 0x80000, 0x10000}, /* WF_MCU_SYSRAM */
    {0x00410000, 0x90000, 0x10000}, /* WF_MCU_SYSRAM (configure register) */
    {0x40000000, 0x70000, 0x10000}, /* WF_UMAC_SYSRAM */
    {0x54000000, 0x02000, 0x01000}, /* WFDMA PCIE0 MCU DMA0 */
    {0x55000000, 0x03000, 0x01000}, /* WFDMA PCIE0 MCU DMA1 */
    {0x58000000, 0x06000, 0x01000}, /* WFDMA PCIE1 MCU DMA0 */
    {0x59000000, 0x07000, 0x01000}, /* WFDMA PCIE1 MCU DMA1 */
    {0x7c000000, 0xf0000, 0x10000}, /* CONN_INFRA */
    {0x7c020000, 0xd0000, 0x10000}, /* CONN_INFRA, WFDMA */
    {0x7c060000, 0xe0000, 0x10000}, /* CONN_INFRA, conn_host_csr_top */
    {0x80020000, 0xb0000, 0x10000}, /* WF_TOP_MISC_OFF */
    {0x81020000, 0xc0000, 0x10000}, /* WF_TOP_MISC_ON */
    {0x820c0000, 0x08000, 0x04000}, /* WF_UMAC_TOP (PLE) */
    {0x820c8000, 0x0c000, 0x02000}, /* WF_UMAC_TOP (PSE) */
    {0x820cc000, 0x0e000, 0x01000}, /* WF_UMAC_TOP (PP) */
    {0x820cd000, 0x0f000, 0x01000}, /* WF_MDP_TOP */
    {0x74030000, 0x10000, 0x10000}, /* PCIE_MAC_IREG */
    {0x820ce000, 0x21c00, 0x00200}, /* WF_LMAC_TOP (WF_SEC) */
    {0x820cf000, 0x22000, 0x01000}, /* WF_LMAC_TOP (WF_PF) */
    {0x820e0000, 0x20000, 0x00400}, /* WF_LMAC_TOP BN0 (WF_CFG) */
    {0x820e1000, 0x20400, 0x00200}, /* WF_LMAC_TOP BN0 (WF_TRB) */
    {0x820e9000, 0x23400, 0x00200}, /* WF_LMAC_TOP BN0 (WF_WTBLOFF) */
    {0x820ea000, 0x24000, 0x00200}, /* WF_LMAC_TOP BN0 (WF_ETBF) */
    {0x820ec000, 0x24600, 0x00200}, /* WF_LMAC_TOP BN0 (WF_INT) */
    {0x820f0000, 0xa0000, 0x00400}, /* WF_LMAC_TOP BN1 (WF_CFG) */
    {0x820f1000, 0xa0600, 0x00200}, /* WF_LMAC_TOP BN1 (WF_TRB) */
    {0x820f2000, 0xa0800, 0x00400}, /* WF_LMAC_TOP BN1 (WF_AGG) */
    {0x820f3000, 0xa0c00, 0x00400}, /* WF_LMAC_TOP BN1 (WF_ARB) */
    {0x820f4000, 0xa1000, 0x00400}, /* WF_LMAC_TOP BN1 (WF_TMAC) */
    {0x820f5000, 0xa1400, 0x00800}, /* WF_LMAC_TOP BN1 (WF_RMAC) */
    {0x820f7000, 0xa1e00, 0x00200}, /* WF_LMAC_TOP BN1 (WF_DMA) */
    {0x820f9000, 0xa3400, 0x00200}, /* WF_LMAC_TOP BN1 (WF_WTBLOFF) */
    {0x820fa000, 0xa4000, 0x00200}, /* WF_LMAC_TOP BN1 (WF_ETBF) */
    {0x820fb000, 0xa4200, 0x00400}, /* WF_LMAC_TOP BN1 (WF_LPON) */
    {0x820fc000, 0xa4600, 0x00200}, /* WF_LMAC_TOP BN1 (WF_INT) */
    {0x820fd000, 0xa4800, 0x00800}, /* WF_LMAC_TOP BN1 (WF_MIB) */
};

/* L1 Remap (mt7921/regs.h) */
constexpr uint32_t MT_INFRA_CFG_BASE = 0xfe000;
constexpr uint32_t MT_HIF_REMAP_L1 = MT_INFRA_CFG_BASE + 0x24c;
constexpr uint32_t MT_HIF_REMAP_L1_MASK = genmask(15, 0);
constexpr uint32_t MT_HIF_REMAP_L1_OFFSET = genmask(15, 0);
constexpr uint32_t MT_HIF_REMAP_L1_BASE = genmask(31, 16);
constexpr uint32_t MT_HIF_REMAP_BASE_L1 = 0x40000;

/* Chip ID / Revision / Reset (via L1 remap) */
constexpr uint32_t MT_HW_BOUND = 0x70010020;
constexpr uint32_t MT_HW_CHIPID = 0x70010200;
constexpr uint32_t MT_HW_REV = 0x70010204;

constexpr uint32_t MT_WFSYS_SW_RST_B = 0x18000140;
constexpr uint32_t WFSYS_SW_RST_B = bit(0);
constexpr uint32_t WFSYS_SW_INIT_DONE = bit(4);

constexpr uint32_t MT_CBTOP_RGU_WF_SUBSYS_RST = 0x70002600;
constexpr uint32_t MT_CBTOP_RGU_WF_SUBSYS_RST_WF_WHOLE_PATH = bit(0);

constexpr uint32_t MT_TOP_LPCR_HOST_BAND0 = 0x18060010;
constexpr uint32_t MT_TOP_LPCR_HOST_FW_OWN = bit(0);
constexpr uint32_t MT_TOP_LPCR_HOST_DRV_OWN = bit(1);

/* CONN_INFRA / Firmware State */
constexpr uint32_t MT_CONN_ON_LPCTL = 0x7c060010;
constexpr uint32_t PCIE_LPCR_HOST_SET_OWN = bit(0);
constexpr uint32_t PCIE_LPCR_HOST_CLR_OWN = bit(1);
constexpr uint32_t PCIE_LPCR_HOST_OWN_SYNC = bit(2);

constexpr uint32_t MT_CONN_ON_MISC = 0x7c0600f0;
constexpr uint32_t MT_TOP_MISC_FW_STATE = genmask(2, 0);
constexpr uint32_t MT_TOP_MISC2_FW_PWR_ON = bit(0);
constexpr uint32_t MT_TOP_MISC2_FW_N9_ON = bit(1);
constexpr uint32_t MT_TOP_MISC2_FW_N9_RDY = genmask(1, 0);

constexpr uint32_t MT_SWDEF_MODE = 0x41f23c;
constexpr uint32_t MT_SWDEF_NORMAL_MODE = 0;

/* PCIe MAC */
constexpr uint32_t MT_PCIE_MAC_BASE = 0x10000;
constexpr uint32_t MT_PCIE_MAC_INT_ENABLE = MT_PCIE_MAC_BASE + 0x188;
constexpr uint32_t MT_PCIE_MAC_PM = MT_PCIE_MAC_BASE + 0x194;
constexpr uint32_t MT_PCIE_MAC_PM_L0S_DIS = bit(8);

/* --------------------------------------------------------------------------
 * WFDMA0 Registers (mt792x_regs.h + mt7921/regs.h)
 * -------------------------------------------------------------------------- */
constexpr uint32_t MT_WFDMA0_BASE = 0xd4000;
constexpr uint32_t MT_WFDMA0(uint32_t ofs) { return MT_WFDMA0_BASE + ofs; }

constexpr uint32_t MT_WFDMA0_RST = MT_WFDMA0(0x100);
constexpr uint32_t MT_WFDMA0_RST_LOGIC_RST = bit(4);
constexpr uint32_t MT_WFDMA0_RST_DMASHDL_ALL_RST = bit(5);

constexpr uint32_t MT_WFDMA0_BUSY_ENA = MT_WFDMA0(0x13c);
constexpr uint32_t MT_WFDMA0_BUSY_ENA_TX_FIFO0 = bit(0);
constexpr uint32_t MT_WFDMA0_BUSY_ENA_TX_FIFO1 = bit(1);
constexpr uint32_t MT_WFDMA0_BUSY_ENA_RX_FIFO = bit(2);

constexpr uint32_t MT_MCU_CMD = MT_WFDMA0(0x1f0);
constexpr uint32_t MT_MCU_CMD_WAKE_RX_PCIE = bit(0);
constexpr uint32_t MT_MCU2HOST_SW_INT_ENA = MT_WFDMA0(0x1f4);

constexpr uint32_t MT_WFDMA0_HOST_INT_STA = MT_WFDMA0(0x200);
constexpr uint32_t MT_WFDMA0_HOST_INT_ENA = MT_WFDMA0(0x204);

constexpr uint32_t MT_WFDMA0_GLO_CFG = MT_WFDMA0(0x208);
constexpr uint32_t MT_WFDMA0_GLO_CFG_TX_DMA_EN = bit(0);
constexpr uint32_t MT_WFDMA0_GLO_CFG_TX_DMA_BUSY = bit(1);
constexpr uint32_t MT_WFDMA0_GLO_CFG_RX_DMA_EN = bit(2);
constexpr uint32_t MT_WFDMA0_GLO_CFG_RX_DMA_BUSY = bit(3);
constexpr uint32_t MT_WFDMA0_GLO_CFG_DMA_SIZE = genmask(5, 4);
constexpr uint32_t MT_WFDMA0_GLO_CFG_TX_WB_DDONE = bit(6);
constexpr uint32_t MT_WFDMA0_GLO_CFG_FIFO_DIS_CHECK = bit(11);
constexpr uint32_t MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN = bit(12);
constexpr uint32_t MT_WFDMA0_GLO_CFG_RX_WB_DDONE = bit(13);
constexpr uint32_t MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN = bit(15);
constexpr uint32_t MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 = bit(21);
constexpr uint32_t MT_WFDMA0_GLO_CFG_OMIT_RX_INFO = bit(27);
constexpr uint32_t MT_WFDMA0_GLO_CFG_OMIT_TX_INFO = bit(28);
constexpr uint32_t MT_WFDMA0_GLO_CFG_CLK_GAT_DIS = bit(30);

constexpr uint32_t MT_WFDMA0_RST_DTX_PTR = MT_WFDMA0(0x20c);
constexpr uint32_t MT_WFDMA0_GLO_CFG_EXT0 = MT_WFDMA0(0x2b0);
constexpr uint32_t MT_WFDMA0_GLO_CFG_EXT0_CSR_TX_DMASHDL_EN = bit(6);
constexpr uint32_t MT_WFDMA0_PRI_DLY_INT_CFG0 = MT_WFDMA0(0x2f0);

constexpr uint32_t MT_DMASHDL_SW_CONTROL = 0x7c026004;
constexpr uint32_t MT_DMASHDL_DMASHDL_BYPASS = bit(28);

/* Ring register bases (each ring occupies 0x10 bytes: desc_base, ring_size, cpu_idx, dma_idx) */
constexpr uint32_t MT_TX_RING_BASE = MT_WFDMA0(0x300);
constexpr uint32_t MT_RX_EVENT_RING_BASE = MT_WFDMA0(0x500);
constexpr uint32_t MT_RX_DATA_RING_BASE = MT_WFDMA0(0x520);
constexpr uint32_t MT_RX_MCU_WA_RING_BASE = MT_WFDMA0(0x540);
constexpr uint32_t MT_RING_STRIDE = 0x10;

/* MT7921/MT7922 Hardware TX/RX Ring Indices */
constexpr uint32_t MT7921_TXQ_BAND0 = 0;
constexpr uint32_t MT7921_TXQ_FWDL = 16;
constexpr uint32_t MT7921_TXQ_MCU_WM = 17;

/* Prefetch Ext Ctrl */
constexpr uint32_t MT_WFDMA0_TX_RING0_EXT_CTRL = MT_WFDMA0(0x600);
constexpr uint32_t MT_WFDMA0_TX_RING1_EXT_CTRL = MT_WFDMA0(0x604);
constexpr uint32_t MT_WFDMA0_TX_RING2_EXT_CTRL = MT_WFDMA0(0x608);
constexpr uint32_t MT_WFDMA0_TX_RING3_EXT_CTRL = MT_WFDMA0(0x60c);
constexpr uint32_t MT_WFDMA0_TX_RING4_EXT_CTRL = MT_WFDMA0(0x610);
constexpr uint32_t MT_WFDMA0_TX_RING5_EXT_CTRL = MT_WFDMA0(0x614);
constexpr uint32_t MT_WFDMA0_TX_RING6_EXT_CTRL = MT_WFDMA0(0x618);
constexpr uint32_t MT_WFDMA0_TX_RING16_EXT_CTRL = MT_WFDMA0(0x640);
constexpr uint32_t MT_WFDMA0_TX_RING17_EXT_CTRL = MT_WFDMA0(0x644);

constexpr uint32_t MT_WFDMA0_RX_RING0_EXT_CTRL = MT_WFDMA0(0x680);
constexpr uint32_t MT_WFDMA0_RX_RING2_EXT_CTRL = MT_WFDMA0(0x688);
constexpr uint32_t MT_WFDMA0_RX_RING3_EXT_CTRL = MT_WFDMA0(0x68c);
constexpr uint32_t MT_WFDMA0_RX_RING4_EXT_CTRL = MT_WFDMA0(0x690);
constexpr uint32_t MT_WFDMA0_RX_RING5_EXT_CTRL = MT_WFDMA0(0x694);

constexpr uint32_t MT_WFDMA_DUMMY_CR = 0x54000120;
constexpr uint32_t MT_WFDMA_NEED_REINIT = bit(1);

constexpr uint32_t MT_WFDMA_EXT_CSR_HIF_MISC = 0xd7044;
constexpr uint32_t MT_WFDMA_EXT_CSR_HIF_MISC_BUSY = bit(0);

constexpr uint32_t prefetch_val(uint32_t base, uint32_t depth) {
  return (base << 16) | depth;
}

/* --------------------------------------------------------------------------
 * MAC / MDP / RMAC / WTBL Registers
 * -------------------------------------------------------------------------- */
constexpr uint32_t MT_MDP_DCR0 = 0x820cd000;
constexpr uint32_t MT_MDP_DCR0_DAMSDU_EN = bit(15);
constexpr uint32_t MT_MDP_DCR0_RX_HDR_TRANS_EN = bit(19);

constexpr uint32_t MT_MDP_DCR1 = 0x820cd004;
constexpr uint32_t MT_MDP_DCR1_MAX_RX_LEN = genmask(15, 3);

constexpr uint32_t MT_DMA_DCR0_BAND0 = 0x820e7000;
constexpr uint32_t MT_DMA_DCR0_MAX_RX_LEN = genmask(15, 3);
constexpr uint32_t MT_DMA_DCR0_RXD_G5_EN = bit(23);

constexpr uint32_t MT_WF_RFCR_BAND0 = 0x820e5000;
constexpr uint32_t MT_WF_RFCR_DROP_STBC_MULTI = bit(0);
constexpr uint32_t MT_WF_RFCR_DROP_FCSFAIL = bit(1);
constexpr uint32_t MT_WF_RFCR_DROP_VERSION = bit(3);
constexpr uint32_t MT_WF_RFCR_DROP_PROBEREQ = bit(4);
constexpr uint32_t MT_WF_RFCR_DROP_MCAST = bit(5);
constexpr uint32_t MT_WF_RFCR_DROP_BCAST = bit(6);
constexpr uint32_t MT_WF_RFCR_DROP_MCAST_FILTERED = bit(7);
constexpr uint32_t MT_WF_RFCR_DROP_A3_MAC = bit(8);
constexpr uint32_t MT_WF_RFCR_DROP_A3_BSSID = bit(9);
constexpr uint32_t MT_WF_RFCR_DROP_A2_BSSID = bit(10);
constexpr uint32_t MT_WF_RFCR_DROP_OTHER_BEACON = bit(11);
constexpr uint32_t MT_WF_RFCR_DROP_FRAME_REPORT = bit(12);
constexpr uint32_t MT_WF_RFCR_DROP_CTL_RSV = bit(13);
constexpr uint32_t MT_WF_RFCR_DROP_CTS = bit(14);
constexpr uint32_t MT_WF_RFCR_DROP_RTS = bit(15);
constexpr uint32_t MT_WF_RFCR_DROP_DUPLICATE = bit(16);
constexpr uint32_t MT_WF_RFCR_DROP_OTHER_BSS = bit(17);
constexpr uint32_t MT_WF_RFCR_DROP_OTHER_UC = bit(18);
constexpr uint32_t MT_WF_RFCR_DROP_OTHER_TIM = bit(19);
constexpr uint32_t MT_WF_RFCR_DROP_NDPA = bit(20);
constexpr uint32_t MT_WF_RFCR_DROP_UNWANTED_CTL = bit(21);

constexpr uint32_t MT_WF_RFCR1_BAND0 = 0x820e5004;
constexpr uint32_t MT_WF_RFCR1_DROP_ACK = bit(4);
constexpr uint32_t MT_WF_RFCR1_DROP_BF_POLL = bit(5);
constexpr uint32_t MT_WF_RFCR1_DROP_BA = bit(6);
constexpr uint32_t MT_WF_RFCR1_DROP_CFEND = bit(7);
constexpr uint32_t MT_WF_RFCR1_DROP_CFACK = bit(8);

constexpr uint32_t MT_ARB_SCR_BAND0 = 0x820e3080;
constexpr uint32_t MT_ARB_SCR_TX_DISABLE = bit(8);
constexpr uint32_t MT_ARB_SCR_RX_DISABLE = bit(9);

/* Band 0 MAC timing registers (mt792x_mac_set_timeing). */
constexpr uint32_t MT_AGG_ACR0_BAND0 = 0x820e2084;
constexpr uint32_t MT_TMAC_CDTR_BAND0 = 0x820e4090;
constexpr uint32_t MT_TMAC_ODTR_BAND0 = 0x820e4094;
constexpr uint32_t MT_TMAC_ICR0_BAND0 = 0x820e40a4;

constexpr uint32_t MT_WTBL_UPDATE = 0x820d4230;
constexpr uint32_t MT_WTBL_UPDATE_WLAN_IDX = genmask(9, 0);
constexpr uint32_t MT_WTBL_UPDATE_ADM_COUNT_CLEAR = bit(12);
constexpr uint32_t MT_WTBL_UPDATE_BUSY = bit(31);

/* --------------------------------------------------------------------------
 * WFDMA Descriptor (mt76/dma.h struct mt76_desc)
 * -------------------------------------------------------------------------- */
#pragma pack(push, 1)
struct MtkDesc {
  uint32_t buf0;
  uint32_t ctrl;
  uint32_t buf1;
  uint32_t info;
};
#pragma pack(pop)
static_assert(sizeof(MtkDesc) == 16, "MtkDesc must be 16 bytes");

constexpr uint32_t MT_DMA_CTL_SD_LEN1 = genmask(13, 0);
constexpr uint32_t MT_DMA_CTL_LAST_SEC1 = bit(14);
constexpr uint32_t MT_DMA_CTL_SD_LEN0 = genmask(29, 16);
constexpr uint32_t MT_DMA_CTL_LAST_SEC0 = bit(30);
constexpr uint32_t MT_DMA_CTL_DMA_DONE = bit(31);

/* --------------------------------------------------------------------------
 * Connac2 TXD / TXP / RXD Constants (mt76_connac2_mac.h)
 * -------------------------------------------------------------------------- */
constexpr size_t MT_TXD_SIZE = 32; /* 8 * uint32_t */

constexpr uint32_t MT_TX_TYPE_CT = 0;
constexpr uint32_t MT_TX_TYPE_SF = 1;
constexpr uint32_t MT_TX_TYPE_CMD = 2;
constexpr uint32_t MT_TX_TYPE_FW = 3;

constexpr uint32_t MT_HDR_FORMAT_802_3 = 0;
constexpr uint32_t MT_HDR_FORMAT_CMD = 1;
constexpr uint32_t MT_HDR_FORMAT_802_11 = 2;

constexpr uint32_t MT_TX_PORT_IDX_MCU = 1;
constexpr uint32_t MT_TX_MCU_PORT_RX_Q0 = 0x20;
constexpr uint32_t MT_LMAC_AC00 = 0x00;
constexpr uint32_t MT_LMAC_ALTX0 = 0x10;

constexpr uint32_t MT_TXD0_Q_IDX = genmask(31, 25);
constexpr uint32_t MT_TXD0_PKT_FMT = genmask(24, 23);
constexpr uint32_t MT_TXD0_TX_BYTES = genmask(15, 0);

constexpr uint32_t MT_TXD1_LONG_FORMAT = bit(31);
constexpr uint32_t MT_TXD1_OWN_MAC = genmask(29, 24);
constexpr uint32_t MT_TXD1_TID = genmask(22, 20);
constexpr uint32_t MT_TXD1_HDR_FORMAT = genmask(17, 16);
constexpr uint32_t MT_TXD1_HDR_INFO = genmask(15, 11);
constexpr uint32_t MT_TXD1_WLAN_IDX = genmask(9, 0);

constexpr uint32_t MT_TXD2_FIX_RATE = bit(31);
constexpr uint32_t MT_TXD2_HTC_VLD = bit(13);
constexpr uint32_t MT_TXD2_MULTICAST = bit(10);
constexpr uint32_t MT_TXD2_FRAME_TYPE = genmask(5, 4);
constexpr uint32_t MT_TXD2_SUB_TYPE = genmask(3, 0);

constexpr uint32_t MT_TXD3_SN_VALID = bit(31);
constexpr uint32_t MT_TXD3_BA_DISABLE = bit(28);
constexpr uint32_t MT_TXD3_SEQ = genmask(27, 16);
constexpr uint32_t MT_TXD3_REM_TX_COUNT = genmask(15, 11);
constexpr uint32_t MT_TXD3_NO_ACK = bit(0);

constexpr uint32_t MT_TXD5_TX_STATUS_HOST = bit(10);
constexpr uint32_t MT_TXD5_PID = genmask(7, 0);

constexpr uint32_t MT_TXD6_TX_RATE = genmask(29, 16);
constexpr uint32_t MT_TXD6_SGI = genmask(15, 14);
constexpr uint32_t MT_TXD6_LDPC = bit(11);
constexpr uint32_t MT_TXD6_FIXED_BW = bit(2);
constexpr uint32_t MT_TXD6_BW = genmask(1, 0);

constexpr uint32_t MT_TXD7_TYPE = genmask(21, 20);
constexpr uint32_t MT_TXD7_SUB_TYPE = genmask(19, 16);
constexpr uint32_t MT_TXD7_SPE_IDX = genmask(15, 11);

/* TX rate encoding in MT_TXD6_TX_RATE */
constexpr uint32_t MT_TX_RATE_NSS = genmask(12, 10);
constexpr uint32_t MT_TX_RATE_MODE = genmask(9, 6);
constexpr uint32_t MT_TX_RATE_IDX = genmask(5, 0);
constexpr uint32_t MT_PHY_TYPE_CCK = 0;
constexpr uint32_t MT_PHY_TYPE_OFDM = 1;
constexpr uint32_t MT_PHY_TYPE_HT = 2;
constexpr uint32_t MT_PHY_TYPE_VHT = 4;

/* TX-free completion notification (PKT_TYPE_TXRX_NOTIFY, mt76_connac2_mac.h). */
constexpr uint32_t MT_TX_FREE_MSDU_CNT = genmask(25, 16);
constexpr uint32_t MT_TX_FREE_PAIR = bit(31);
constexpr uint32_t MT_TX_FREE_MSDU_ID = genmask(30, 16);
constexpr uint32_t MT_TX_FREE_STATUS = genmask(14, 13);
constexpr uint32_t MT_TX_FREE_COUNT = genmask(12, 0);

/* Hardware TXP (mt76_connac.h struct mt76_connac_hw_txp) */
constexpr uint16_t MT_MSDU_ID_VALID = 0x8000u;
constexpr uint16_t MT_TXD_LEN_LAST = 0x8000u;
constexpr uint16_t MT_TXD_LEN_MASK = 0x0fffu;

#pragma pack(push, 1)
struct MtkTxpPtr {
  uint32_t buf0;
  uint16_t len0;
  uint16_t len1;
  uint32_t buf1;
};

struct MtkHwTxp {
  uint16_t msdu_id[4];
  MtkTxpPtr ptr[2]; /* 8 + 24 = 32 bytes; matches Linux sizeof(mt76_connac_hw_txp) */
};
#pragma pack(pop)
static_assert(sizeof(MtkHwTxp) == 32, "MtkHwTxp must be 32 bytes");

/* RXD bitfields (mt76_connac2_mac.h / mt76_connac.h enum rx_pkt_type) */
constexpr uint32_t MT_RXD0_LENGTH = genmask(15, 0);
constexpr uint32_t MT_RXD0_PKT_FLAG = genmask(19, 16);
constexpr uint32_t MT_RXD0_PKT_TYPE = genmask(31, 27);
constexpr uint32_t PKT_TYPE_TXS = 0x0;
constexpr uint32_t PKT_TYPE_TXRXV = 0x1;
constexpr uint32_t PKT_TYPE_NORMAL = 0x2;
constexpr uint32_t PKT_TYPE_TXRX_NOTIFY = 0x6;
constexpr uint32_t PKT_TYPE_RX_EVENT = 0x7;
constexpr uint32_t PKT_TYPE_NORMAL_MCU = 0x8;

constexpr uint32_t MT_RXD1_NORMAL_GROUP_1 = bit(11);
constexpr uint32_t MT_RXD1_NORMAL_GROUP_2 = bit(12);
constexpr uint32_t MT_RXD1_NORMAL_GROUP_3 = bit(13);
constexpr uint32_t MT_RXD1_NORMAL_GROUP_4 = bit(14);
constexpr uint32_t MT_RXD1_NORMAL_GROUP_5 = bit(15);
constexpr uint32_t MT_RXD1_NORMAL_FCS_ERR = bit(27);
constexpr uint32_t MT_RXD1_NORMAL_BAND_IDX = bit(28);

constexpr uint32_t MT_RXD2_NORMAL_HDR_TRANS = bit(13);
constexpr uint32_t MT_RXD2_NORMAL_HDR_OFFSET = genmask(15, 14);
constexpr uint32_t MT_RXD2_NORMAL_AMSDU_ERR = bit(23);
constexpr uint32_t MT_RXD2_NORMAL_MAX_LEN_ERROR = bit(24);

constexpr uint32_t MT_RXD3_NORMAL_CH_FREQ = genmask(15, 8);
constexpr uint32_t MT_RXD4_NORMAL_PAYLOAD_FORMAT = genmask(1, 0);
constexpr uint32_t MT_PRXV_RCPI0 = genmask(7, 0);

/* --------------------------------------------------------------------------
 * Connac2 MCU Headers & Firmware Image Formats (mt76_connac_mcu.h)
 * -------------------------------------------------------------------------- */
constexpr uint8_t MCU_PKT_ID = 0xa0;
constexpr uint8_t MCU_Q_QUERY = 0;
constexpr uint8_t MCU_Q_SET = 1;
constexpr uint8_t MCU_Q_NA = 2;
constexpr uint8_t MCU_S2D_H2N = 0;
constexpr uint8_t MCU_CMD_UNI_EXT_ACK = 0x07; /* ACK(1) | UNI(2) | SET(4) */

constexpr uint16_t mcu_pq_id(uint16_t p, uint16_t q) {
  return static_cast<uint16_t>((p << 15) | (q << 10));
}

/* Standard MCU Command IDs */
constexpr uint8_t MCU_CMD_TARGET_ADDRESS_LEN_REQ = 0x01;
constexpr uint8_t MCU_CMD_FW_START_REQ = 0x02;
constexpr uint8_t MCU_CMD_NIC_POWER_CTRL = 0x04;
constexpr uint8_t MCU_CMD_PATCH_START_REQ = 0x05;
constexpr uint8_t MCU_CMD_PATCH_FINISH_REQ = 0x07;
constexpr uint8_t MCU_CMD_PATCH_SEM_CONTROL = 0x10;
constexpr uint8_t MCU_CMD_EXT_CID = 0xed;
constexpr uint8_t MCU_CMD_FW_SCATTER = 0xee;
constexpr uint8_t MCU_CMD_RESTART_DL_REQ = 0xef;

/* Extended MCU Command IDs (cid = 0xed, ext_cid = below) */
constexpr uint8_t MCU_EXT_CMD_CHANNEL_SWITCH = 0x08;
constexpr uint8_t MCU_EXT_CMD_EFUSE_BUFFER_MODE = 0x21;
constexpr uint8_t MCU_EXT_CMD_MAC_INIT_CTRL = 0x46;
constexpr uint8_t MCU_EXT_CMD_SET_RX_PATH = 0x4e;

/* CE MCU Command IDs (cid = below, ext_cid = 0, set_query = MCU_Q_SET) */
constexpr uint8_t MCU_CE_CMD_SET_RX_FILTER = 0x0a;
constexpr uint8_t MCU_CE_CMD_SET_CHAN_DOMAIN = 0x0f;
constexpr uint8_t MCU_CE_CMD_SET_EDCA_PARMS = 0x1d;
constexpr uint32_t MT7921_FILTER_FCSFAIL = bit(2);
constexpr uint32_t MT7921_FILTER_CONTROL = bit(5);
constexpr uint32_t MT7921_FILTER_OTHER_BSS = bit(6);
constexpr uint32_t MT7921_FILTER_ENABLE = bit(31);
constexpr uint8_t MT7921_FIF_BIT_SET = 1u; /* BIT(0) in Linux mt7921/mcu.c */
constexpr uint8_t MT7921_FIF_BIT_CLR = 2u; /* BIT(1) in Linux mt7921/mcu.c */

/* Unified MCU Command IDs */
constexpr uint16_t MCU_UNI_CMD_DEV_INFO_UPDATE = 0x01;
constexpr uint16_t MCU_UNI_CMD_BSS_INFO_UPDATE = 0x02;
constexpr uint16_t MCU_UNI_CMD_STA_REC_UPDATE = 0x03;

/* Patch semaphore status */
constexpr uint8_t PATCH_SEM_RELEASE = 0;
constexpr uint8_t PATCH_SEM_GET = 1;
constexpr uint8_t PATCH_IS_DL = 0;
constexpr uint8_t PATCH_NOT_DL_SEM_SUCCESS = 2;
constexpr uint8_t PATCH_REL_SEM_SUCCESS = 3;

/* Download mode flags */
constexpr uint32_t DL_MODE_ENCRYPT = bit(0);
constexpr uint32_t DL_MODE_KEY_IDX = genmask(2, 1);
constexpr uint32_t DL_MODE_RESET_SEC_IV = bit(3);
constexpr uint32_t DL_MODE_WORKING_PDA_CR4 = bit(4);
constexpr uint32_t DL_CONFIG_ENCRY_MODE_SEL = bit(6);
constexpr uint32_t DL_MODE_NEED_RSP = bit(31);

constexpr uint32_t FW_FEATURE_SET_ENCRYPT = bit(0);
constexpr uint32_t FW_FEATURE_SET_KEY_IDX = genmask(2, 1);
constexpr uint32_t FW_FEATURE_ENCRY_MODE = bit(4);
constexpr uint32_t FW_FEATURE_OVERRIDE_ADDR = bit(5);
constexpr uint32_t FW_FEATURE_NON_DL = bit(6);

constexpr uint32_t FW_START_OVERRIDE = bit(0);
constexpr uint32_t PATCH_SEC_TYPE_MASK = genmask(15, 0);
constexpr uint32_t PATCH_SEC_TYPE_INFO = 0x2;
constexpr uint32_t PATCH_SEC_ENC_TYPE_MASK = genmask(31, 24);
constexpr uint32_t PATCH_SEC_ENC_TYPE_PLAIN = 0x00;
constexpr uint32_t PATCH_SEC_ENC_TYPE_AES = 0x01;
constexpr uint32_t PATCH_SEC_ENC_TYPE_SCRAMBLE = 0x02;
constexpr uint32_t PATCH_SEC_ENC_AES_KEY_MASK = genmask(7, 0);
constexpr uint32_t PATCH_SEC_NOT_SUPPORT = 0xffffffffu;

/* Channel switch bandwidths */
constexpr uint8_t CMD_CBW_20MHZ = 0;
constexpr uint8_t CMD_CBW_40MHZ = 1;
constexpr uint8_t CMD_CBW_80MHZ = 2;
constexpr uint8_t CH_SWITCH_NORMAL = 0;
constexpr uint8_t CH_SWITCH_SCAN_BYPASS_DPD = 9;

/* UNI DEV/BSS/STA TLV tags */
constexpr uint16_t DEV_INFO_ACTIVE = 0;
constexpr uint16_t UNI_BSS_INFO_BASIC = 0;
constexpr uint16_t UNI_BSS_INFO_QBSS = 15;
constexpr uint16_t STA_REC_BASIC = 0;
constexpr uint16_t STA_REC_RA = 1;
constexpr uint16_t STA_REC_BA = 6;
constexpr uint16_t STA_REC_STATE = 7;
constexpr uint16_t STA_REC_HT = 9;
constexpr uint16_t STA_REC_VHT = 10;
constexpr uint16_t STA_REC_WTBL = 13;
constexpr uint16_t STA_REC_PHY = 0x15;
constexpr uint8_t STA_STATE_ASSOC = 2;
constexpr uint16_t EXTRA_INFO_VER = bit(0);
constexpr uint16_t EXTRA_INFO_NEW = bit(1);
constexpr uint32_t CONNECTION_INFRA_STA = bit(0) | bit(16);
constexpr uint32_t CONNECTION_INFRA_AP = bit(1) | bit(16);
constexpr uint32_t CONNECTION_IBSS_ADHOC = bit(2) | bit(18);
constexpr uint32_t CONNECTION_INFRA_BC = bit(5) | bit(16);
constexpr uint8_t CONN_STATE_DISCONNECT = 0;
constexpr uint8_t CONN_STATE_CONNECT = 1;
constexpr uint8_t CONN_STATE_PORT_SECURE = 2;

/* MT7921/MT7922 WTBL & Block Ack constants */
constexpr uint16_t MT792X_WTBL_SIZE = 20;
constexpr uint16_t MT792X_WTBL_RESERVED = 19;
constexpr uint8_t WTBL_RESET_AND_SET = 1;
constexpr uint8_t WTBL_SET = 2;
constexpr uint16_t WTBL_GENERIC = 0;
constexpr uint16_t WTBL_RX = 1;
constexpr uint16_t WTBL_HT = 2;
constexpr uint16_t WTBL_VHT = 3;
constexpr uint16_t WTBL_HDR_TRANS = 6;
constexpr uint16_t WTBL_BA = 8;
constexpr uint16_t WTBL_SMPS = 13;
constexpr uint8_t MT_BA_TYPE_ORIGINATOR = 1;
constexpr uint8_t MT_BA_TYPE_RECIPIENT = 2;
constexpr uint8_t RST_BA_MAC_TID_MATCH = 0;

#pragma pack(push, 1)
struct MtkMcuTxd {
  uint32_t txd[8];
  uint16_t len;
  uint16_t pq_id;
  uint8_t cid;
  uint8_t pkt_type;
  uint8_t set_query;
  uint8_t seq;
  uint8_t uc_d2b0_rev;
  uint8_t ext_cid;
  uint8_t s2d_index;
  uint8_t ext_cid_ack;
  uint32_t rsv[5];
};
static_assert(sizeof(MtkMcuTxd) == 64, "MtkMcuTxd must be 64 bytes");

struct MtkMcuUniTxd {
  uint32_t txd[8];
  uint16_t len;
  uint16_t cid;
  uint8_t rsv;
  uint8_t pkt_type;
  uint8_t frag_n;
  uint8_t seq;
  uint16_t checksum;
  uint8_t s2d_index;
  uint8_t option;
  uint8_t rsv1[4];
};
static_assert(sizeof(MtkMcuUniTxd) == 48, "MtkMcuUniTxd must be 48 bytes");

struct MtkMcuRxd {
  uint32_t rxd[6];
  uint16_t len;
  uint16_t pkt_type_id;
  uint8_t eid;
  uint8_t seq;
  uint8_t option;
  uint8_t rsv;
  uint8_t ext_eid;
  uint8_t rsv1[2];
  uint8_t s2d_index;
};
static_assert(sizeof(MtkMcuRxd) == 36, "MtkMcuRxd must be 36 bytes");

struct MtkPatchHdr {
  char build_date[16];
  char platform[4];
  uint32_t hw_sw_ver;
  uint32_t patch_ver;
  uint16_t checksum;
  uint16_t rsv;
  struct {
    uint32_t patch_ver;
    uint32_t subsys;
    uint32_t feature;
    uint32_t n_region;
    uint32_t crc;
    uint32_t rsv[11];
  } desc;
};

struct MtkPatchSec {
  uint32_t type;
  uint32_t offs;
  uint32_t size;
  uint32_t addr;
  uint32_t len;
  uint32_t sec_key_idx;
  uint32_t align_len;
  uint32_t rsv[9];
};

struct MtkFwTrailer {
  uint8_t chip_id;
  uint8_t eco_code;
  uint8_t n_region;
  uint8_t format_ver;
  uint8_t format_flag;
  uint8_t rsv[2];
  char fw_ver[10];
  char build_date[15];
  uint32_t crc;
};

struct MtkFwRegion {
  uint32_t decomp_crc;
  uint32_t decomp_len;
  uint32_t decomp_blk_sz;
  uint8_t rsv[4];
  uint32_t addr;
  uint32_t len;
  uint8_t feature_set;
  uint8_t type;
  uint8_t rsv1[14];
};
#pragma pack(pop)

} // namespace mtk
