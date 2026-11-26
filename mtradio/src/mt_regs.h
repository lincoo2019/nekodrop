// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT7902 (connac2, mt7921 family) registers and MCU constants, from
 * Linux mt76 (mt792x_regs.h, mt7921/regs.h, mt76_connac*.h; BSD-3-Clause-Clear
 * / ISC, MediaTek and Lorenzo Bianconi). Only what this user-mode driver uses. */
#pragma once

#include <cstdint>

namespace mt {

constexpr uint32_t bit(unsigned n) { return 1u << n; }

/* ---- Chip identity (reached through the L1 remap window) ---- */
constexpr uint32_t kHwChipId = 0x70010200;
constexpr uint32_t kHwRev = 0x70010204;

/* ---- L1 remap window: one 64 KiB window onto the chip's bus ---- */
constexpr uint32_t kHifRemapL1 = 0xfe000 + 0x24c; /* MT_INFRA(0x24c) */
constexpr uint32_t kHifRemapBaseL1 = 0x40000;

/* ---- Ownership between host and firmware ---- */
constexpr uint32_t kConnOnLpctl = 0x7c060010;
constexpr uint32_t kLpcrHostSetOwn = bit(0);
constexpr uint32_t kLpcrHostClrOwn = bit(1);
constexpr uint32_t kLpcrHostOwnSync = bit(2);
constexpr uint32_t kTopLpcrHostBand0 = 0x18060010;
constexpr uint32_t kTopLpcrHostFwOwn = bit(0);
constexpr uint32_t kTopLpcrHostDrvOwn = bit(1);

constexpr uint32_t kConnOnMisc = 0x7c0600f0;
constexpr uint32_t kTopMiscFwState = 0x7;
constexpr uint32_t kTopMisc2FwPwrOn = bit(0);
constexpr uint32_t kTopMisc2FwN9Rdy = 0x3;

/* WFSYS software reset (connac2 address). */
constexpr uint32_t kWfsysSwRst = 0x18000140;
constexpr uint32_t kWfsysSwRstB = bit(0);
constexpr uint32_t kWfsysSwInitDone = bit(4);

/* ---- PCIe MAC ---- */
constexpr uint32_t kPcieMacIntEnable = 0x10000 + 0x188;
constexpr uint32_t kPcieMacPm = 0x10000 + 0x194;
constexpr uint32_t kPcieMacPmL0sDis = bit(8);

/* ---- WFDMA0 ---- */
constexpr uint32_t kWfdma0 = 0xd4000;
constexpr uint32_t wfdma0(uint32_t ofs) { return kWfdma0 + ofs; }
constexpr uint32_t kWfdma0Rst = wfdma0(0x100);
constexpr uint32_t kWfdma0RstLogicRst = bit(4);
constexpr uint32_t kWfdma0RstDmashdlAllRst = bit(5);
constexpr uint32_t kMcuCmd = wfdma0(0x1f0);
constexpr uint32_t kHostIntSta = wfdma0(0x200);
constexpr uint32_t kHostIntEna = wfdma0(0x204);
constexpr uint32_t kGloCfg = wfdma0(0x208);
constexpr uint32_t kGloTxDmaEn = bit(0);
constexpr uint32_t kGloTxDmaBusy = bit(1);
constexpr uint32_t kGloRxDmaEn = bit(2);
constexpr uint32_t kGloRxDmaBusy = bit(3);
constexpr uint32_t kGloDmaSizeShift = 4; /* 2 bits */
constexpr uint32_t kGloTxWbDdone = bit(6);
constexpr uint32_t kGloFifoDisCheck = bit(11);
constexpr uint32_t kGloFifoLittleEndian = bit(12);
constexpr uint32_t kGloRxWbDdone = bit(13);
constexpr uint32_t kGloDispBasePtrChainEn = bit(15);
constexpr uint32_t kGloOmitRxInfoPfet2 = bit(21);
constexpr uint32_t kGloOmitRxInfo = bit(27);
constexpr uint32_t kGloOmitTxInfo = bit(28);
constexpr uint32_t kGloClkGatDis = bit(30);
constexpr uint32_t kRstDtxPtr = wfdma0(0x20c);
constexpr uint32_t kGloCfgExt0 = wfdma0(0x2b0);
constexpr uint32_t kGloCfgExt0TxDmashdlEn = bit(6);
constexpr uint32_t kPriDlyIntCfg0 = wfdma0(0x2f0);
constexpr uint32_t tx_ring_ext_ctrl(unsigned ring) { return wfdma0(0x600 + 4 * ring); }
constexpr uint32_t rx_ring_ext_ctrl(unsigned ring) { return wfdma0(0x680 + 4 * ring); }
constexpr uint32_t prefetch(uint32_t base, uint32_t depth) { return base << 16 | depth; }

constexpr uint32_t kTxRingBase = wfdma0(0x300);
constexpr uint32_t kRxRingBase = wfdma0(0x500);
constexpr uint32_t kRingRegStride = 0x10; /* desc_base, ring_size, cpu_idx, dma_idx */

constexpr uint32_t kDmashdlSwControl = 0x7c026000 + 0x004;
constexpr uint32_t kDmashdlBypass = bit(28);
constexpr uint32_t kWfdmaDummyCr = 0x54000000 + 0x120;
constexpr uint32_t kWfdmaNeedReinit = bit(1);

/* Rings: MT7902 puts the MCU (WM) command ring at 15, not 17, has no WA
 * event ring, and shares RX ring 0 between events and TX-done. */
constexpr unsigned kTxRingData = 0;
constexpr unsigned kTxRingMcuWm = 15;
constexpr unsigned kTxRingFwdl = 16;
constexpr unsigned kRxRingMcu = 0;
constexpr unsigned kRxRingData = 2;

/* ---- DMA descriptor ---- */
struct Desc {
  uint32_t buf0;
  uint32_t ctrl;
  uint32_t buf1;
  uint32_t info;
};
constexpr uint32_t kDescSdLen1Mask = 0x3fff;     /* bits 13:0 */
constexpr uint32_t kDescLastSec1 = bit(14);
constexpr uint32_t kDescSdLen0Shift = 16;        /* bits 29:16 */
constexpr uint32_t kDescSdLen0Mask = 0x3fff;
constexpr uint32_t kDescLastSec0 = bit(30);
constexpr uint32_t kDescDmaDone = bit(31);

/* ---- MCU messages ---- */
constexpr uint8_t kMcuCmdTargetAddressLenReq = 0x01;
constexpr uint8_t kMcuCmdFwStartReq = 0x02;
constexpr uint8_t kMcuCmdNicPowerCtrl = 0x04;
constexpr uint8_t kMcuCmdPatchStartReq = 0x05;
constexpr uint8_t kMcuCmdPatchFinishReq = 0x07;
constexpr uint8_t kMcuCmdPatchSemControl = 0x10;
constexpr uint8_t kMcuCmdExtCid = 0xed;
constexpr uint8_t kMcuCmdFwScatter = 0xee;
constexpr uint8_t kMcuCeCmdGetNicCapab = 0x8a;
constexpr uint8_t kMcuCeCmdFwlog2Host = 0xc5;

constexpr uint8_t kMcuPktId = 0xa0;
constexpr uint8_t kMcuQSet = 1;
constexpr uint8_t kMcuQQuery = 0;
constexpr uint8_t kMcuQNa = 3;
constexpr uint8_t kMcuS2dH2n = 0;
constexpr uint32_t kTxMcuPortRxQ0 = 0x20;
constexpr uint32_t kTxPortIdxMcu = 1;
constexpr uint32_t kTxTypeCmd = 2;
constexpr uint32_t kHdrFormatCmd = 1;
constexpr uint32_t kTxd1LongFormat = bit(31);

enum PatchSem : uint8_t { kPatchNotDlSemFail, kPatchIsDl, kPatchNotDlSemSuccess, kPatchRelSemSuccess };
constexpr uint32_t kPatchSemRelease = 0, kPatchSemGet = 1;
constexpr uint32_t kMcuPatchAddress = 0x200000;

constexpr uint32_t kFwFeatureSetEncrypt = bit(0);
constexpr uint32_t kFwFeatureSetKeyIdxShift = 1; /* 2 bits */
constexpr uint32_t kFwFeatureEncryMode = bit(4);
constexpr uint32_t kFwFeatureOverrideAddr = bit(5);
constexpr uint32_t kFwFeatureNonDl = bit(6);
constexpr uint32_t kDlModeEncrypt = bit(0);
constexpr uint32_t kDlModeKeyIdxShift = 1;
constexpr uint32_t kDlModeResetSecIv = bit(3);
constexpr uint32_t kDlConfigEncryModeSel = bit(6);
constexpr uint32_t kDlModeNeedRsp = bit(31);
constexpr uint32_t kFwStartOverride = bit(0);

constexpr uint32_t kPatchSecTypeMask = 0xffff;
constexpr uint32_t kPatchSecTypeInfo = 0x2;
constexpr uint32_t kPatchSecNotSupport = 0xffffffff;
constexpr unsigned kPatchSecEncTypeShift = 24;
constexpr uint32_t kPatchSecEncTypePlain = 0, kPatchSecEncTypeAes = 1, kPatchSecEncTypeScramble = 2;
constexpr uint32_t kPatchSecEncAesKeyMask = 0xff;

/* RX descriptor word 0 */
constexpr uint32_t kRxd0PktTypeShift = 27;
constexpr uint32_t kPktTypeNormal = 2, kPktTypeRxEvent = 7, kPktTypeNormalMcu = 8;

constexpr unsigned kNicCapMacAddr = 7;

/* UNI command options (MCU_CMD_ACK | MCU_CMD_UNI | MCU_CMD_SET) */
constexpr uint8_t kMcuCmdUniExtAck = 0x7, kMcuCmdUniQueryAck = 0x3;
constexpr uint8_t kMcuUniDevInfoUpdate = 0x01, kMcuUniBssInfoUpdate = 0x02, kMcuUniSniffer = 0x24;
constexpr uint8_t kMcuExtChannelSwitch = 0x08, kMcuExtEfuseBufferMode = 0x21,
                  kMcuExtMacInitCtrl = 0x46, kMcuExtSetRxPath = 0x4e;
constexpr uint8_t kMcuCeSetRxFilter = 0x0a, kMcuCeSetChanDomain = 0x0f;
constexpr uint32_t kConnectionIbssAdhoc = bit(2) | bit(18);
/* As mt76 registers a monitor interface (CONNECTION_INFRA_AP) and the
 * stations under an AP (CONNECTION_INFRA_STA). */
constexpr uint32_t kConnectionInfraAp = bit(1) | bit(16), kConnectionInfraSta = bit(0) | bit(16);
constexpr uint32_t kFilterFcsFail = bit(2), kFilterControl = bit(5), kFilterOtherBss = bit(6),
                   kFilterEnable = bit(31);

/* SWDEF: firmware operation mode, set before the download */
constexpr uint32_t kSwdefMode = 0x41f200 + 0x3c;

/* RX packet types and descriptor fields (mt76_connac2_mac.h) */
constexpr uint32_t kPktTypeTxs = 0, kPktTypeTxrxNotify = 6;
constexpr uint32_t kRxd1Group1 = bit(11), kRxd1Group2 = bit(12), kRxd1Group3 = bit(13),
                   kRxd1Group4 = bit(14), kRxd1Group5 = bit(15);
constexpr uint32_t kRxd1FcsErr = bit(27), kRxd1BandIdx = bit(28);
constexpr uint32_t kRxd2HdrTrans = bit(13), kRxd2HdrOffsetShift = 14; /* 2 bits */
constexpr uint32_t kRxd2AmsduErr = bit(23), kRxd2MaxLenError = bit(24);
constexpr uint32_t kRxd3ChFreqShift = 8; /* 8 bits */
constexpr uint32_t kRxd4PayloadFormatMask = 0x3;

/* TXD (MT_TXD*) */
constexpr unsigned kTxdSize = 32;
constexpr uint32_t kTxTypeCt = 0;
constexpr uint32_t kLmacAc01 = 0x1, kLmacAltx0 = 0x10;
constexpr uint32_t kHdrFormat80211 = 2;
constexpr uint32_t kTxd2FixRate = bit(31), kTxd2HtcVld = bit(13), kTxd2Multicast = bit(10);
constexpr uint32_t kTxd3SnValid = bit(31), kTxd3BaDisable = bit(28), kTxd3NoAck = bit(0);
constexpr uint32_t kTxd6FixedBw = bit(2);
constexpr uint16_t kMsduIdValid = 0x8000;
constexpr uint16_t kTxdLenLast = 0x8000;
constexpr unsigned kTxFrameOffset = 256;
constexpr uint8_t kPhyTypeCck = 0, kPhyTypeOfdm = 1, kPhyTypeHt = 2;

#pragma pack(push, 1)
struct McuTxd { /* mt76_connac2_mcu_txd: hardware TXD, then the command header */
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
static_assert(sizeof(McuTxd) == 64, "mcu txd");

struct McuUniTxd { /* mt76_connac2_mcu_uni_txd */
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
static_assert(sizeof(McuUniTxd) == 48, "mcu uni txd");

struct McuRxd { /* mt76_connac2_mcu_rxd header */
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
static_assert(sizeof(McuRxd) == 36, "mcu rxd");

struct PatchHdr { /* big-endian fields */
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
static_assert(sizeof(PatchHdr) == 96, "patch hdr");

struct PatchSec { /* big-endian fields */
  uint32_t type;
  uint32_t offs;
  uint32_t size;
  uint32_t addr;
  uint32_t len;
  uint32_t sec_key_idx;
  uint32_t align_len;
  uint32_t rsv[9];
};
static_assert(sizeof(PatchSec) == 64, "patch sec");

struct FwTrailer {
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
static_assert(sizeof(FwTrailer) == 36, "fw trailer");

struct FwRegion {
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
static_assert(sizeof(FwRegion) == 40, "fw region");
#pragma pack(pop)

} // namespace mt
