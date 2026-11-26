// SPDX-License-Identifier: GPL-2.0-only
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "mtk_dev.h"
#include "mtk_platform.h"

namespace mtk {

namespace {

uint64_t now_ms() { return plat::now_ms(); }

bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    error = "cannot open firmware file: " + path;
    return false;
  }
  const auto sz = f.tellg();
  if (sz <= 0) {
    error = "empty firmware file: " + path;
    return false;
  }
  out.resize(static_cast<size_t>(sz));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(out.data()), sz)) {
    error = "failed reading firmware file: " + path;
    return false;
  }
  return true;
}

std::string hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", v);
  return buf;
}

/* Compute center channel for 40/80 MHz 5 GHz channels (standard 802.11 blocks). */
uint8_t center_channel_for(uint8_t ch, unsigned width_mhz) {
  if (width_mhz <= 20 || ch <= 14) return ch;
  if (width_mhz == 40) {
    if (ch >= 36 && ch <= 144) return static_cast<uint8_t>(((ch - 36) / 8) * 8 + 38);
    if (ch == 149 || ch == 153) return 151;
    if (ch == 157 || ch == 161) return 159;
    return ch;
  }
  /* 80 MHz */
  if (ch >= 36 && ch <= 48) return 42;
  if (ch >= 52 && ch <= 64) return 58;
  if (ch >= 100 && ch <= 112) return 106;
  if (ch >= 116 && ch <= 128) return 122;
  if (ch >= 132 && ch <= 144) return 138;
  if (ch >= 149 && ch <= 161) return 155;
  return ch;
}

/* Map CCK rate (in 500 kbps units: 2, 4, 11, 22) to Connac2 TX rate encoding. */
uint16_t cck_rate_val(unsigned mbps_x2) {
  const uint8_t idx = mbps_x2 >= 22 ? 3 : mbps_x2 >= 11 ? 2 : mbps_x2 >= 4 ? 1 : 0;
  return static_cast<uint16_t>(field_prep(MT_TX_RATE_MODE, MT_PHY_TYPE_CCK) |
                               field_prep(MT_TX_RATE_IDX, idx));
}

/* Map OFDM Mbps to MT76 Connac2 OFDM rate index (mt76_connac_mac.c). */
uint16_t ofdm_rate_val(unsigned mbps) {
  uint8_t idx = 0x0b; /* 6 Mbps */
  switch (mbps) {
    case 6: idx = 0x0b; break;
    case 9: idx = 0x0f; break;
    case 12: idx = 0x0a; break;
    case 18: idx = 0x0e; break;
    case 24: idx = 0x09; break;
    case 36: idx = 0x0d; break;
    case 48: idx = 0x08; break;
    case 54: idx = 0x0c; break;
    default: idx = 0x0b; break;
  }
  return static_cast<uint16_t>(field_prep(MT_TX_RATE_MODE, MT_PHY_TYPE_OFDM) |
                               field_prep(MT_TX_RATE_IDX, idx));
}

/* Map HT MCS (0..15) to MT76 Connac2 TX rate encoding (mt7915/mac.c). */
uint16_t ht_rate_val(uint8_t mcs) {
  const uint8_t m = (mcs <= 15u) ? mcs : 7u;
  const uint8_t nss_minus_1 = static_cast<uint8_t>(m >> 3); /* 0 for MCS 0..7, 1 for MCS 8..15 */
  return static_cast<uint16_t>(field_prep(MT_TX_RATE_MODE, MT_PHY_TYPE_HT) |
                               field_prep(MT_TX_RATE_NSS, nss_minus_1) |
                               field_prep(MT_TX_RATE_IDX, m));
}

/* Map VHT MCS (0..9) + NSS (1..2) to MT76 Connac2 TX rate encoding. */
uint16_t vht_rate_val(uint8_t mcs, uint8_t nss) {
  const uint8_t m = (mcs <= 9u) ? mcs : 7u;
  const uint8_t n = (nss >= 1u && nss <= 2u) ? static_cast<uint8_t>(nss - 1u) : 0u;
  return static_cast<uint16_t>(field_prep(MT_TX_RATE_MODE, MT_PHY_TYPE_VHT) |
                               field_prep(MT_TX_RATE_NSS, n) |
                               field_prep(MT_TX_RATE_IDX, m));
}

inline uint16_t read_le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t read_le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string mac_str(const uint8_t mac[6]) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                mac[4], mac[5]);
  return buf;
}

size_t ieee80211_hdrlen(uint16_t fc) {
  const uint16_t ftype = fc & 0x000cu;
  const uint16_t stype = fc & 0x00f0u;
  if (ftype == 0x0008u) { /* Data */
    size_t len = 24;
    if ((fc & 0x0300u) == 0x0300u) len += 6; /* ToDS & FromDS */
    if (stype & 0x0080u) len += 2;           /* QoS */
    if (fc & 0x8000u) len += 4;              /* Order / HT Control */
    return len;
  }
  if (ftype == 0x0000u) { /* Mgmt */
    size_t len = 24;
    if (fc & 0x8000u) len += 4;
    return len;
  }
  return 16; /* Control */
}

/* Helper to build Connac2 MCU_UNI_CMD_STA_REC_UPDATE TLV payloads
 * (matching mtradio/src/mt7902_sta.cpp and mt76_connac_mcu.c). */
class StaReq {
public:
  StaReq(uint8_t wcid, uint8_t omac) {
    _b = {omac /* bss_idx */, wcid, 0, 0 /* tlv_num */, 1 /* is_tlv_append */,
          omac /* muar_idx */, 0 /* wlan_idx_hi */, 0};
  }
  uint8_t *add(uint16_t tag, uint16_t len) {
    const size_t at = put(tag, len);
    bump(2);
    return _b.data() + at;
  }
  void wtbl(uint8_t op, uint8_t wcid) {
    _wtbl = put(STA_REC_WTBL, 4);
    bump(2);
    _wtbl_hdr = _b.size();
    const uint8_t hdr[8] = {wcid, op, 0, 0, 0, 0, 0, 0};
    _b.insert(_b.end(), hdr, hdr + 8);
    grow_wtbl(8);
  }
  uint8_t *wadd(uint16_t tag, uint16_t len) {
    const size_t at = put(tag, len);
    bump(_wtbl_hdr + 2);
    grow_wtbl(len);
    return _b.data() + at;
  }
  const std::vector<uint8_t> &bytes() const { return _b; }

private:
  size_t put(uint16_t tag, uint16_t len) {
    const size_t at = _b.size();
    _b.resize(at + len, 0);
    std::memcpy(&_b[at], &tag, 2);
    std::memcpy(&_b[at + 2], &len, 2);
    return at;
  }
  void bump(size_t at) {
    uint16_t n = 0;
    std::memcpy(&n, &_b[at], 2);
    n++;
    std::memcpy(&_b[at], &n, 2);
  }
  void grow_wtbl(uint16_t by) {
    uint16_t len = 0;
    std::memcpy(&len, &_b[_wtbl + 2], 2);
    len = static_cast<uint16_t>(len + by);
    std::memcpy(&_b[_wtbl + 2], &len, 2);
  }
  std::vector<uint8_t> _b;
  size_t _wtbl = 0, _wtbl_hdr = 0;
};

template <class T> void put_le(uint8_t *at, size_t off, T v) {
  std::memcpy(at + off, &v, sizeof(v));
}

} // namespace

bool parse_tx_radiotap(const uint8_t *buf, size_t len, const uint8_t *&frame, size_t &frame_len,
                       TxParams &params) {
  if (!buf || len < 8) return false;
  const uint16_t rt_len = read_le16(buf + 2);
  if (rt_len < 8 || rt_len >= len) return false;

  frame = buf + rt_len;
  frame_len = len - rt_len;
  if (frame_len < 16) return false;

  /* Default fallback if client sends minimal radiotap without RATE/MCS fields.
   * Use 24 Mbps (not fragile 54 Mbps) for fixed-rate fallback before firmware RA activates. */
  params = TxParams{};
  params.rate_mbps = ((frame[0] & 0x0cu) == 0x08u) ? 24u : 6u;

  const uint32_t present0 = read_le32(buf + 4);
  size_t pos = 8;
  uint32_t cur = present0;
  while (cur & (1u << 31)) {
    if (pos + 4 > rt_len) return true;
    cur = read_le32(buf + pos);
    pos += 4;
  }

  /* Standard radiotap field (align, size) table for bits 0..21. */
  struct RtField {
    uint8_t align;
    uint8_t size;
  };
  static constexpr RtField kFields[22] = {
      {8, 8},  /* 0: TSFT */
      {1, 1},  /* 1: FLAGS */
      {1, 1},  /* 2: RATE (500 kbps units) */
      {2, 4},  /* 3: CHANNEL */
      {2, 2},  /* 4: FHSS */
      {1, 1},  /* 5: DBM_ANTSIGNAL */
      {1, 1},  /* 6: DBM_ANTNOISE */
      {2, 2},  /* 7: LOCK_QUALITY */
      {2, 2},  /* 8: TX_ATTENUATION */
      {2, 2},  /* 9: DB_TX_ATTENUATION */
      {1, 1},  /* 10: DBM_TX_POWER */
      {1, 1},  /* 11: ANTENNA */
      {1, 1},  /* 12: DB_ANTSIGNAL */
      {1, 1},  /* 13: DB_ANTNOISE */
      {2, 2},  /* 14: RX_FLAGS */
      {2, 2},  /* 15: TX_FLAGS */
      {1, 1},  /* 16: RTS_RETRIES */
      {1, 1},  /* 17: DATA_RETRIES */
      {4, 8},  /* 18: XCHANNEL */
      {1, 3},  /* 19: MCS */
      {4, 8},  /* 20: A-MPDU STATUS */
      {2, 12}, /* 21: VHT */
  };

  for (unsigned b = 0; b < 22; b++) {
    if (!(present0 & (1u << b))) continue;
    const size_t align = kFields[b].align;
    const size_t sz = kFields[b].size;
    pos = (pos + align - 1) & ~(align - 1);
    if (pos + sz > rt_len) break;

    if (b == 2) { /* RATE */
      const uint8_t r500 = buf[pos];
      if (r500 > 0) {
        params.is_ht = false;
        params.is_vht = false;
        params.rate_mbps = static_cast<uint8_t>(r500 / 2u);
      }
    } else if (b == 15) { /* TX_FLAGS */
      const uint16_t tx_flags = read_le16(buf + pos);
      params.no_ack = (tx_flags & 0x0008u) != 0;
    } else if (b == 19) { /* MCS */
      const uint8_t known = buf[pos];
      const uint8_t flags = buf[pos + 1];
      const uint8_t mcs = buf[pos + 2];
      if ((known & 0x02u) && mcs <= 15u) {
        params.is_ht = true;
        params.is_vht = false;
        params.mcs = mcs;
        if (known & 0x04u) params.sgi = (flags & 0x04u) != 0;
        if (known & 0x01u) params.bw = ((flags & 0x03u) == 1u) ? 1u : 0u;
      }
    } else if (b == 21) { /* VHT */
      const uint16_t known = read_le16(buf + pos);
      const uint8_t flags = buf[pos + 2];
      const uint8_t bw_raw = buf[pos + 3];
      const uint8_t mcs_nss0 = buf[pos + 4];
      const uint8_t nss = static_cast<uint8_t>(mcs_nss0 & 0x0fu);
      const uint8_t mcs = static_cast<uint8_t>((mcs_nss0 >> 4) & 0x0fu);
      if (nss >= 1u && mcs <= 9u) {
        params.is_vht = true;
        params.is_ht = false;
        params.mcs = mcs;
        params.nss = (nss > 2u) ? 2u : nss;
        if (known & 0x0004u) params.sgi = (flags & 0x04u) != 0;
        if (known & 0x0040u) {
          if (bw_raw == 4u) params.bw = 2u;      /* 80 MHz */
          else if (bw_raw == 1u) params.bw = 1u; /* 40 MHz */
          else params.bw = 0u;                   /* 20 MHz */
        }
      }
    }
    pos += sz;
  }

  return true;
}

MtkDevice::~MtkDevice() { close(); }

void MtkDevice::log(const std::string &msg) {
  if (_log) _log(msg);
}

/* --------------------------------------------------------------------------
 * Register Address Translation & Access
 * -------------------------------------------------------------------------- */
uint32_t MtkDevice::remap_l1(uint32_t addr) {
  const uint32_t offset = addr & MT_HIF_REMAP_L1_OFFSET;
  const uint32_t base = (addr >> 16) & 0xffffu;
  if (_l1_base != base) {
    uint32_t cur = _proxy.read32(MT_HIF_REMAP_L1);
    cur = (cur & ~MT_HIF_REMAP_L1_MASK) | field_prep(MT_HIF_REMAP_L1_MASK, base);
    _proxy.write32(MT_HIF_REMAP_L1, cur);
    (void)_proxy.read32(MT_HIF_REMAP_L1); /* flush posted write */
    _l1_base = base;
  }
  return MT_HIF_REMAP_BASE_L1 + offset;
}

uint32_t MtkDevice::translate_addr(uint32_t addr) {
  if (addr < 0x100000u) return addr;
  for (const auto &e : kFixedRegMap) {
    if (addr >= e.phys && (addr - e.phys) < e.size) {
      return e.maps + (addr - e.phys);
    }
  }
  if ((addr >= 0x18000000u && addr < 0x18c00000u) ||
      (addr >= 0x70000000u && addr < 0x78000000u) ||
      (addr >= 0x7c000000u && addr < 0x7c400000u)) {
    return remap_l1(addr);
  }
  log("WARNING: unmapped register address " + hex32(addr));
  return 0;
}

uint32_t MtkDevice::rr(uint32_t addr) {
  return _proxy.read32(translate_addr(addr));
}

void MtkDevice::wr(uint32_t addr, uint32_t val) {
  _proxy.write32(translate_addr(addr), val);
}

void MtkDevice::rmw(uint32_t addr, uint32_t mask, uint32_t val) {
  const uint32_t off = translate_addr(addr);
  uint32_t cur = _proxy.read32(off);
  cur = (cur & ~mask) | val;
  _proxy.write32(off, cur);
}

uint32_t MtkDevice::l1_rr(uint32_t addr) {
  return _proxy.read32(remap_l1(addr));
}

void MtkDevice::l1_wr(uint32_t addr, uint32_t val) {
  _proxy.write32(remap_l1(addr), val);
}

bool MtkDevice::poll_reg(uint32_t addr, uint32_t mask, uint32_t want, unsigned timeout_ms) {
  const uint64_t deadline = now_ms() + timeout_ms;
  for (;;) {
    const uint32_t v = rr(addr);
    if (v != 0xdeadfeedu && v != 0xffffffffu && (v & mask) == want) return true;
    if (now_ms() >= deadline) return false;
    plat::sleep_ms(1);
  }
}

/* --------------------------------------------------------------------------
 * Probe & Power Management
 * -------------------------------------------------------------------------- */
bool MtkDevice::driver_own(std::string &error) {
  /* If already driver-owned (OWN_SYNC == 0 and WFSYS_RST valid), do NOT write CLR_OWN again:
   * writing redundant CLR_OWN while in ROM mode can desync the ROM's LPCTL state machine. */
  {
    const uint32_t lpctl = rr(MT_CONN_ON_LPCTL);
    const uint32_t rst = rr(MT_WFSYS_SW_RST_B);
    if (lpctl != 0xdeadfeedu && (lpctl & PCIE_LPCR_HOST_OWN_SYNC) == 0 &&
        rst != 0xdeadfeedu && rst != 0xffffffffu) {
      return true;
    }
  }

  const uint64_t deadline = now_ms() + 500;
  for (;;) {
    wr(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);

    for (int i = 0; i < 50; i++) {
      const uint32_t lpctl = rr(MT_CONN_ON_LPCTL);
      const uint32_t rst = rr(MT_WFSYS_SW_RST_B);
      if (lpctl != 0xdeadfeedu && (lpctl & PCIE_LPCR_HOST_OWN_SYNC) == 0 &&
          rst != 0xdeadfeedu && rst != 0xffffffffu) {
        return true;
      }
      if (now_ms() >= deadline) {
        error = "timeout waiting for driver own (LPCTL=" + hex32(lpctl) +
                " WFSYS_RST=" + hex32(rst) + ")";
        return false;
      }
      plat::sleep_ms(1);
    }
  }
}

bool MtkDevice::wfsys_reset(std::string &error) {
  /* Exact match to upstream mt792x_wfsys_reset_default():
   * toggle WFSYS_SW_RST_B and wait for WFSYS_SW_INIT_DONE + 50 ms ROM boot.
   * Do NOT write SET_OWN or redundant CLR_OWN, and do NOT poll MISC==1 (it stays 0 until FW). */
  clear(MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);
  plat::sleep_ms(50);
  set(MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);
  if (!poll_reg(MT_WFSYS_SW_RST_B, WFSYS_SW_INIT_DONE, WFSYS_SW_INIT_DONE, 500)) {
    error = "timeout waiting for WFSYS_SW_INIT_DONE (RST=" + hex32(rr(MT_WFSYS_SW_RST_B)) + ")";
    return false;
  }
  plat::sleep_ms(50);
  return true;
}

bool MtkDevice::probe(std::string &error) {
  if (!_proxy.is_open()) {
    if (!_proxy.open(error)) return false;
  }

  log("regproxy opened: PCI " + hex32((_proxy.vendor() << 16) | _proxy.device()) +
      " BAR0=" + hex32(_proxy.bar_len()));

  if (_proxy.vendor() != PCI_VENDOR_MEDIATEK && _proxy.vendor() != 0x0b48) {
    error = "bound card is not MediaTek (vendor=" + hex32(_proxy.vendor()) + ")";
    return false;
  }
  if (_proxy.bar_len() < 0x100000u) {
    error = "BAR0 too small for MT7921/MT7922 (" + hex32(_proxy.bar_len()) + ", need >= 1 MiB)";
    return false;
  }

  /* Take driver ownership before reading chip ID registers. */
  if (!driver_own(error)) return false;

  _chip_id = l1_rr(MT_HW_CHIPID) & 0xffffu;
  _hw_rev = l1_rr(MT_HW_REV) & 0xffu;
  const uint32_t pcie_cmd = _proxy.read32(0x11004);
  const uint32_t pcie_pm = _proxy.read32(MT_PCIE_MAC_PM);
  log("MTK chip_id=" + hex32(_chip_id) + " hw_rev=" + hex32(_hw_rev) +
      " pcie_cfg_cmd=" + hex32(pcie_cmd) + " pcie_pm=" + hex32(pcie_pm));

  if (_chip_id != 0x7922 && _chip_id != 0x7961 && _chip_id != 0x7920) {
    error = "unexpected MediaTek chip_id " + hex32(_chip_id);
    return false;
  }
  return true;
}

/* --------------------------------------------------------------------------
 * WFDMA Ring Setup & Polling
 * -------------------------------------------------------------------------- */
bool MtkDevice::dma_disable(bool force, std::string &error) {
  clear(MT_WFDMA0_GLO_CFG, MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN |
                               MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
                               MT_WFDMA0_GLO_CFG_OMIT_TX_INFO | MT_WFDMA0_GLO_CFG_OMIT_RX_INFO |
                               MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);
  if (!poll_reg(MT_WFDMA0_GLO_CFG,
                MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY, 0, 500)) {
    error = "WFDMA failed to go idle";
    return false;
  }

  /* Disable interrupts (we poll in user mode). */
  wr(MT_WFDMA0_HOST_INT_ENA, 0);
  wr(MT_PCIE_MAC_INT_ENABLE, 0);

  if (force) {
    clear(MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
    set(MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
  }
  _dma_running = false;
  _tx_pending_kick = false;
  return true;
}

bool MtkDevice::alloc_tx_ring(Ring &r, uint32_t reg_base, uint32_t count, bool with_txwi,
                              std::string &error) {
  r.reg_base = reg_base;
  r.count = count;
  r.head = 0;
  r.tail = 0;
  r.rx_reasm.clear();
  r.rx_reasm_drop = false;
  r.desc_buf = _dma.alloc(count * sizeof(MtkDesc), 256);
  if (!r.desc_buf) {
    error = "failed to allocate 32-bit DMA descriptors for TX ring";
    return false;
  }
  r.bufs.resize(count);
  if (with_txwi) r.txwi.resize(count);
  MtkDesc *d = r.descs();
  for (uint32_t i = 0; i < count; i++) {
    r.bufs[i] = _dma.alloc_page();
    if (!r.bufs[i]) {
      error = "failed to allocate 32-bit DMA TX payload page";
      return false;
    }
    if (with_txwi) {
      r.txwi[i] = _dma.alloc(MT_TXD_SIZE + sizeof(MtkHwTxp), 64);
      if (!r.txwi[i]) {
        error = "failed to allocate 32-bit DMA TXWI buffer";
        return false;
      }
    }
    d[i].buf0 = 0;
    d[i].ctrl = MT_DMA_CTL_DMA_DONE;
    d[i].buf1 = 0;
    d[i].info = 0;
  }
  program_ring(r, false);
  return true;
}

bool MtkDevice::alloc_rx_ring(Ring &r, uint32_t reg_base, uint32_t count, std::string &error) {
  r.reg_base = reg_base;
  r.count = count;
  r.head = 0;
  r.tail = 0;
  r.rx_reasm.clear();
  r.rx_reasm_drop = false;
  r.desc_buf = _dma.alloc(count * sizeof(MtkDesc), 256);
  if (!r.desc_buf) {
    error = "failed to allocate 32-bit DMA descriptors for RX ring";
    return false;
  }
  r.bufs.resize(count);
  MtkDesc *d = r.descs();
  for (uint32_t i = 0; i < count; i++) {
    r.bufs[i] = _dma.alloc_page();
    if (!r.bufs[i]) {
      error = "failed to allocate 32-bit DMA RX buffer page";
      return false;
    }
    d[i].buf0 = r.bufs[i].pa;
    d[i].ctrl = field_prep(MT_DMA_CTL_SD_LEN0, static_cast<uint32_t>(DmaPool32::kPageBytes));
    d[i].buf1 = 0;
    d[i].info = 0;
  }
  /* Arm all descriptors except one so cpu_idx = count - 1. */
  r.head = count - 1;
  program_ring(r, true);
  return true;
}

void MtkDevice::program_ring(const Ring &r, bool is_rx) {
  (void)is_rx;
  wr(r.reg_base + 0x0, r.desc_buf.pa);
  wr(r.reg_base + 0x4, r.count);
  wr(r.reg_base + 0x8, 0);
  wr(r.reg_base + 0xc, 0);
}

bool MtkDevice::dma_enable(std::string &error) {
  /* Prefetch settings for MT7921/MT7922 (mt792x_dma.c lines 265-279) */
  wr(MT_WFDMA0_RX_RING0_EXT_CTRL, prefetch_val(0x000, 0x4));
  wr(MT_WFDMA0_RX_RING2_EXT_CTRL, prefetch_val(0x040, 0x4));
  wr(MT_WFDMA0_RX_RING3_EXT_CTRL, prefetch_val(0x080, 0x4));
  wr(MT_WFDMA0_RX_RING4_EXT_CTRL, prefetch_val(0x0c0, 0x4));
  wr(MT_WFDMA0_RX_RING5_EXT_CTRL, prefetch_val(0x100, 0x4));

  wr(MT_WFDMA0_TX_RING0_EXT_CTRL, prefetch_val(0x140, 0x4));
  wr(MT_WFDMA0_TX_RING1_EXT_CTRL, prefetch_val(0x180, 0x4));
  wr(MT_WFDMA0_TX_RING2_EXT_CTRL, prefetch_val(0x1c0, 0x4));
  wr(MT_WFDMA0_TX_RING3_EXT_CTRL, prefetch_val(0x200, 0x4));
  wr(MT_WFDMA0_TX_RING4_EXT_CTRL, prefetch_val(0x240, 0x4));
  wr(MT_WFDMA0_TX_RING5_EXT_CTRL, prefetch_val(0x280, 0x4));
  wr(MT_WFDMA0_TX_RING6_EXT_CTRL, prefetch_val(0x2c0, 0x4));
  wr(MT_WFDMA0_TX_RING16_EXT_CTRL, prefetch_val(0x340, 0x4));
  wr(MT_WFDMA0_TX_RING17_EXT_CTRL, prefetch_val(0x380, 0x4));

  wr(MT_WFDMA0_RST_DTX_PTR, ~0u);
  wr(MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

  set(MT_WFDMA0_GLO_CFG,
      MT_WFDMA0_GLO_CFG_TX_WB_DDONE | MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
          MT_WFDMA0_GLO_CFG_CLK_GAT_DIS | MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
          field_prep(MT_WFDMA0_GLO_CFG_DMA_SIZE, 3) | MT_WFDMA0_GLO_CFG_FIFO_DIS_CHECK |
          MT_WFDMA0_GLO_CFG_RX_WB_DDONE | MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
          MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);

  set(MT_WFDMA0_GLO_CFG, MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);
  set(MT_WFDMA_DUMMY_CR, MT_WFDMA_NEED_REINIT);

  /* Kick RX rings after WFDMA pointer reset and global enable. */
  wr(_rx_mcu.reg_base + 0x8, _rx_mcu.count - 1);
  wr(_rx_mcu_wa.reg_base + 0x8, _rx_mcu_wa.count - 1);
  wr(_rx_data.reg_base + 0x8, _rx_data.count - 1);

  /* Enable WFDMA0 & PCIE MAC interrupt status masks (matches mt792x_dma_enable). */
  wr(MT_WFDMA0_HOST_INT_STA, ~0u);
  wr(MT_WFDMA0_HOST_INT_ENA, 0x2847fff5u);
  wr(MT_PCIE_MAC_INT_ENABLE, 0xffu);

  _dma_running = true;
  (void)error;
  return true;
}

bool MtkDevice::dma_init(std::string &error) {
  /* Need ~1065 4 KiB pages (< 5 MiB) below 4 GiB for 512-entry RX data + 256-entry TX + MCU rings. */
  if (!_dma.init(2048, 1250, error)) return false;
  log("32-bit DMA pool locked " + std::to_string(_dma.pages_locked()) + " pages (< 4 GiB)");

  /* Register crash-safe close-ops so RegProxy immediately disables interrupts,
   * stops WFDMA, holds WFDMA logic in reset, and holds WFSYS in reset if this
   * process crashes or is forcefully killed before close() runs (#72).
   * Normal close() clears these ops after completing a clean wfsys_reset(). */
  const uint32_t glo_off = translate_addr(MT_WFDMA0_GLO_CFG);
  const uint32_t int_off = translate_addr(MT_WFDMA0_HOST_INT_ENA);
  const uint32_t pcie_int_off = translate_addr(MT_PCIE_MAC_INT_ENABLE);
  const uint32_t rst_off = translate_addr(MT_WFDMA0_RST);
  const uint32_t l1_cur = _proxy.read32(MT_HIF_REMAP_L1);
  const uint32_t l1_wfsys =
      (l1_cur & ~MT_HIF_REMAP_L1_MASK) |
      field_prep(MT_HIF_REMAP_L1_MASK, (MT_WFSYS_SW_RST_B >> 16) & 0xffffu);
  const uint32_t wfsys_off =
      MT_HIF_REMAP_BASE_L1 + (MT_WFSYS_SW_RST_B & MT_HIF_REMAP_L1_OFFSET);
  std::vector<RegOp> close_ops = {
      {int_off, 0, 1},      /* write 0 to WFDMA0 HOST_INT_ENA */
      {pcie_int_off, 0, 1}, /* write 0 to PCIE_MAC_INT_ENABLE */
      {glo_off,
       MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN |
           MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN | MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
           MT_WFDMA0_GLO_CFG_OMIT_RX_INFO | MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2,
       3}, /* clear TX/RX DMA EN */
      {rst_off, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST,
       3}, /* hold WFDMA logic & DMASHDL in reset */
      {MT_HIF_REMAP_L1, l1_wfsys, 1}, /* point L1 remap window to MT_WFSYS_SW_RST_B */
      {wfsys_off, WFSYS_SW_RST_B, 3}, /* hold WFSYS in reset on crash */
  };
  _proxy.set_close_ops(close_ops);

  if (!dma_disable(true, error)) return false;

  if (!alloc_tx_ring(_tx_data, MT_TX_RING_BASE + MT7921_TXQ_BAND0 * MT_RING_STRIDE, 256, true,
                     error))
    return false;
  wr(MT_WFDMA0_TX_RING0_EXT_CTRL, 0x4);

  if (!alloc_tx_ring(_tx_mcu, MT_TX_RING_BASE + MT7921_TXQ_MCU_WM * MT_RING_STRIDE, 64, false,
                     error))
    return false;
  if (!alloc_tx_ring(_tx_fwdl, MT_TX_RING_BASE + MT7921_TXQ_FWDL * MT_RING_STRIDE, 64, false,
                     error))
    return false;

  if (!alloc_rx_ring(_rx_mcu, MT_RX_EVENT_RING_BASE, 16, error)) return false;
  if (!alloc_rx_ring(_rx_mcu_wa, MT_RX_MCU_WA_RING_BASE, 128, error)) return false;
  if (!alloc_rx_ring(_rx_data, MT_RX_DATA_RING_BASE, 512, error)) return false;

  _dma.trim();
  log("WFDMA rings allocated (" + std::to_string(_dma.pages_used()) + " pages used)");

  return dma_enable(error);
}

void MtkDevice::tx_ring_reclaim(Ring &r) {
  auto *d = r.descs();
  while (r.tail != r.head) {
    const uint32_t ctrl = *static_cast<const volatile uint32_t *>(&d[r.tail].ctrl);
    if (!(ctrl & MT_DMA_CTL_DMA_DONE)) break;
    r.tail = (r.tail + 1) % r.count;
  }
}

bool MtkDevice::tx_ring_push(Ring &r, const void *data, size_t len, bool wait_done) {
  if (len == 0 || len > DmaPool32::kPageBytes) return false;
  tx_ring_reclaim(r);
  const uint32_t next = (r.head + 1) % r.count;
  if (next == r.tail) return false;

  const uint32_t idx = r.head;
  std::memcpy(r.bufs[idx].va, data, len);

  MtkDesc *d = r.descs();
  d[idx].buf0 = r.bufs[idx].pa;
  d[idx].buf1 = 0;
  d[idx].info = 0;
  plat::mem_barrier();
  d[idx].ctrl = field_prep(MT_DMA_CTL_SD_LEN0, static_cast<uint32_t>(len)) | MT_DMA_CTL_LAST_SEC0;
  plat::mem_barrier();

  r.head = next;
  wr(r.reg_base + 0x8, r.head);

  if (wait_done) {
    const uint64_t deadline = now_ms() + 1000;
    while (!(*static_cast<const volatile uint32_t *>(&d[idx].ctrl) & MT_DMA_CTL_DMA_DONE)) {
      if (_stopping && _stopping()) return false;
      if (now_ms() >= deadline) {
        log("tx_ring_push timeout ring=" + hex32(r.reg_base) +
            " base=" + hex32(rr(r.reg_base + 0x0)) +
            " cnt=" + std::to_string(rr(r.reg_base + 0x4)) +
            " cidx=" + std::to_string(rr(r.reg_base + 0x8)) +
            " didx=" + std::to_string(rr(r.reg_base + 0xc)) +
            " ext17=" + hex32(rr(MT_WFDMA0_TX_RING17_EXT_CTRL)) +
            " ctrl=" + hex32(d[idx].ctrl) +
            " glo=" + hex32(rr(MT_WFDMA0_GLO_CFG)) +
            " ext0=" + hex32(rr(MT_WFDMA0_GLO_CFG_EXT0)) +
            " int_sta=" + hex32(rr(MT_WFDMA0_HOST_INT_STA)) +
            " dmashdl=" + hex32(rr(MT_DMASHDL_SW_CONTROL)) +
            " hif_misc=" + hex32(rr(MT_WFDMA_EXT_CSR_HIF_MISC)) +
            " pcie_sta=" + hex32(_proxy.read32(0x11004)) +
            " pcie_devsta=" + hex32(_proxy.read32(0x11088)));
        return false;
      }
      plat::sleep_ms(0);
    }
    tx_ring_reclaim(r);
  }
  return true;
}

bool MtkDevice::rx_ring_pop(Ring &r, std::vector<uint8_t> &out, bool commit) {
  static constexpr size_t kMaxReasmBytes = 12288;
  MtkDesc *d = r.descs();
  out.clear();
  for (;;) {
    const uint32_t idx = r.tail;
    const uint32_t ctrl = *static_cast<const volatile uint32_t *>(&d[idx].ctrl);
    if (!(ctrl & MT_DMA_CTL_DMA_DONE)) return false;

    plat::mem_barrier();
    const uint32_t len = field_get(MT_DMA_CTL_SD_LEN0, ctrl);
    const bool last_sec = (ctrl & MT_DMA_CTL_LAST_SEC0) != 0;

    if (r.rx_reasm_drop) {
      if (last_sec) r.rx_reasm_drop = false;
    } else if (len > 0 && len <= DmaPool32::kPageBytes) {
      if (r.rx_reasm.empty() && last_sec) {
        /* Fast path: single-descriptor frame (no extra copy). */
        out.assign(r.bufs[idx].va, r.bufs[idx].va + len);
      } else {
        /* Multi-descriptor RX A-MSDU fragment (up to 12 KiB). */
        if (r.rx_reasm.size() + len <= kMaxReasmBytes) {
          r.rx_reasm.insert(r.rx_reasm.end(), r.bufs[idx].va, r.bufs[idx].va + len);
          if (last_sec && !r.rx_reasm.empty()) {
            out.swap(r.rx_reasm);
            r.rx_reasm.clear();
          }
        } else {
          r.rx_reasm.clear();
          if (!last_sec) r.rx_reasm_drop = true;
        }
      }
    } else {
      r.rx_reasm.clear();
      if (!last_sec) r.rx_reasm_drop = true;
    }

    /* Re-arm descriptor and advance head/tail; optionally defer cpu_idx MMIO doorbell. */
    d[idx].buf0 = r.bufs[idx].pa;
    d[idx].buf1 = 0;
    d[idx].info = 0;
    plat::mem_barrier();
    d[idx].ctrl = field_prep(MT_DMA_CTL_SD_LEN0, static_cast<uint32_t>(DmaPool32::kPageBytes));
    plat::mem_barrier();

    r.head = idx;
    r.tail = (r.tail + 1) % r.count;
    if (commit) wr(r.reg_base + 0x8, r.head);

    if (last_sec) return !out.empty();
  }
}

void MtkDevice::rx_ring_commit(Ring &r) {
  wr(r.reg_base + 0x8, r.head);
}

/* --------------------------------------------------------------------------
 * Connac2 MCU Messaging
 * -------------------------------------------------------------------------- */
bool MtkDevice::mcu_wait_response(uint8_t seq, std::vector<uint8_t> &out, unsigned timeout_ms) {
  const uint64_t deadline = now_ms() + timeout_ms;
  std::vector<uint8_t> pkt;
  do {
    if (_stopping && _stopping()) return false;
    /* MCU responses arrive on RX Ring 0 before FW runs, and on RX Ring 4 after FW runs. */
    if (rx_ring_pop(_rx_mcu, pkt) || rx_ring_pop(_rx_mcu_wa, pkt)) {
      if (pkt.size() >= sizeof(MtkMcuRxd) - 4) {
        const auto *rxd = reinterpret_cast<const MtkMcuRxd *>(pkt.data());
        const uint32_t pkt_type = field_get(MT_RXD0_PKT_TYPE, rxd->rxd[0]);
        const uint32_t pkt_flag = field_get(MT_RXD0_PKT_FLAG, rxd->rxd[0]);
        /* PKT_TYPE_RX_EVENT with pkt_flag == 1 is an 802.11 frame (NORMAL_MCU), not an MCU reply. */
        if (pkt_type == PKT_TYPE_RX_EVENT && pkt_flag != 0x1 && rxd->seq == seq) {
          out = std::move(pkt);
          return true;
        }
      }
      /* The MCU/mgmt ring also carries 802.11 frames. In particular, a scan
       * can hear a beacon while waiting for the channel switch reply (#54).
       * Keep frames in arrival order for poll(); do not run callbacks inside
       * an MCU command. Bound memory and record the channel before it changes. */
      RxFrame frame;
      if (parse_rx_packet(pkt.data(), pkt.size(), frame)) {
        constexpr size_t kDeferredRxLimit = 64;
        if (_deferred_rx.size() == kDeferredRxLimit) _deferred_rx.pop_front();
        _deferred_rx.push_back({{frame.data, frame.data + frame.len}, frame.dbm, frame.channel});
      } else {
        handle_rx_meta(pkt.data(), pkt.size());
      }
      continue;
    }
    plat::sleep_ms(1);
  } while (now_ms() < deadline);
  return false;
}

bool MtkDevice::mcu_send(uint8_t cid, uint8_t ext_cid, uint8_t set_query,
                         const void *payload, size_t len, bool wait_rsp,
                         std::vector<uint8_t> *rsp, unsigned timeout_ms) {
  if (!_dma_running) return false;
  _mcu_seq = (_mcu_seq + 1) & 0x0fu;
  if (_mcu_seq == 0) _mcu_seq = 1;
  const uint8_t seq = _mcu_seq;

  std::vector<uint8_t> msg(sizeof(MtkMcuTxd), 0);
  if (len && payload) {
    const auto *p = static_cast<const uint8_t *>(payload);
    msg.insert(msg.end(), p, p + len);
  } else if (len) {
    msg.resize(sizeof(MtkMcuTxd) + len, 0);
  }

  auto *txd = reinterpret_cast<MtkMcuTxd *>(msg.data());
  txd->txd[0] = field_prep(MT_TXD0_TX_BYTES, static_cast<uint32_t>(msg.size())) |
                field_prep(MT_TXD0_PKT_FMT, MT_TX_TYPE_CMD) |
                field_prep(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_Q0);
  txd->txd[1] = MT_TXD1_LONG_FORMAT | field_prep(MT_TXD1_HDR_FORMAT, MT_HDR_FORMAT_CMD);

  txd->len = static_cast<uint16_t>(msg.size() - sizeof(txd->txd));
  txd->pq_id = mcu_pq_id(MT_TX_PORT_IDX_MCU, MT_TX_MCU_PORT_RX_Q0);
  txd->pkt_type = MCU_PKT_ID;
  txd->seq = seq;
  txd->cid = cid;
  txd->ext_cid = ext_cid;
  txd->set_query = set_query;
  txd->ext_cid_ack = ext_cid ? 1 : 0;
  txd->s2d_index = MCU_S2D_H2N;

  if (!tx_ring_push(_tx_mcu, msg.data(), msg.size(), true)) return false;
  if (!wait_rsp) return true;

  std::vector<uint8_t> tmp;
  if (!mcu_wait_response(seq, tmp, timeout_ms)) return false;
  if (rsp) *rsp = std::move(tmp);
  return true;
}

bool MtkDevice::mcu_send_uni(uint16_t cid, const void *payload, size_t len,
                             bool wait_rsp, unsigned timeout_ms) {
  if (!_dma_running) return false;
  _mcu_seq = (_mcu_seq + 1) & 0x0fu;
  if (_mcu_seq == 0) _mcu_seq = 1;
  const uint8_t seq = _mcu_seq;

  std::vector<uint8_t> msg(sizeof(MtkMcuUniTxd), 0);
  if (len && payload) {
    const auto *p = static_cast<const uint8_t *>(payload);
    msg.insert(msg.end(), p, p + len);
  } else if (len) {
    msg.resize(sizeof(MtkMcuUniTxd) + len, 0);
  }

  auto *txd = reinterpret_cast<MtkMcuUniTxd *>(msg.data());
  txd->txd[0] = field_prep(MT_TXD0_TX_BYTES, static_cast<uint32_t>(msg.size())) |
                field_prep(MT_TXD0_PKT_FMT, MT_TX_TYPE_CMD) |
                field_prep(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_Q0);
  txd->txd[1] = MT_TXD1_LONG_FORMAT | field_prep(MT_TXD1_HDR_FORMAT, MT_HDR_FORMAT_CMD);

  txd->len = static_cast<uint16_t>(msg.size() - sizeof(txd->txd));
  txd->cid = cid;
  txd->pkt_type = MCU_PKT_ID;
  txd->seq = seq;
  txd->s2d_index = MCU_S2D_H2N;
  txd->option = MCU_CMD_UNI_EXT_ACK;

  if (!tx_ring_push(_tx_mcu, msg.data(), msg.size(), true)) return false;
  if (!wait_rsp) return true;

  std::vector<uint8_t> rsp;
  return mcu_wait_response(seq, rsp, timeout_ms);
}

bool MtkDevice::mcu_send_scatter(const uint8_t *data, size_t len, std::string &error) {
  constexpr size_t kChunk = 4096;
  size_t off = 0;
  while (off < len) {
    const size_t n = std::min(kChunk, len - off);
    if (!tx_ring_push(_tx_fwdl, data + off, n, true)) {
      const uint32_t cidx = rr(_tx_fwdl.reg_base + 0x8);
      const uint32_t didx = rr(_tx_fwdl.reg_base + 0xc);
      const uint32_t glo = rr(MT_WFDMA0_GLO_CFG);
      const uint32_t misc = rr(MT_CONN_ON_MISC);
      error = "FWDL scatter TX failed at offset " + std::to_string(off) +
              " (cidx=" + std::to_string(cidx) + " didx=" + std::to_string(didx) +
              " glo=" + hex32(glo) + " misc=" + hex32(misc) + ")";
      return false;
    }
    off += n;
  }
  return true;
}

/* --------------------------------------------------------------------------
 * Firmware Download (Patch + RAM)
 * -------------------------------------------------------------------------- */
int MtkDevice::mcu_patch_sem_ctrl(bool get) {
  struct {
    uint32_t op;
  } req{get ? PATCH_SEM_GET : PATCH_SEM_RELEASE};
  std::vector<uint8_t> rsp;
  if (!mcu_send(MCU_CMD_PATCH_SEM_CONTROL, 0, MCU_Q_NA, &req, sizeof(req), true, &rsp, 3000))
    return -1;
  if (rsp.size() < sizeof(MtkMcuRxd) - 4 + 1) return -1;
  return rsp[sizeof(MtkMcuRxd) - 4];
}

bool MtkDevice::mcu_init_download(uint32_t addr, uint32_t len, uint32_t mode) {
  struct {
    uint32_t addr;
    uint32_t len;
    uint32_t mode;
  } req{addr, len, mode};
  const uint8_t cid = (addr == 0x200000u || addr == 0x900000u)
                          ? MCU_CMD_PATCH_START_REQ
                          : MCU_CMD_TARGET_ADDRESS_LEN_REQ;
  return mcu_send(cid, 0, MCU_Q_NA, &req, sizeof(req), true, nullptr, 3000);
}

bool MtkDevice::mcu_start_patch() {
  struct {
    uint8_t check_crc;
    uint8_t reserved[3];
  } req{0, {0, 0, 0}};
  std::vector<uint8_t> rsp;
  return mcu_send(MCU_CMD_PATCH_FINISH_REQ, 0, MCU_Q_NA, &req, sizeof(req), true, &rsp, 3000);
}

bool MtkDevice::mcu_start_firmware(uint32_t override_addr, uint32_t option) {
  struct {
    uint32_t option;
    uint32_t addr;
  } req{option, override_addr};
  return mcu_send(MCU_CMD_FW_START_REQ, 0, MCU_Q_NA, &req, sizeof(req), true, nullptr, 3000);
}

bool MtkDevice::load_patch(const std::string &path, std::string &error) {
  int sem = mcu_patch_sem_ctrl(true);
  if (sem == PATCH_IS_DL) {
    log("ROM patch already loaded");
    return true;
  }
  if (sem != PATCH_NOT_DL_SEM_SUCCESS) {
    error = "failed to acquire patch semaphore (status=" + std::to_string(sem) + ")";
    return false;
  }

  std::vector<uint8_t> fw;
  if (!read_file_bytes(path, fw, error)) {
    mcu_patch_sem_ctrl(false);
    return false;
  }
  if (fw.size() < sizeof(MtkPatchHdr)) {
    mcu_patch_sem_ctrl(false);
    error = "patch image too small";
    return false;
  }

  const auto *hdr = reinterpret_cast<const MtkPatchHdr *>(fw.data());
  const uint32_t n_region = bswap32(hdr->desc.n_region);
  log("loading ROM patch (" + std::to_string(n_region) + " regions, " +
      std::to_string(fw.size()) + " bytes)");

  bool ok = true;
  for (uint32_t i = 0; i < n_region; i++) {
    const auto *sec = reinterpret_cast<const MtkPatchSec *>(
        fw.data() + sizeof(MtkPatchHdr) + i * sizeof(MtkPatchSec));
    if ((bswap32(sec->type) & PATCH_SEC_TYPE_MASK) != PATCH_SEC_TYPE_INFO) {
      error = "unsupported patch section type";
      ok = false;
      break;
    }
    const uint32_t addr = bswap32(sec->addr);
    const uint32_t len = bswap32(sec->len);
    const uint32_t offs = bswap32(sec->offs);
    const uint32_t sec_info = bswap32(sec->sec_key_idx);
    if (offs + len > fw.size()) {
      error = "patch section out of bounds";
      ok = false;
      break;
    }

    uint32_t mode = DL_MODE_NEED_RSP;
    if (sec_info != PATCH_SEC_NOT_SUPPORT) {
      const uint32_t enc = field_get(PATCH_SEC_ENC_TYPE_MASK, sec_info);
      if (enc == PATCH_SEC_ENC_TYPE_AES) {
        mode |= DL_MODE_ENCRYPT |
                (field_prep(DL_MODE_KEY_IDX, sec_info & PATCH_SEC_ENC_AES_KEY_MASK) &
                 DL_MODE_KEY_IDX) |
                DL_MODE_RESET_SEC_IV;
      } else if (enc == PATCH_SEC_ENC_TYPE_SCRAMBLE) {
        mode |= DL_MODE_ENCRYPT | DL_CONFIG_ENCRY_MODE_SEL | DL_MODE_RESET_SEC_IV;
      }
    }

    if (!mcu_init_download(addr, len, mode)) {
      error = "patch init_download failed for region " + std::to_string(i);
      ok = false;
      break;
    }
    if (!mcu_send_scatter(fw.data() + offs, len, error)) {
      ok = false;
      break;
    }
  }

  if (ok && !mcu_start_patch()) {
    error = "patch start failed";
    ok = false;
  }

  const int rel = mcu_patch_sem_ctrl(false);
  if (rel != PATCH_REL_SEM_SUCCESS && ok) {
    error = "failed to release patch semaphore (status=" + std::to_string(rel) + ")";
    return false;
  }
  return ok;
}

bool MtkDevice::load_ram(const std::string &path, std::string &error) {
  std::vector<uint8_t> fw;
  if (!read_file_bytes(path, fw, error)) return false;
  if (fw.size() < sizeof(MtkFwTrailer)) {
    error = "RAM firmware too small";
    return false;
  }

  const auto *hdr = reinterpret_cast<const MtkFwTrailer *>(fw.data() + fw.size() - sizeof(MtkFwTrailer));
  const size_t regions_size = static_cast<size_t>(hdr->n_region) * sizeof(MtkFwRegion);
  if (fw.size() < sizeof(MtkFwTrailer) + regions_size) {
    error = "RAM firmware trailer invalid";
    return false;
  }

  log("loading RAM firmware (" + std::to_string(hdr->n_region) + " regions, " +
      std::to_string(fw.size()) + " bytes, ver=" + std::string(hdr->fw_ver, 10) + ")");

  size_t offset = 0;
  uint32_t override_addr = 0;
  uint32_t option = 0;

  for (uint8_t i = 0; i < hdr->n_region; i++) {
    const auto *reg = reinterpret_cast<const MtkFwRegion *>(
        reinterpret_cast<const uint8_t *>(hdr) - (hdr->n_region - i) * sizeof(MtkFwRegion));
    const uint32_t len = reg->len;
    const uint32_t addr = reg->addr;

    if (offset + len > fw.size()) {
      error = "RAM region out of bounds";
      return false;
    }
    if (!(reg->feature_set & FW_FEATURE_NON_DL)) {
      if (reg->feature_set & FW_FEATURE_OVERRIDE_ADDR) override_addr = addr;

      uint32_t mode = DL_MODE_NEED_RSP;
      if (reg->feature_set & FW_FEATURE_SET_ENCRYPT) {
        mode |= DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV |
                field_prep(DL_MODE_KEY_IDX, field_get(FW_FEATURE_SET_KEY_IDX, reg->feature_set));
      }
      if (reg->feature_set & FW_FEATURE_ENCRY_MODE) mode |= DL_CONFIG_ENCRY_MODE_SEL;

      if (!mcu_init_download(addr, len, mode)) {
        error = "RAM init_download failed for region " + std::to_string(i);
        return false;
      }
      if (!mcu_send_scatter(fw.data() + offset, len, error)) return false;
    }
    offset += len;
  }

  if (override_addr) option |= FW_START_OVERRIDE;
  if (!mcu_start_firmware(override_addr, option)) {
    error = "RAM start_firmware failed";
    return false;
  }
  return true;
}

bool MtkDevice::load_firmware(const std::string &fw_dir, std::string &error) {
  /* Matches mt7921e_mcu_init(): write MT_TOP_LPCR_HOST_DRV_OWN (2) after wfsys_reset + dma_init. */
  wr(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);
  plat::sleep_ms(20);
  wr(MT_SWDEF_MODE, MT_SWDEF_NORMAL_MODE);
  set(MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS);

  /* Restart download engine only if firmware N9 is still running (skip if already in ROM state). */
  const uint32_t misc = rr(MT_CONN_ON_MISC);
  if ((misc & MT_TOP_MISC2_FW_N9_ON) != 0) {
    struct {
      uint8_t power_mode;
      uint8_t rsv[3];
    } rst_req{1, {0, 0, 0}};
    (void)mcu_send(MCU_CMD_NIC_POWER_CTRL, 0, MCU_Q_NA, &rst_req, sizeof(rst_req), false);
    if (!poll_reg(MT_CONN_ON_MISC, MT_TOP_MISC_FW_STATE, MT_TOP_MISC2_FW_PWR_ON, 1000)) {
      log("note: MT_CONN_ON_MISC=" + hex32(rr(MT_CONN_ON_MISC)) + " before patch load");
    }
  } else {
    log("ROM ready (MT_CONN_ON_MISC=" + hex32(misc) + "), skipping NIC_POWER_CTRL");
  }

  std::string patch_file = "WIFI_MT7922_patch_mcu_1_1_hdr.bin";
  std::string ram_file = "WIFI_RAM_CODE_MT7922_1.bin";
  if (_chip_id == 0x7961) {
    patch_file = "WIFI_MT7961_patch_mcu_1_2_hdr.bin";
    ram_file = "WIFI_RAM_CODE_MT7961_1.bin";
  }

  if (!load_patch(plat::path_join(fw_dir, patch_file), error)) return false;
  if (!load_ram(plat::path_join(fw_dir, ram_file), error)) return false;

  if (!poll_reg(MT_CONN_ON_MISC, MT_TOP_MISC2_FW_N9_RDY, MT_TOP_MISC2_FW_N9_RDY, 1500)) {
    error = "timeout waiting for MT_TOP_MISC2_FW_N9_RDY (MISC=" + hex32(rr(MT_CONN_ON_MISC)) + ")";
    return false;
  }

  /* If LPCTL was latched at 0x4 before FW boot, ask the newly booted RAM firmware to clear it. */
  if ((rr(MT_CONN_ON_LPCTL) & PCIE_LPCR_HOST_OWN_SYNC) != 0) {
    wr(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);
    (void)poll_reg(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC, 0, 200);
    log("post-FW CLR_OWN: LPCTL=" + hex32(rr(MT_CONN_ON_LPCTL)) +
        " MISC=" + hex32(rr(MT_CONN_ON_MISC)));
  }

  _fw_loaded = true;
  const char *chip_name =
      (_chip_id == 0x7961) ? "MT7921" : (_chip_id == 0x7920 ? "MT7920" : "MT7922");
  log(std::string(chip_name) + " firmware booted and ready!");
  return true;
}

/* --------------------------------------------------------------------------
 * MAC Init, Monitor/AWDL Setup & Channel Switching
 * -------------------------------------------------------------------------- */
bool MtkDevice::mcu_set_eeprom() {
  struct {
    uint8_t buffer_mode;
    uint8_t format;
    uint16_t len;
  } req{0 /* EE_MODE_EFUSE */, 0 /* EE_FORMAT_WHOLE */, 0};
  return mcu_send(MCU_CMD_EXT_CID, MCU_EXT_CMD_EFUSE_BUFFER_MODE, MCU_Q_SET, &req, sizeof(req),
                  true);
}

bool MtkDevice::mac_init(std::string &error) {
  if (!mcu_set_eeprom()) {
    error = "MCU EFUSE_BUFFER_MODE failed";
    return false;
  }

  /* Enable Band 0 MAC engine (mt76_connac_mcu_set_mac_enable). */
  struct {
    uint8_t enable;
    uint8_t band;
    uint8_t rsv[2];
  } mac_req{1, 0, {0, 0}};
  if (!mcu_send(MCU_CMD_EXT_CID, MCU_EXT_CMD_MAC_INIT_CTRL, MCU_Q_SET, &mac_req, sizeof(mac_req),
                true)) {
    error = "MCU MAC_INIT_CTRL failed";
    return false;
  }

  /* Populate regulatory channel domain in firmware (mt76_connac_mcu_set_channel_domain). */
  {
    static constexpr uint8_t k2gChans[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    static constexpr uint8_t k5gChans[] = {36,  40,  44,  48,  52,  56,  60,  64,  100, 104, 108,
                                           112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153,
                                           157, 161, 165};
#pragma pack(push, 1)
    struct ChanDomainHdr {
      uint8_t alpha2[4];
      uint8_t bw_2g;
      uint8_t bw_5g;
      uint8_t bw_6g;
      uint8_t pad;
      uint8_t n_2ch;
      uint8_t n_5ch;
      uint8_t n_6ch;
      uint8_t pad2;
    };
    struct ChanEntry {
      uint16_t hw_value;
      uint16_t pad;
      uint32_t flags;
    };
#pragma pack(pop)
    std::vector<uint8_t> dom(sizeof(ChanDomainHdr) +
                             (sizeof(k2gChans) + sizeof(k5gChans)) * sizeof(ChanEntry),
                             0);
    auto *hdr = reinterpret_cast<ChanDomainHdr *>(dom.data());
    hdr->alpha2[0] = static_cast<uint8_t>(_country.size() >= 2 ? _country[0] : 'U');
    hdr->alpha2[1] = static_cast<uint8_t>(_country.size() >= 2 ? _country[1] : 'S');
    hdr->bw_2g = 0; /* BW_20_40M */
    hdr->bw_5g = (_chip_id == 0x7961) ? 2 : 3; /* MT7921: 80 MHz max; MT7922: 160 MHz */
    hdr->bw_6g = (_chip_id == 0x7961) ? 0 : 3;
    hdr->n_2ch = static_cast<uint8_t>(sizeof(k2gChans));
    hdr->n_5ch = static_cast<uint8_t>(sizeof(k5gChans));
    hdr->n_6ch = 0;
    auto *entries = reinterpret_cast<ChanEntry *>(dom.data() + sizeof(ChanDomainHdr));
    size_t idx = 0;
    for (uint8_t c : k2gChans) {
      entries[idx++] = ChanEntry{c, 0, 0};
    }
    for (uint8_t c : k5gChans) {
      entries[idx++] = ChanEntry{c, 0, 0};
    }
    (void)mcu_send(MCU_CE_CMD_SET_CHAN_DOMAIN, 0, MCU_Q_SET, dom.data(), dom.size(), false);
  }

  /* MAX_RX_LEN fields are in 8-byte units (1536 * 8 = 12,288 B, matching Linux mt7921);
   * host RX DMA buffers are 4 KiB pages (DmaPool32::kPageBytes). */
  rmw(MT_MDP_DCR1, MT_MDP_DCR1_MAX_RX_LEN, field_prep(MT_MDP_DCR1_MAX_RX_LEN, 1536));
  /* Explicitly disable hardware de-AMSDU and Ethernet header translation for raw 802.11 frames. */
  clear(MT_MDP_DCR0, MT_MDP_DCR0_DAMSDU_EN | MT_MDP_DCR0_RX_HDR_TRANS_EN);

  /* Band 0 WF_DMA RX config (mt792x_mac_init_band). */
  rmw(MT_DMA_DCR0_BAND0, MT_DMA_DCR0_MAX_RX_LEN, field_prep(MT_DMA_DCR0_MAX_RX_LEN, 1536));
  clear(MT_DMA_DCR0_BAND0, MT_DMA_DCR0_RXD_G5_EN);
  /* Enable MIB TX/RX airtime counters (0x820ed004 bits 8,9). */
  set(0x820ed004u, bit(8) | bit(9));

  clear(MT_ARB_SCR_BAND0, MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);

  /* Accept AWDL Action frames & Data frames; drop FCS errors and noisy control frames. */
  const uint32_t rfcr = MT_WF_RFCR_DROP_FCSFAIL | MT_WF_RFCR_DROP_VERSION |
                        MT_WF_RFCR_DROP_CTS | MT_WF_RFCR_DROP_RTS |
                        MT_WF_RFCR_DROP_CTL_RSV | MT_WF_RFCR_DROP_NDPA |
                        MT_WF_RFCR_DROP_UNWANTED_CTL;
  wr(MT_WF_RFCR_BAND0, rfcr);
  wr(MT_WF_RFCR1_BAND0, MT_WF_RFCR1_DROP_ACK | MT_WF_RFCR1_DROP_BF_POLL |
                            MT_WF_RFCR1_DROP_BA | MT_WF_RFCR1_DROP_CFEND |
                            MT_WF_RFCR1_DROP_CFACK);
  return true;
}

bool MtkDevice::hard_reset(std::string &error) {
  if (!probe(error)) return false;
  log("issuing CBTOP RGU whole-path Wi-Fi subsystem reset...");
  uint32_t v = l1_rr(MT_CBTOP_RGU_WF_SUBSYS_RST);
  l1_wr(MT_CBTOP_RGU_WF_SUBSYS_RST, v | MT_CBTOP_RGU_WF_SUBSYS_RST_WF_WHOLE_PATH);
  plat::sleep_ms(20);
  l1_wr(MT_CBTOP_RGU_WF_SUBSYS_RST, v & ~MT_CBTOP_RGU_WF_SUBSYS_RST_WF_WHOLE_PATH);
  plat::sleep_ms(50);
  if (!wfsys_reset(error)) return false;
  log("hard reset complete (LPCTL=" + hex32(rr(MT_CONN_ON_LPCTL)) +
      " MISC=" + hex32(rr(MT_CONN_ON_MISC)) + ").");
  return true;
}

bool MtkDevice::open(const Options &options, std::string &error) {
  _verbose = options.verbose;
  if (options.country.size() == 2 && options.country != "ZZ" && options.country != "00") {
    _country = options.country;
  } else {
    _country = "US";
  }
  if (!probe(error)) return false;
  /* Reset WF subsystem out of any previous OS driver firmware state into clean ROM boot. */
  if (!wfsys_reset(error)) return false;
  if (!driver_own(error)) return false;
  if (!dma_init(error)) return false;
  if (!load_firmware(options.firmware_dir, error)) return false;
  if (!mac_init(error)) return false;
  return true;
}

bool MtkDevice::set_address(const uint8_t address[6]) {
  if (std::memcmp(_mac.data(), address, 6) != 0) {
    _peers.fill(PeerEntry{});
    _pending_addba.fill(PendingAddba{});
    _pending_tx_addba.fill(PendingAddba{});
  }
  std::memcpy(_mac.data(), address, 6);

#pragma pack(push, 1)
  struct {
    uint8_t omac_idx;
    uint8_t band_idx;
    uint16_t pad;
    uint16_t tag;
    uint16_t len;
    uint8_t active;
    uint8_t link_idx;
    uint8_t omac_addr[6];
  } dev_req{};
#pragma pack(pop)
  dev_req.omac_idx = 0;
  dev_req.band_idx = 0;
  dev_req.tag = DEV_INFO_ACTIVE;
  dev_req.len = 12;
  dev_req.active = 1;
  dev_req.link_idx = 0;
  std::memcpy(dev_req.omac_addr, address, 6);

  if (!mcu_send_uni(MCU_UNI_CMD_DEV_INFO_UPDATE, &dev_req, sizeof(dev_req), true)) {
    log("WARNING: DEV_INFO_UPDATE failed");
    return false;
  }

  /* Register BSS 0 with AWDL BSSID 00:25:00:ff:94:73 so hardware ACKs unicast frames. */
#pragma pack(push, 1)
  struct {
    uint8_t bss_idx;
    uint8_t pad[3];
    uint16_t tag;
    uint16_t len;
    uint8_t active;
    uint8_t omac_idx;
    uint8_t hw_bss_idx;
    uint8_t band_idx;
    uint32_t conn_type;
    uint8_t conn_state;
    uint8_t wmm_idx;
    uint8_t bssid[6];
    uint16_t bmc_tx_wlan_idx;
    uint16_t bcn_interval;
    uint8_t dtim_period;
    uint8_t phymode;
    uint16_t sta_idx;
    uint16_t nonht_basic_phy;
    uint8_t phymode_ext;
    uint8_t link_idx;
  } bss_req{};
#pragma pack(pop)
  bss_req.bss_idx = 0;
  bss_req.tag = UNI_BSS_INFO_BASIC;
  bss_req.len = sizeof(bss_req) - 4;
  bss_req.active = 1;
  bss_req.omac_idx = 0;
  bss_req.hw_bss_idx = 0;
  bss_req.band_idx = 0;
  bss_req.conn_type = CONNECTION_IBSS_ADHOC;
  bss_req.conn_state = CONN_STATE_CONNECT;
  bss_req.wmm_idx = 0;
  static constexpr uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
  std::memcpy(bss_req.bssid, kAwdlBssid, 6);
  bss_req.bmc_tx_wlan_idx = 0;
  bss_req.bcn_interval = 100;
  bss_req.dtim_period = 1;
  bss_req.phymode = 0xb1; /* 802.11a + HT5 + VHT + HE5 (Connac2 5 GHz HE80) */
  bss_req.sta_idx = 0;
  bss_req.nonht_basic_phy = 0x78; /* OFDM | HT | VHT | HE */

  if (!mcu_send_uni(MCU_UNI_CMD_BSS_INFO_UPDATE, &bss_req, sizeof(bss_req), true)) {
    log("WARNING: BSS_INFO_UPDATE failed");
    return false;
  }

  /* Register WTBL / STA record 0 (broadcast/mgmt global_wcid) so LMAC transmits frames. */
#pragma pack(push, 1)
  struct {
    uint8_t bss_idx;
    uint8_t wlan_idx_lo;
    uint16_t tlv_num;
    uint8_t is_tlv_append;
    uint8_t muar_idx;
    uint8_t wlan_idx_hi;
    uint8_t rsv;
    /* STA_REC_BASIC TLV (20 bytes) */
    uint16_t tag;
    uint16_t len;
    uint32_t conn_type;
    uint8_t conn_state;
    uint8_t qos;
    uint16_t aid;
    uint8_t peer_addr[6];
    uint16_t extra_info;
  } sta_req{};
#pragma pack(pop)
  sta_req.bss_idx = 0;
  sta_req.wlan_idx_lo = 0;
  sta_req.tlv_num = 1;
  sta_req.is_tlv_append = 1;
  sta_req.muar_idx = 0;
  sta_req.tag = STA_REC_BASIC;
  sta_req.len = 20;
  sta_req.conn_type = CONNECTION_INFRA_BC;
  sta_req.conn_state = CONN_STATE_PORT_SECURE;
  sta_req.qos = 1;
  std::memset(sta_req.peer_addr, 0xff, 6);
  sta_req.extra_info = EXTRA_INFO_VER | EXTRA_INFO_NEW;
  if (!mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, &sta_req, sizeof(sta_req), true)) {
    log("WARNING: STA_REC_UPDATE failed");
  }

  /* Configure EDCA WMM parameters for BSS 0 (matches mt7921_mcu_set_tx). */
#pragma pack(push, 1)
  struct EdcaAc {
    uint16_t cw_min;
    uint16_t cw_max;
    uint16_t txop;
    uint16_t aifs;
    uint8_t guardtime;
    uint8_t acm;
  };
  struct {
    EdcaAc edca[4];
    uint8_t bss_idx;
    uint8_t qos;
    uint8_t wmm_idx;
    uint8_t pad;
  } edca_req{};
#pragma pack(pop)
  for (int ac = 0; ac < 4; ac++) {
    edca_req.edca[ac].cw_min = 3;
    edca_req.edca[ac].cw_max = 7;
    edca_req.edca[ac].txop = 94; /* ~3 ms (94 * 32 us) for back-to-back A-MPDU bursts */
    edca_req.edca[ac].aifs = 2;
  }
  edca_req.bss_idx = 0;
  edca_req.qos = 1;
  edca_req.wmm_idx = 0;
  (void)mcu_send(MCU_CE_CMD_SET_EDCA_PARMS, 0, MCU_Q_SET, &edca_req, sizeof(edca_req), false);
  return true;
}

bool MtkDevice::add_omac(uint8_t omac, const uint8_t addr[6], const uint8_t bssid[6], bool enable) {
#pragma pack(push, 1)
  struct {
    uint8_t omac_idx;
    uint8_t band_idx;
    uint16_t pad;
    uint16_t tag;
    uint16_t len;
    uint8_t active;
    uint8_t link_idx;
    uint8_t omac_addr[6];
  } dev_req{};
  struct {
    uint8_t bss_idx;
    uint8_t pad[3];
    uint16_t tag;
    uint16_t len;
    uint8_t active;
    uint8_t omac_idx;
    uint8_t hw_bss_idx;
    uint8_t band_idx;
    uint32_t conn_type;
    uint8_t conn_state;
    uint8_t wmm_idx;
    uint8_t bssid[6];
    uint16_t bmc_tx_wlan_idx;
    uint16_t bcn_interval;
    uint8_t dtim_period;
    uint8_t phymode;
    uint16_t sta_idx;
    uint16_t nonht_basic_phy;
    uint8_t phymode_ext;
    uint8_t link_idx;
  } bss_req{};
#pragma pack(pop)
  dev_req.omac_idx = omac;
  dev_req.band_idx = 0;
  dev_req.tag = DEV_INFO_ACTIVE;
  dev_req.len = 12;
  dev_req.active = enable ? 1u : 0u;
  dev_req.link_idx = 0;
  std::memcpy(dev_req.omac_addr, addr, 6);

  bss_req.bss_idx = omac;
  bss_req.tag = UNI_BSS_INFO_BASIC;
  bss_req.len = sizeof(bss_req) - 4;
  bss_req.active = enable ? 1u : 0u;
  bss_req.omac_idx = omac;
  bss_req.hw_bss_idx = omac;
  bss_req.band_idx = 0;
  bss_req.conn_type = CONNECTION_INFRA_AP; /* as mt76 for a monitor-like interface */
  bss_req.conn_state = CONN_STATE_CONNECT;
  bss_req.wmm_idx = 0;
  std::memcpy(bss_req.bssid, bssid, 6);
  bss_req.bmc_tx_wlan_idx = 0;
  bss_req.bcn_interval = 100;
  bss_req.dtim_period = 1;
  bss_req.phymode = 0x3f; /* A, B, G, GN, AN, AC */
  bss_req.sta_idx = 0;
  bss_req.nonht_basic_phy = 0x02; /* OFDM */

  bool ok = false;
  if (enable) {
    ok = mcu_send_uni(MCU_UNI_CMD_DEV_INFO_UPDATE, &dev_req, sizeof(dev_req), true) &&
         mcu_send_uni(MCU_UNI_CMD_BSS_INFO_UPDATE, &bss_req, sizeof(bss_req), true);
  } else {
    ok = mcu_send_uni(MCU_UNI_CMD_BSS_INFO_UPDATE, &bss_req, sizeof(bss_req), true) &&
         mcu_send_uni(MCU_UNI_CMD_DEV_INFO_UPDATE, &dev_req, sizeof(dev_req), true);
  }
  if (ok && enable) {
#pragma pack(push, 1)
    struct EdcaAc {
      uint16_t cw_min;
      uint16_t cw_max;
      uint16_t txop;
      uint16_t aifs;
      uint8_t guardtime;
      uint8_t acm;
    };
    struct {
      EdcaAc edca[4];
      uint8_t bss_idx;
      uint8_t qos;
      uint8_t wmm_idx;
      uint8_t pad;
    } edca_req{};
#pragma pack(pop)
    for (int ac = 0; ac < 4; ac++) {
      edca_req.edca[ac].cw_min = 3;
      edca_req.edca[ac].cw_max = 7;
      edca_req.edca[ac].txop = 94;
      edca_req.edca[ac].aifs = 2;
    }
    edca_req.bss_idx = omac;
    edca_req.qos = 1;
    edca_req.wmm_idx = 0;
    (void)mcu_send(MCU_CE_CMD_SET_EDCA_PARMS, 0, MCU_Q_SET, &edca_req, sizeof(edca_req), false);
  }
  return ok;
}

bool MtkDevice::set_station_identity(const uint8_t addr[6], const uint8_t bssid[6]) {
  if (_sta_iface_up) {
    (void)add_omac(kStationOmac, _sta_addr.data(), _sta_bssid.data(), false);
  }
  std::memcpy(_sta_addr.data(), addr, 6);
  std::memcpy(_sta_bssid.data(), bssid, 6);
  _sta_iface_up = add_omac(kStationOmac, _sta_addr.data(), _sta_bssid.data(), true);
  restore_rx_filter();
  return _sta_iface_up;
}

void MtkDevice::clear_station_identity() {
  if (!_sta_iface_up) return;
  (void)add_omac(kStationOmac, _sta_addr.data(), _sta_bssid.data(), false);
  _sta_iface_up = false;
  restore_rx_filter();
}

void MtkDevice::set_timing() { /* mt792x_mac_set_timeing, coverage class 0, short slot */
  const bool is_2ghz = _channel <= 14;
  const uint32_t cck = 231u | (48u << 16);
  const uint32_t ofdm = 60u | (28u << 16);
  const uint32_t sifs = is_2ghz ? 10u : 16u;
  const uint32_t slot = 9u;
  set(MT_ARB_SCR_BAND0, MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);
  wr(MT_TMAC_CDTR_BAND0, cck);
  wr(MT_TMAC_ODTR_BAND0, ofdm);
  wr(MT_TMAC_ICR0_BAND0, 360u | (2u << 10) | (sifs << 16) | (slot << 24));
  rmw(MT_AGG_ACR0_BAND0, 0x3fffu, 0x49u);
  clear(MT_ARB_SCR_BAND0, MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);
}

void MtkDevice::apply_rx_filter_mcu() {
  /* Configure firmware RX filter (matching Linux mt7921_configure_filter and
   * mt7921_mcu_set_beacon_filter(false)):
   * 1) mode=1 enables OTHER_BSS reception in firmware;
   * 2) mode=2 with MT7921_FIF_BIT_CLR clears DROP_OTHER_BEACON / DROP_OTHER_BSS /
   *    DROP_OTHER_UC / BSSID-drop bits in firmware's cached RFCR so channel switches
   *    during Wi-Fi scans do not re-enable DROP_OTHER_BEACON (#54, #66, #72).
   * Note: Linux mt7921_mcu_set_rxfilter passes wait_resp=false; firmware sends no ACK. */
#pragma pack(push, 1)
  struct {
    uint8_t rsv[4];
    uint8_t mode;
    uint8_t rsv2[3];
    uint32_t fif;
    uint32_t bit_map;
    uint8_t bit_op;
    uint8_t pad[51];
  } rxfilter_req{};
#pragma pack(pop)
  rxfilter_req.mode = 1;
  rxfilter_req.fif = MT7921_FILTER_ENABLE | MT7921_FILTER_OTHER_BSS;
  (void)mcu_send(MCU_CE_CMD_SET_RX_FILTER, 0, MCU_Q_SET, &rxfilter_req, sizeof(rxfilter_req),
                 false);

  rxfilter_req = {};
  rxfilter_req.mode = 2;
  rxfilter_req.fif = 0;
  rxfilter_req.bit_op = MT7921_FIF_BIT_CLR;
  rxfilter_req.bit_map = MT_WF_RFCR_DROP_OTHER_BEACON | MT_WF_RFCR_DROP_OTHER_BSS |
                         MT_WF_RFCR_DROP_OTHER_UC | MT_WF_RFCR_DROP_A2_BSSID |
                         MT_WF_RFCR_DROP_A3_BSSID | MT_WF_RFCR_DROP_A3_MAC |
                         MT_WF_RFCR_DROP_BCAST | MT_WF_RFCR_DROP_MCAST;
  (void)mcu_send(MCU_CE_CMD_SET_RX_FILTER, 0, MCU_Q_SET, &rxfilter_req, sizeof(rxfilter_req),
                 false);
}

void MtkDevice::restore_rx_filter() {
  const uint32_t rfcr = MT_WF_RFCR_DROP_FCSFAIL | MT_WF_RFCR_DROP_VERSION |
                        MT_WF_RFCR_DROP_CTS | MT_WF_RFCR_DROP_RTS |
                        MT_WF_RFCR_DROP_CTL_RSV | MT_WF_RFCR_DROP_NDPA |
                        MT_WF_RFCR_DROP_UNWANTED_CTL;
  wr(MT_WF_RFCR_BAND0, rfcr);
  /* Ensure an aborted/timed-out channel switch never leaves Band 0 RX disabled (#72). */
  clear(MT_ARB_SCR_BAND0, MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);
}

bool MtkDevice::mcu_set_chan_cmd(uint8_t ext_cid, uint8_t ch, unsigned width_mhz,
                                 uint8_t switch_reason) {
#pragma pack(push, 1)
  struct {
    uint8_t control_ch;
    uint8_t center_ch;
    uint8_t bw;
    uint8_t tx_streams_num;
    uint8_t rx_streams;
    uint8_t switch_reason;
    uint8_t band_idx;
    uint8_t center_ch2;
    uint16_t cac_case;
    uint8_t channel_band;
    uint8_t rsv0;
    uint32_t outband_freq;
    uint8_t txpower_drop;
    uint8_t ap_bw;
    uint8_t ap_center_ch;
    uint8_t rsv1[57];
  } req{};
#pragma pack(pop)
  static_assert(sizeof(req) == 76, "chan_info req must be 76 bytes");

  req.control_ch = ch;
  req.center_ch = center_channel_for(ch, width_mhz);
  req.bw = (width_mhz == 80) ? CMD_CBW_80MHZ : (width_mhz == 40 ? CMD_CBW_40MHZ : CMD_CBW_20MHZ);
  req.tx_streams_num = 2; /* MT7922 is 2x2 */
  req.rx_streams = (ext_cid == MCU_EXT_CMD_CHANNEL_SWITCH) ? 2 : 0x3; /* num vs antenna mask */
  req.switch_reason = switch_reason;
  req.band_idx = 0;
  req.channel_band = (ch <= 14) ? 0 : 1; /* 0 = 2.4 GHz, 1 = 5 GHz */

  const unsigned timeout =
      (ext_cid == MCU_EXT_CMD_CHANNEL_SWITCH && _rx_path_set) ? move_timeout_ms : 3000u;
  return mcu_send(MCU_CMD_EXT_CID, ext_cid, MCU_Q_SET, &req, sizeof(req), true, nullptr, timeout);
}

bool MtkDevice::set_channel(uint8_t channel, unsigned width_mhz, uint8_t switch_reason) {
  if (channel <= 14) width_mhz = 20;
  const bool band_changed = !_rx_path_set || ((channel <= 14) != (_channel <= 14));
  if (band_changed) {
    if (!mcu_set_chan_cmd(MCU_EXT_CMD_SET_RX_PATH, channel, width_mhz, switch_reason)) return false;
  }
  if (!mcu_set_chan_cmd(MCU_EXT_CMD_CHANNEL_SWITCH, channel, width_mhz, switch_reason)) {
    apply_rx_filter_mcu();
    restore_rx_filter();
    return false;
  }

  if (!_rx_path_set) {
    apply_rx_filter_mcu();
    _rx_path_set = true;
  }
  /* Also clear DROP_OTHER_BEACON / DROP_OTHER_UC / DROP_OTHER_BSS directly in RFCR. */
  restore_rx_filter();

  _channel = channel;
  _width_mhz = width_mhz;
  set_timing();
  if (_verbose) {
    log("tuned to ch " + std::to_string(channel) + " (" + std::to_string(width_mhz) +
        " MHz) RFCR=" + hex32(rr(MT_WF_RFCR_BAND0)) +
        " DCR0=" + hex32(rr(MT_DMA_DCR0_BAND0)) +
        " ARB=" + hex32(rr(MT_ARB_SCR_BAND0)));
  }
  return true;
}

bool MtkDevice::start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]) {
  if (!set_address(address)) return false;
  return set_channel(channel, width_mhz);
}

/* --------------------------------------------------------------------------
 * Hardware RX Block Ack & Peer WTBL Management (Connac2 MCU_UNI_CMD_STA_REC_UPDATE)
 * -------------------------------------------------------------------------- */
int MtkDevice::find_peer_wcid(const uint8_t mac[6]) {
  for (uint8_t wcid = 1; wcid < kApWcid; wcid++) {
    if (_peers[wcid].in_use && std::memcmp(_peers[wcid].mac.data(), mac, 6) == 0) {
      _peers[wcid].last_used_ms = now_ms();
      return wcid;
    }
  }
  return -1;
}

bool MtkDevice::mcu_register_peer(uint8_t wcid, const uint8_t mac[6]) {
  if (wcid == 0 || wcid >= kApWcid) return false;

  /* Clear WTBL admission counter before programming/reusing wcid. */
  rmw(MT_WTBL_UPDATE, MT_WTBL_UPDATE_WLAN_IDX,
      field_prep(MT_WTBL_UPDATE_WLAN_IDX, wcid) | MT_WTBL_UPDATE_ADM_COUNT_CLEAR);
  (void)poll_reg(MT_WTBL_UPDATE, MT_WTBL_UPDATE_BUSY, 0, 100);

#pragma pack(push, 1)
  struct PeerStaRecMsg {
    /* sta_req_hdr (8 B) */
    uint8_t bss_idx;
    uint8_t wlan_idx_lo;
    uint16_t tlv_num;
    uint8_t is_tlv_append;
    uint8_t muar_idx;
    uint8_t wlan_idx_hi;
    uint8_t rsv0;

    /* sta_rec_basic (20 B) */
    uint16_t basic_tag;
    uint16_t basic_len;
    uint32_t conn_type;
    uint8_t conn_state;
    uint8_t qos;
    uint16_t aid;
    uint8_t peer_addr[6];
    uint16_t extra_info;

    /* sta_rec_ht (8 B) */
    uint16_t ht_tag;
    uint16_t ht_len;
    uint16_t ht_cap;
    uint16_t ht_rsv;

    /* sta_rec_vht (16 B, Connac2) */
    uint16_t vht_tag;
    uint16_t vht_len;
    uint32_t vht_cap;
    uint16_t vht_rx_mcs_map;
    uint16_t vht_tx_mcs_map;
    uint8_t rts_bw_sig;
    uint8_t vht_rsv[3];

    /* STA_REC_WTBL outer TLV header (4 B) */
    uint16_t wtbl_tlv_tag;
    uint16_t wtbl_tlv_len;

    /* wtbl_req_hdr (8 B) */
    uint8_t wtbl_wlan_idx_lo;
    uint8_t wtbl_operation;
    uint16_t wtbl_tlv_num;
    uint8_t wtbl_wlan_idx_hi;
    uint8_t wtbl_rsv[3];

    /* wtbl_generic (20 B) */
    uint16_t gen_tag;
    uint16_t gen_len;
    uint8_t gen_peer_addr[6];
    uint8_t gen_muar_idx;
    uint8_t gen_skip_tx;
    uint8_t gen_cf_ack;
    uint8_t gen_qos;
    uint8_t gen_mesh;
    uint8_t gen_adm;
    uint16_t gen_partial_aid;
    uint8_t gen_baf_en;
    uint8_t gen_aad_om;

    /* wtbl_rx (12 B) */
    uint16_t rx_tag;
    uint16_t rx_len;
    uint8_t rx_rcid;
    uint8_t rx_rca1;
    uint8_t rx_rca2;
    uint8_t rx_rv;
    uint8_t rx_rsv[4];

    /* wtbl_hdr_trans (8 B) */
    uint16_t htr_tag;
    uint16_t htr_len;
    uint8_t htr_to_ds;
    uint8_t htr_from_ds;
    uint8_t htr_no_rx_trans;
    uint8_t htr_rsv;

    /* wtbl_ht (12 B) */
    uint16_t wht_tag;
    uint16_t wht_len;
    uint8_t wht_ht;
    uint8_t wht_ldpc;
    uint8_t wht_af;
    uint8_t wht_mm;
    uint8_t wht_rsv[4];

    /* wtbl_vht (12 B) */
    uint16_t wvht_tag;
    uint16_t wvht_len;
    uint8_t wvht_ldpc;
    uint8_t wvht_dyn_bw;
    uint8_t wvht_vht;
    uint8_t wvht_txop_ps;
    uint8_t wvht_rsv[4];
  };
#pragma pack(pop)
  static_assert(sizeof(PeerStaRecMsg) == 128, "PeerStaRecMsg must be 128 bytes");

  PeerStaRecMsg req{};
  req.bss_idx = 0;
  req.wlan_idx_lo = wcid;
  req.tlv_num = 4;
  req.is_tlv_append = 1;
  req.muar_idx = 0;

  req.basic_tag = STA_REC_BASIC;
  req.basic_len = 20;
  req.conn_type = CONNECTION_IBSS_ADHOC;
  req.conn_state = CONN_STATE_PORT_SECURE;
  req.qos = 1;
  req.aid = wcid;
  std::memcpy(req.peer_addr, mac, 6);
  req.extra_info = EXTRA_INFO_VER | EXTRA_INFO_NEW;

  req.ht_tag = STA_REC_HT;
  req.ht_len = 8;
  req.ht_cap = 0x016fu; /* HT RX LDPC, 40 MHz, SMPS disabled, SGI20/40, RX STBC 1, Max AMSDU 3839B */

  req.vht_tag = STA_REC_VHT;
  req.vht_len = 16;
  req.vht_cap = 0x03800130u; /* Max MPDU 3895B, RX LDPC, SGI80, RX STBC 1, Max AMPDU Exp 7 (1 MiB) */
  req.vht_rx_mcs_map = 0xfffau; /* 2SS MCS 0-9 */
  req.vht_tx_mcs_map = 0xfffau; /* 2SS MCS 0-9 */

  req.wtbl_tlv_tag = STA_REC_WTBL;
  req.wtbl_tlv_len = 76; /* 4 + 8 + 20 + 12 + 8 + 12 + 12 */

  req.wtbl_wlan_idx_lo = wcid;
  req.wtbl_operation = WTBL_RESET_AND_SET;
  req.wtbl_tlv_num = 5;

  req.gen_tag = WTBL_GENERIC;
  req.gen_len = 20;
  std::memcpy(req.gen_peer_addr, mac, 6);
  req.gen_qos = 1;
  req.gen_partial_aid = wcid;

  req.rx_tag = WTBL_RX;
  req.rx_len = 12;
  req.rx_rca1 = 1;
  req.rx_rca2 = 1;
  req.rx_rv = 1;

  req.htr_tag = WTBL_HDR_TRANS;
  req.htr_len = 8;
  req.htr_no_rx_trans = 1;

  req.wht_tag = WTBL_HT;
  req.wht_len = 12;
  req.wht_ht = 1;
  req.wht_ldpc = 1;
  req.wht_af = 7; /* max(ht_af=3, vht_af=7) for 1 MiB VHT A-MPDU */

  req.wvht_tag = WTBL_VHT;
  req.wvht_len = 12;
  req.wvht_ldpc = 1;
  req.wvht_vht = 1;

  if (!mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, &req, sizeof(req), true)) {
    log("WARNING: peer STA_REC_UPDATE failed for wcid=" + std::to_string(wcid) + " peer=" +
        mac_str(mac));
    return false;
  }

  /* Follow-up STA_REC_UPDATE: program Connac2 PHY + HE + HE_V2 + RA + STATE
   * (80 MHz 2SS MCS 0-11, LDPC, A-MSDU in A-MPDU) so firmware RA can ramp to HE80 2SS. */
  {
    constexpr size_t kPhyBytes = 12, kHeBytes = 28, kHeV2Bytes = 28, kRaBytes = 16, kStateBytes = 12;
    uint8_t ext[8 + kPhyBytes + kHeBytes + kHeV2Bytes + kRaBytes + kStateBytes]{};
    ext[0] = 0; /* bss_idx */
    ext[1] = wcid;
    ext[2] = 5; /* tlv_num */
    ext[4] = 1; /* is_tlv_append */
    ext[5] = 0; /* muar_idx */

    auto put16 = [](uint8_t *p, uint16_t v) {
      p[0] = static_cast<uint8_t>(v & 0xffu);
      p[1] = static_cast<uint8_t>(v >> 8);
    };
    auto put32 = [](uint8_t *p, uint32_t v) {
      p[0] = static_cast<uint8_t>(v & 0xffu);
      p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
      p[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
      p[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
    };

    uint8_t *phy = ext + 8;
    put16(phy, 0x15); /* STA_REC_PHY */
    put16(phy + 2, kPhyBytes);
    put16(phy + 4, 0x0540);
    phy[6] = (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6); /* OFDM | HT | VHT | HE */
    phy[7] = 0x1du; /* HT AMPDU: 16 KiB, 16 us fallback */

    uint8_t *he = phy + kPhyBytes;
    put16(he, 0x0e); /* STA_REC_HE */
    put16(he + 2, kHeBytes);
    put32(he + 4, (1u << 6) | (1u << 4)); /* LDPC | A-MSDU in A-MPDU */
    he[10] = 2; /* 80 MHz directly */
    he[18] = 2; /* 16 us nominal packet padding */
    put16(he + 20, 0xfffau); /* 2SS MCS 0-11 */

    uint8_t *he_v2 = he + kHeBytes;
    put16(he_v2, 0x19); /* STA_REC_HE_V2 */
    put16(he_v2 + 2, kHeV2Bytes);
    he_v2[8] = 0x40; /* A-MSDU in A-MPDU */
    he_v2[10] = 0x04; /* 5 GHz 40/80 MHz */
    he_v2[11] = 0x20; /* LDPC */
    he_v2[21] = 2;
    put16(he_v2 + 22, 0xfffau); /* 2SS MCS 0-11 */

    uint8_t *ra = he_v2 + kHeV2Bytes;
    put16(ra, 0x01); /* STA_REC_RA */
    put16(ra + 2, kRaBytes);
    put16(ra + 4, 0x3fc0); /* 5 GHz OFDM legacy rates */
    ra[6] = 0xff; /* HT MCS 0-7 */
    ra[7] = 0xff; /* HT MCS 8-15 (2SS) */

    uint8_t *st = ra + kRaBytes;
    put16(st, 0x07); /* STA_REC_STATE */
    put16(st + 2, kStateBytes);
    st[8] = 2; /* Associated */
    st[9] = 2 | (1u << 4); /* 80 MHz | 2SS */

    if (!mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, ext, sizeof(ext), true)) {
      log("WARNING: peer HE/RA STA_REC_UPDATE failed for wcid=" + std::to_string(wcid));
    }
  }
  return true;
}

int MtkDevice::get_or_add_peer_wcid(const uint8_t mac[6]) {
  const int existing = find_peer_wcid(mac);
  if (existing > 0) return existing;

  uint8_t chosen = 0;
  uint64_t oldest_ts = ~0ULL;
  for (uint8_t wcid = 1; wcid < kApWcid; wcid++) {
    if (!_peers[wcid].in_use) {
      chosen = wcid;
      break;
    }
    if (_peers[wcid].last_used_ms < oldest_ts) {
      oldest_ts = _peers[wcid].last_used_ms;
      chosen = wcid;
    }
  }
  if (chosen == 0 || chosen >= kApWcid) return -1;

  if (!mcu_register_peer(chosen, mac)) return -1;

  _peers[chosen].in_use = true;
  std::memcpy(_peers[chosen].mac.data(), mac, 6);
  _peers[chosen].tx_ba_active.fill(false);
  _peers[chosen].last_used_ms = now_ms();
  log("registered AWDL peer wcid=" + std::to_string(chosen) + " mac=" + mac_str(mac));
  return chosen;
}

bool MtkDevice::mcu_set_rx_ba(uint8_t wcid, const uint8_t mac[6], uint8_t tid, uint16_t ssn,
                              uint16_t winsize, bool amsdu, bool enable) {
  if (wcid == 0 || wcid >= MT792X_WTBL_RESERVED || tid >= 8) return false;

#pragma pack(push, 1)
  struct WtblBaMsg {
    /* sta_req_hdr (8 B) */
    uint8_t bss_idx;
    uint8_t wlan_idx_lo;
    uint16_t tlv_num;
    uint8_t is_tlv_append;
    uint8_t muar_idx;
    uint8_t wlan_idx_hi;
    uint8_t rsv0;

    /* STA_REC_WTBL outer TLV header (4 B) */
    uint16_t wtbl_tlv_tag;
    uint16_t wtbl_tlv_len;

    /* wtbl_req_hdr (8 B) */
    uint8_t wtbl_wlan_idx_lo;
    uint8_t wtbl_operation;
    uint16_t wtbl_tlv_num;
    uint8_t wtbl_wlan_idx_hi;
    uint8_t wtbl_rsv[3];

    /* wtbl_ba (28 B) */
    uint16_t ba_tag;
    uint16_t ba_len;
    uint8_t ba_tid;
    uint8_t ba_type;
    uint8_t ba_rsv0[2];
    uint16_t ba_sn;
    uint8_t ba_en;
    uint8_t ba_winsize_idx;
    uint16_t ba_winsize;
    uint8_t peer_addr[6];
    uint8_t rst_ba_tid;
    uint8_t rst_ba_sel;
    uint8_t rst_ba_sb;
    uint8_t band_idx;
    uint8_t ba_rsv1[4];
  };

  struct StaRecBaMsg {
    /* sta_req_hdr (8 B) */
    uint8_t bss_idx;
    uint8_t wlan_idx_lo;
    uint16_t tlv_num;
    uint8_t is_tlv_append;
    uint8_t muar_idx;
    uint8_t wlan_idx_hi;
    uint8_t rsv0;

    /* sta_rec_ba (12 B) */
    uint16_t ba_tag;
    uint16_t ba_len;
    uint8_t ba_tid;
    uint8_t ba_type;
    uint8_t ba_amsdu;
    uint8_t ba_en;
    uint16_t ba_ssn;
    uint16_t ba_winsize;
  };
#pragma pack(pop)
  static_assert(sizeof(WtblBaMsg) == 48, "WtblBaMsg must be 48 bytes");
  static_assert(sizeof(StaRecBaMsg) == 20, "StaRecBaMsg must be 20 bytes");

  /* Msg 1: STA_REC_WTBL + WTBL_BA (48 B) */
  WtblBaMsg msg1{};
  msg1.bss_idx = 0;
  msg1.wlan_idx_lo = wcid;
  msg1.tlv_num = 1;
  msg1.is_tlv_append = 1;
  msg1.wtbl_tlv_tag = STA_REC_WTBL;
  msg1.wtbl_tlv_len = 40; /* 4 + 8 + 28 */
  msg1.wtbl_wlan_idx_lo = wcid;
  msg1.wtbl_operation = WTBL_SET;
  msg1.wtbl_tlv_num = 1;
  msg1.ba_tag = WTBL_BA;
  msg1.ba_len = 28;
  msg1.ba_tid = tid;
  msg1.ba_type = MT_BA_TYPE_RECIPIENT;
  msg1.ba_winsize = enable ? winsize : 0u;
  std::memcpy(msg1.peer_addr, mac, 6);
  msg1.rst_ba_tid = tid;
  msg1.rst_ba_sel = RST_BA_MAC_TID_MATCH;
  msg1.rst_ba_sb = 1;
  msg1.band_idx = 0;

  if (!mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, &msg1, sizeof(msg1), true)) {
    log("WARNING: WTBL_BA update failed for wcid=" + std::to_string(wcid) + " tid=" +
        std::to_string(tid));
    return false;
  }

  /* Msg 2: STA_REC_BA (20 B) */
  StaRecBaMsg msg2{};
  msg2.bss_idx = 0;
  msg2.wlan_idx_lo = wcid;
  msg2.tlv_num = 1;
  msg2.is_tlv_append = 1;
  msg2.ba_tag = STA_REC_BA;
  msg2.ba_len = 12;
  msg2.ba_tid = tid;
  msg2.ba_type = MT_BA_TYPE_RECIPIENT;
  msg2.ba_amsdu = amsdu ? 1u : 0u;
  msg2.ba_en = enable ? static_cast<uint8_t>(1u << tid) : 0u;
  msg2.ba_ssn = ssn;
  msg2.ba_winsize = winsize;

  if (!mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, &msg2, sizeof(msg2), true)) {
    log("WARNING: STA_REC_BA update failed for wcid=" + std::to_string(wcid) + " tid=" +
        std::to_string(tid));
    return false;
  }

  log(std::string(enable ? "HW RX BA enabled" : "HW RX BA disabled") + " wcid=" +
      std::to_string(wcid) + " peer=" + mac_str(mac) + " tid=" + std::to_string(tid) +
      " ssn=" + std::to_string(ssn) + " win=" + std::to_string(winsize) +
      " amsdu=" + std::to_string(amsdu ? 1 : 0));
  return true;
}

bool MtkDevice::mcu_set_tx_ba(uint8_t wcid, const uint8_t mac[6], uint8_t tid, uint16_t ssn,
                              uint16_t winsize, bool amsdu, bool enable) {
  if (wcid == 0 || wcid >= MT792X_WTBL_RESERVED || tid >= 8) return false;

#pragma pack(push, 1)
  struct WtblBaMsg {
    /* sta_req_hdr (8 B) */
    uint8_t bss_idx;
    uint8_t wlan_idx_lo;
    uint16_t tlv_num;
    uint8_t is_tlv_append;
    uint8_t muar_idx;
    uint8_t wlan_idx_hi;
    uint8_t rsv0;

    /* STA_REC_WTBL outer TLV header (4 B) */
    uint16_t wtbl_tlv_tag;
    uint16_t wtbl_tlv_len;

    /* wtbl_req_hdr (8 B) */
    uint8_t wtbl_wlan_idx_lo;
    uint8_t wtbl_operation;
    uint16_t wtbl_tlv_num;
    uint8_t wtbl_wlan_idx_hi;
    uint8_t wtbl_rsv[3];

    /* wtbl_ba (28 B) */
    uint16_t ba_tag;
    uint16_t ba_len;
    uint8_t ba_tid;
    uint8_t ba_type;
    uint8_t ba_rsv0[2];
    uint16_t ba_sn;
    uint8_t ba_en;
    uint8_t ba_winsize_idx;
    uint16_t ba_winsize;
    uint8_t peer_addr[6];
    uint8_t rst_ba_tid;
    uint8_t rst_ba_sel;
    uint8_t rst_ba_sb;
    uint8_t band_idx;
    uint8_t ba_rsv1[4];
  };

  struct StaRecBaMsg {
    /* sta_req_hdr (8 B) */
    uint8_t bss_idx;
    uint8_t wlan_idx_lo;
    uint16_t tlv_num;
    uint8_t is_tlv_append;
    uint8_t muar_idx;
    uint8_t wlan_idx_hi;
    uint8_t rsv0;

    /* sta_rec_ba (12 B) */
    uint16_t ba_tag;
    uint16_t ba_len;
    uint8_t ba_tid;
    uint8_t ba_type;
    uint8_t ba_amsdu;
    uint8_t ba_en;
    uint16_t ba_ssn;
    uint16_t ba_winsize;
  };
#pragma pack(pop)
  static_assert(sizeof(WtblBaMsg) == 48, "WtblBaMsg must be 48 bytes");
  static_assert(sizeof(StaRecBaMsg) == 20, "StaRecBaMsg must be 20 bytes");

  static constexpr uint8_t kBaRange[8] = {4, 8, 12, 24, 36, 48, 54, 64};
  uint8_t winsize_idx = 0;
  if (enable) {
    for (int i = 7; i > 0; i--) {
      if (winsize >= kBaRange[i]) {
        winsize_idx = static_cast<uint8_t>(i);
        break;
      }
    }
  }

  /* Msg 1: STA_REC_WTBL + WTBL_BA (Originator, 48 B) */
  WtblBaMsg msg1{};
  msg1.bss_idx = 0;
  msg1.wlan_idx_lo = wcid;
  msg1.tlv_num = 1;
  msg1.is_tlv_append = 1;
  msg1.wtbl_tlv_tag = STA_REC_WTBL;
  msg1.wtbl_tlv_len = 40; /* 4 + 8 + 28 */
  msg1.wtbl_wlan_idx_lo = wcid;
  msg1.wtbl_operation = WTBL_SET;
  msg1.wtbl_tlv_num = 1;
  msg1.ba_tag = WTBL_BA;
  msg1.ba_len = 28;
  msg1.ba_tid = tid;
  msg1.ba_type = MT_BA_TYPE_ORIGINATOR;
  msg1.ba_sn = ssn;
  msg1.ba_en = enable ? 1u : 0u;
  msg1.ba_winsize_idx = winsize_idx;
  msg1.ba_winsize = enable ? winsize : 0u;

  if (!mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, &msg1, sizeof(msg1), true)) {
    log("WARNING: TX WTBL_BA update failed for wcid=" + std::to_string(wcid) + " tid=" +
        std::to_string(tid));
    return false;
  }

  /* Msg 2: STA_REC_BA (Originator, 20 B) */
  StaRecBaMsg msg2{};
  msg2.bss_idx = 0;
  msg2.wlan_idx_lo = wcid;
  msg2.tlv_num = 1;
  msg2.is_tlv_append = 1;
  msg2.ba_tag = STA_REC_BA;
  msg2.ba_len = 12;
  msg2.ba_tid = tid;
  msg2.ba_type = MT_BA_TYPE_ORIGINATOR;
  msg2.ba_amsdu = amsdu ? 1u : 0u;
  msg2.ba_en = enable ? static_cast<uint8_t>(1u << tid) : 0u;
  msg2.ba_ssn = ssn;
  msg2.ba_winsize = winsize;

  if (!mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, &msg2, sizeof(msg2), true)) {
    log("WARNING: TX STA_REC_BA update failed for wcid=" + std::to_string(wcid) + " tid=" +
        std::to_string(tid));
    return false;
  }

  _peers[wcid].tx_ba_active[tid] = enable;
  log(std::string(enable ? "HW TX BA enabled" : "HW TX BA disabled") + " wcid=" +
      std::to_string(wcid) + " peer=" + mac_str(mac) + " tid=" + std::to_string(tid) +
      " ssn=" + std::to_string(ssn) + " win=" + std::to_string(winsize) +
      " amsdu=" + std::to_string(amsdu ? 1 : 0));
  return true;
}

void MtkDevice::snoop_rx_ba(const uint8_t *frame, size_t len) {
  if (len < 24 || (frame[0] & 0xfcu) != 0xd0u) return; /* Management Action only */
  if (std::memcmp(frame + 4, _mac.data(), 6) != 0) return; /* DA must be us */

  const uint16_t fc = read_le16(frame);
  const size_t hdr_len = ieee80211_hdrlen(fc);
  if (len < hdr_len + 6) return;

  const uint8_t *body = frame + hdr_len;
  if (body[0] != 3u) return; /* Category 3 = Block Ack */
  const uint8_t *peer_mac = frame + 10; /* SA */

  if (body[1] == 0u && len >= hdr_len + 9) { /* Incoming ADDBA Request -> RX BA */
    const uint8_t token = body[2];
    const uint16_t params = read_le16(body + 3);
    const bool amsdu = (params & 1u) != 0;
    const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0x0fu);
    const uint16_t ssn = static_cast<uint16_t>((read_le16(body + 7) >> 4) & 0x0fffu);

    size_t slot = 0;
    uint64_t oldest_ts = ~0ULL;
    for (size_t i = 0; i < _pending_addba.size(); i++) {
      const auto &e = _pending_addba[i];
      if (e.valid && e.tid == tid && e.token == token &&
          std::memcmp(e.peer_mac.data(), peer_mac, 6) == 0) {
        slot = i;
        break;
      }
      if (!e.valid) {
        slot = i;
        oldest_ts = 0;
        break;
      }
      if (e.ts_ms < oldest_ts) {
        oldest_ts = e.ts_ms;
        slot = i;
      }
    }
    auto &dst = _pending_addba[slot];
    dst.valid = true;
    std::memcpy(dst.peer_mac.data(), peer_mac, 6);
    dst.tid = tid;
    dst.token = token;
    dst.ssn = ssn;
    dst.amsdu = amsdu;
    dst.ts_ms = now_ms();
  } else if (body[1] == 1u && len >= hdr_len + 9) { /* Incoming ADDBA Response -> TX BA */
    const uint16_t status = read_le16(body + 3);
    if (status != 0u) return;

    const uint8_t token = body[2];
    const uint16_t params = read_le16(body + 5);
    const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0x0fu);
    const uint16_t raw_win = static_cast<uint16_t>((params >> 6) & 0x03ffu);
    const uint16_t winsize = std::clamp<uint16_t>(raw_win, 1u, 64u);

    uint16_t ssn = 0;
    bool amsdu = (params & 1u) != 0;
    for (auto &e : _pending_tx_addba) {
      if (e.valid && e.tid == tid && e.token == token &&
          std::memcmp(e.peer_mac.data(), peer_mac, 6) == 0) {
        ssn = e.ssn;
        amsdu = amsdu && e.amsdu;
        e.valid = false;
        break;
      }
    }

    const int wcid = get_or_add_peer_wcid(peer_mac);
    if (wcid > 0) {
      (void)mcu_set_tx_ba(static_cast<uint8_t>(wcid), peer_mac, tid, ssn, winsize, amsdu, true);
    }
  } else if (body[1] == 2u && len >= hdr_len + 6) { /* DELBA */
    const uint16_t params = read_le16(body + 2);
    const uint8_t initiator = static_cast<uint8_t>((params >> 11) & 1u);
    const uint8_t tid = static_cast<uint8_t>((params >> 12) & 0x0fu);
    const int wcid = find_peer_wcid(peer_mac);
    if (wcid > 0) {
      if (initiator == 1u) {
        (void)mcu_set_rx_ba(static_cast<uint8_t>(wcid), peer_mac, tid, 0, 0, false, false);
      } else {
        (void)mcu_set_tx_ba(static_cast<uint8_t>(wcid), peer_mac, tid, 0, 0, false, false);
      }
    }
  }
}

void MtkDevice::snoop_tx_ba(const uint8_t *frame, size_t len) {
  if (len < 24 || (frame[0] & 0xfcu) != 0xd0u) return; /* Management Action only */
  const uint16_t fc = read_le16(frame);
  const size_t hdr_len = ieee80211_hdrlen(fc);
  if (len < hdr_len + 6) return;

  const uint8_t *body = frame + hdr_len;
  if (body[0] != 3u) return; /* Category 3 = Block Ack */
  const uint8_t *peer_mac = frame + 4; /* DA */

  if (body[1] == 0u && len >= hdr_len + 9) { /* Outgoing ADDBA Request -> track for TX BA */
    const uint8_t token = body[2];
    const uint16_t params = read_le16(body + 3);
    const bool amsdu = (params & 1u) != 0;
    const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0x0fu);
    const uint16_t ssn = static_cast<uint16_t>((read_le16(body + 7) >> 4) & 0x0fffu);

    size_t slot = 0;
    uint64_t oldest_ts = ~0ULL;
    for (size_t i = 0; i < _pending_tx_addba.size(); i++) {
      const auto &e = _pending_tx_addba[i];
      if (e.valid && e.tid == tid && e.token == token &&
          std::memcmp(e.peer_mac.data(), peer_mac, 6) == 0) {
        slot = i;
        break;
      }
      if (!e.valid) {
        slot = i;
        oldest_ts = 0;
        break;
      }
      if (e.ts_ms < oldest_ts) {
        oldest_ts = e.ts_ms;
        slot = i;
      }
    }
    auto &dst = _pending_tx_addba[slot];
    dst.valid = true;
    std::memcpy(dst.peer_mac.data(), peer_mac, 6);
    dst.tid = tid;
    dst.token = token;
    dst.ssn = ssn;
    dst.amsdu = amsdu;
    dst.ts_ms = now_ms();
  } else if (body[1] == 1u && len >= hdr_len + 9) { /* Outgoing ADDBA Response -> RX BA */
    const uint16_t status = read_le16(body + 3);
    if (status != 0u) return;

    const uint8_t token = body[2];
    const uint16_t params = read_le16(body + 5);
    const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0x0fu);
    const uint16_t raw_win = static_cast<uint16_t>((params >> 6) & 0x03ffu);
    const uint16_t winsize = std::clamp<uint16_t>(raw_win, 1u, 64u);

    uint16_t ssn = 0;
    bool amsdu = (params & 1u) != 0;
    for (auto &e : _pending_addba) {
      if (e.valid && e.tid == tid && e.token == token &&
          std::memcmp(e.peer_mac.data(), peer_mac, 6) == 0) {
        ssn = e.ssn;
        amsdu = amsdu && e.amsdu;
        e.valid = false;
        break;
      }
    }

    const int wcid = get_or_add_peer_wcid(peer_mac);
    if (wcid > 0) {
      (void)mcu_set_rx_ba(static_cast<uint8_t>(wcid), peer_mac, tid, ssn, winsize, amsdu, true);
    }
  } else if (body[1] == 2u && len >= hdr_len + 6) { /* Outgoing DELBA */
    const uint16_t params = read_le16(body + 2);
    const uint8_t initiator = static_cast<uint8_t>((params >> 11) & 1u);
    const uint8_t tid = static_cast<uint8_t>((params >> 12) & 0x0fu);
    const int wcid = find_peer_wcid(peer_mac);
    if (wcid > 0) {
      if (initiator == 0u) {
        (void)mcu_set_rx_ba(static_cast<uint8_t>(wcid), peer_mac, tid, 0, 0, false, false);
      } else {
        (void)mcu_set_tx_ba(static_cast<uint8_t>(wcid), peer_mac, tid, 0, 0, false, false);
      }
    }
  }
}

bool MtkDevice::sta_add(uint8_t wcid, const uint8_t peer[6], const PeerCaps &caps, uint8_t omac) {
  if (wcid == 0 || wcid >= MT792X_WTBL_RESERVED) return false;

  rmw(MT_WTBL_UPDATE, MT_WTBL_UPDATE_WLAN_IDX,
      field_prep(MT_WTBL_UPDATE_WLAN_IDX, wcid) | MT_WTBL_UPDATE_ADM_COUNT_CLEAR);
  (void)poll_reg(MT_WTBL_UPDATE, MT_WTBL_UPDATE_BUSY, 0, 100);

  StaReq r(wcid, omac);
  { /* mt76_connac_mcu_sta_basic_tlv */
    uint8_t *t = r.add(STA_REC_BASIC, 20);
    put_le<uint32_t>(t, 4, CONNECTION_INFRA_STA);
    t[8] = CONN_STATE_PORT_SECURE;
    t[9] = 1; /* qos */
    std::memcpy(t + 12, peer, 6);
    put_le<uint16_t>(t, 18, EXTRA_INFO_VER | EXTRA_INFO_NEW);
  }
  { /* mt76_connac_mcu_sta_tlv */
    uint8_t *ht = r.add(STA_REC_HT, 8);
    put_le<uint16_t>(ht, 4, caps.ht_cap);
    if (caps.vht) {
      uint8_t *vht = r.add(STA_REC_VHT, 16);
      put_le<uint32_t>(vht, 4, caps.vht_cap);
      put_le<uint16_t>(vht, 8, caps.vht_mcs);
      put_le<uint16_t>(vht, 10, caps.vht_mcs);
    }
    uint8_t *phy = r.add(STA_REC_PHY, 12);
    put_le<uint16_t>(phy, 4, _channel > 14 ? 0x15u : 0x0fu);
    phy[6] = static_cast<uint8_t>(_channel > 14 ? (bit(3) | bit(4) | (caps.vht ? bit(5) : 0u))
                                                : (bit(0) | bit(1) | bit(4)));
    phy[7] = static_cast<uint8_t>((caps.ampdu_params & 0x3u) | (caps.ampdu_params & 0x1cu));
    uint8_t *ra = r.add(STA_REC_RA, 16);
    put_le<uint16_t>(ra, 4, static_cast<uint16_t>(_channel > 14 ? (0xffu << 6) : ((0xffu << 6) | 0xfu)));
    ra[6] = caps.rx_mcs[0];
    ra[7] = caps.rx_mcs[1];
    uint8_t *st = r.add(STA_REC_STATE, 12);
    st[8] = STA_STATE_ASSOC;
    if (caps.vht || omac) {
      st[9] = static_cast<uint8_t>(caps.bw | ((caps.nss > 0 ? caps.nss - 1 : 0) << 4));
    }
  }
  r.wtbl(WTBL_RESET_AND_SET, wcid);
  { /* mt76_connac_mcu_wtbl_generic_tlv, _hdr_trans_tlv, _ht_tlv */
    uint8_t *g = r.wadd(WTBL_GENERIC, 20);
    std::memcpy(g + 4, peer, 6);
    g[10] = omac; /* muar_idx */
    g[13] = 1;    /* qos */
    uint8_t *rx = r.wadd(WTBL_RX, 12);
    rx[5] = 0; /* rca1: 0 under CONNECTION_INFRA_AP */
    rx[6] = 1; /* rca2 */
    rx[7] = 1; /* rv */
    uint8_t *h = r.wadd(WTBL_HDR_TRANS, 8);
    h[5] = 1; /* from_ds */
    h[6] = 1; /* no_rx_trans: raw 802.11 */
    uint8_t af = caps.ampdu_params & 0x3u;
    uint8_t *ht = r.wadd(WTBL_HT, 12);
    ht[4] = 1;
    ht[5] = (caps.ht_cap & 0x1u) ? 1u : 0u; /* LDPC */
    ht[7] = static_cast<uint8_t>((caps.ampdu_params >> 2) & 0x7u);
    if (caps.vht) {
      uint8_t *vht = r.wadd(WTBL_VHT, 12);
      vht[4] = (caps.vht_cap & bit(4)) ? 1u : 0u; /* RXLDPC */
      vht[6] = 1;
      af = std::max<uint8_t>(af, static_cast<uint8_t>((caps.vht_cap >> 23) & 0x7u));
    }
    ht[6] = af;
    (void)r.wadd(WTBL_SMPS, 8);
  }
  const auto &b = r.bytes();
  const bool ok = mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, b.data(), b.size(), true, sta_cmd_timeout_ms);
  log(std::string("station ") + std::to_string(wcid) + " (" + mac_str(peer) + ") " +
      (ok ? "added" : "not added"));
  return ok;
}

bool MtkDevice::sta_remove(uint8_t wcid, const uint8_t peer[6], uint8_t omac) {
  if (wcid == 0 || wcid >= MT792X_WTBL_RESERVED) return false;
  StaReq r(wcid, omac);
  uint8_t *t = r.add(STA_REC_BASIC, 20);
  put_le<uint32_t>(t, 4, CONNECTION_INFRA_STA);
  t[8] = CONN_STATE_DISCONNECT;
  std::memcpy(t + 12, peer, 6);
  put_le<uint16_t>(t, 18, EXTRA_INFO_VER);
  r.wtbl(WTBL_RESET_AND_SET, wcid);
  const auto &b = r.bytes();
  return mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, b.data(), b.size(), true, sta_cmd_timeout_ms);
}

bool MtkDevice::sta_ba(uint8_t wcid, const uint8_t peer[6], uint8_t tid, uint16_t ssn, uint16_t win,
                       bool tx, bool enable, uint8_t omac) {
  if (wcid == 0 || wcid >= MT792X_WTBL_RESERVED || tid >= 8) return false;
  { /* mt76_connac_mcu_wtbl_ba_tlv */
    StaReq r(wcid, omac);
    r.wtbl(WTBL_SET, wcid);
    uint8_t *ba = r.wadd(WTBL_BA, 28);
    ba[4] = tid;
    if (tx) {
      ba[5] = MT_BA_TYPE_ORIGINATOR;
      put_le<uint16_t>(ba, 8, enable ? ssn : 0u);
      ba[10] = enable ? 1u : 0u;
    } else {
      ba[5] = MT_BA_TYPE_RECIPIENT;
      std::memcpy(ba + 14, peer, 6);
      ba[20] = tid;
      ba[21] = RST_BA_MAC_TID_MATCH;
      ba[22] = 1;
    }
    put_le<uint16_t>(ba, 12, enable ? win : 0u);
    const auto &b = r.bytes();
    if (!mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, b.data(), b.size(), true, sta_cmd_timeout_ms))
      return false;
  }
  { /* mt76_connac_mcu_sta_ba_tlv */
    StaReq r(wcid, omac);
    uint8_t *ba = r.add(STA_REC_BA, 12);
    ba[4] = tid;
    ba[5] = tx ? MT_BA_TYPE_ORIGINATOR : MT_BA_TYPE_RECIPIENT;
    ba[6] = 0; /* no A-MSDU */
    ba[7] = static_cast<uint8_t>(enable ? (1u << tid) : 0u);
    put_le<uint16_t>(ba, 8, ssn);
    put_le<uint16_t>(ba, 10, win);
    const auto &b = r.bytes();
    return mcu_send_uni(MCU_UNI_CMD_STA_REC_UPDATE, b.data(), b.size(), true, sta_cmd_timeout_ms);
  }
}

/* --------------------------------------------------------------------------
 * Raw 802.11 TX & RX Data Path
 * -------------------------------------------------------------------------- */
uint16_t MtkDevice::tx_rate_ofdm(unsigned mbps) { return ofdm_rate_val(mbps); }
uint16_t MtkDevice::tx_rate_cck(unsigned mbps_x2) { return cck_rate_val(mbps_x2); }
uint16_t MtkDevice::tx_rate_ht(unsigned mcs) { return ht_rate_val(static_cast<uint8_t>(mcs)); }

int MtkDevice::queue_tx_frame(const uint8_t *frame, size_t len, const uint32_t txd[8], bool kick) {
  if (!_dma_running || !frame || len < 10 || len > DmaPool32::kPageBytes) return -1;

  tx_ring_reclaim(_tx_data);
  const uint32_t next = (_tx_data.head + 1) % _tx_data.count;
  if (next == _tx_data.tail) return -1;

  const uint32_t idx = _tx_data.head;
  std::memcpy(_tx_data.bufs[idx].va, frame, len);

  auto *txwi = reinterpret_cast<uint32_t *>(_tx_data.txwi[idx].va);
  std::memset(txwi, 0, MT_TXD_SIZE + sizeof(MtkHwTxp));
  std::memcpy(txwi, txd, MT_TXD_SIZE);

  auto *txp = reinterpret_cast<MtkHwTxp *>(_tx_data.txwi[idx].va + MT_TXD_SIZE);
  _tx_token = (_tx_token + 1) & 0x1fffu;
  const uint16_t token = _tx_token;
  txp->msdu_id[0] = static_cast<uint16_t>(token | MT_MSDU_ID_VALID);
  txp->ptr[0].buf0 = _tx_data.bufs[idx].pa;
  txp->ptr[0].len0 = static_cast<uint16_t>((len & MT_TXD_LEN_MASK) | MT_TXD_LEN_LAST);

  MtkDesc *d = _tx_data.descs();
  d[idx].buf0 = _tx_data.txwi[idx].pa;
  d[idx].buf1 = 0;
  d[idx].info = 0;
  plat::mem_barrier();
  d[idx].ctrl = field_prep(MT_DMA_CTL_SD_LEN0, static_cast<uint32_t>(MT_TXD_SIZE + sizeof(MtkHwTxp))) |
                MT_DMA_CTL_LAST_SEC0;
  plat::mem_barrier();

  _tx_data.head = next;
  if (kick) {
    wr(_tx_data.reg_base + 0x8, _tx_data.head);
    _tx_pending_kick = false;
  } else {
    _tx_pending_kick = true;
  }
  return static_cast<int>(token);
}

int MtkDevice::transmit_raw(const uint8_t *frame, size_t len, uint16_t rate_val, uint8_t omac,
                            bool no_ack, bool force_altx) {
  if (!_dma_running || !frame || len < 10 || len > DmaPool32::kPageBytes) return -1;
  const uint16_t fc = static_cast<uint16_t>(frame[0] | (frame[1] << 8));
  const uint8_t fc_type = static_cast<uint8_t>((fc >> 2) & 0x3u);
  const uint8_t fc_stype = static_cast<uint8_t>((fc >> 4) & 0xfu);
  const bool multicast = (frame[4] & 1u) != 0;
  const bool eff_no_ack = no_ack || multicast;
  const size_t hdr_len = ieee80211_hdrlen(fc);
  const bool is_qos_data = (frame[0] & 0xfcu) == 0x88u;
  const uint8_t tid =
      (is_qos_data && len >= hdr_len) ? static_cast<uint8_t>(frame[hdr_len - 2] & 0x07u) : 0u;
  const uint32_t q_idx = (force_altx || fc_type == 0 || multicast) ? MT_LMAC_ALTX0 : MT_LMAC_AC00;

  uint32_t txd[8] = {};
  txd[0] = field_prep(MT_TXD0_TX_BYTES, static_cast<uint32_t>(len + MT_TXD_SIZE)) |
           field_prep(MT_TXD0_PKT_FMT, MT_TX_TYPE_CT) |
           field_prep(MT_TXD0_Q_IDX, q_idx);
  txd[1] = MT_TXD1_LONG_FORMAT |
           field_prep(MT_TXD1_WLAN_IDX, 0u) |
           field_prep(MT_TXD1_OWN_MAC, omac) |
           field_prep(MT_TXD1_TID, tid) |
           field_prep(MT_TXD1_HDR_FORMAT, MT_HDR_FORMAT_802_11) |
           field_prep(MT_TXD1_HDR_INFO, static_cast<uint32_t>(hdr_len / 2));
  txd[2] = field_prep(MT_TXD2_FRAME_TYPE, fc_type) |
           field_prep(MT_TXD2_SUB_TYPE, fc_stype) |
           field_prep(MT_TXD2_MULTICAST, multicast ? 1u : 0u) |
           MT_TXD2_FIX_RATE | MT_TXD2_HTC_VLD;
  txd[3] = field_prep(MT_TXD3_REM_TX_COUNT, eff_no_ack ? 1u : 15u) |
           MT_TXD3_BA_DISABLE |
           (eff_no_ack ? MT_TXD3_NO_ACK : 0u);
  if (fc_type != 1 && len >= 24) {
    const uint16_t sc = static_cast<uint16_t>(frame[22] | (frame[23] << 8));
    txd[3] |= MT_TXD3_SN_VALID | field_prep(MT_TXD3_SEQ, (sc >> 4) & 0xfffu);
  }
  txd[6] = MT_TXD6_FIXED_BW | field_prep(MT_TXD6_TX_RATE, rate_val);
  txd[7] = field_prep(MT_TXD7_TYPE, fc_type) |
           field_prep(MT_TXD7_SUB_TYPE, fc_stype) |
           field_prep(MT_TXD7_SPE_IDX, 24u);
  return queue_tx_frame(frame, len, txd, true);
}

int MtkDevice::transmit_sta(const uint8_t *frame, size_t len, uint8_t wcid, uint8_t omac) {
  if (!_dma_running || !frame || len < 26 || len > DmaPool32::kPageBytes) return -1;
  const uint16_t fc = static_cast<uint16_t>(frame[0] | (frame[1] << 8));
  const uint8_t fc_type = static_cast<uint8_t>((fc >> 2) & 0x3u);
  const uint8_t fc_stype = static_cast<uint8_t>((fc >> 4) & 0xfu);
  const size_t hdr_len = ieee80211_hdrlen(fc);
  if (fc_type != 2 || !(fc_stype & 8u) || len < hdr_len) return -1;
  const uint8_t tid = static_cast<uint8_t>(frame[hdr_len - 2] & 0x07u);
  const uint16_t sc = static_cast<uint16_t>(frame[22] | (frame[23] << 8));

  uint32_t txd[8] = {};
  txd[0] = field_prep(MT_TXD0_TX_BYTES, static_cast<uint32_t>(len + MT_TXD_SIZE)) |
           field_prep(MT_TXD0_PKT_FMT, MT_TX_TYPE_CT) |
           field_prep(MT_TXD0_Q_IDX, MT_LMAC_AC00);
  txd[1] = MT_TXD1_LONG_FORMAT |
           field_prep(MT_TXD1_WLAN_IDX, wcid) |
           field_prep(MT_TXD1_OWN_MAC, omac) |
           field_prep(MT_TXD1_TID, tid) |
           field_prep(MT_TXD1_HDR_FORMAT, MT_HDR_FORMAT_802_11) |
           field_prep(MT_TXD1_HDR_INFO, static_cast<uint32_t>(hdr_len / 2));
  txd[2] = field_prep(MT_TXD2_FRAME_TYPE, fc_type) |
           field_prep(MT_TXD2_SUB_TYPE, fc_stype);
  /* Firmware rate control picks rate from STA record; keep TX BA disabled for the AP. */
  txd[3] = field_prep(MT_TXD3_REM_TX_COUNT, 15u) |
           MT_TXD3_BA_DISABLE | MT_TXD3_SN_VALID |
           field_prep(MT_TXD3_SEQ, (sc >> 4) & 0xfffu);
  txd[7] = field_prep(MT_TXD7_TYPE, fc_type) |
           field_prep(MT_TXD7_SUB_TYPE, fc_stype) |
           field_prep(MT_TXD7_SPE_IDX, 24u);
  return queue_tx_frame(frame, len, txd, true);
}

bool MtkDevice::transmit(const uint8_t *frame, size_t len, unsigned rate_mbps, bool kick) {
  TxParams params{};
  params.is_ht = false;
  params.rate_mbps = static_cast<uint8_t>(rate_mbps);
  return transmit(frame, len, params, kick);
}

void MtkDevice::kick_tx() {
  if (_dma_running && _tx_pending_kick) {
    wr(_tx_data.reg_base + 0x8, _tx_data.head);
    _tx_pending_kick = false;
  }
}

bool MtkDevice::transmit(const uint8_t *frame, size_t len, const TxParams &params, bool kick) {
  if (!_dma_running || !frame || len < 16 || len > DmaPool32::kPageBytes) return false;

  /* Configure hardware RX Block Ack before transmitting ADDBA Response / DELBA. */
  snoop_tx_ba(frame, len);

  tx_ring_reclaim(_tx_data);
  const uint32_t next = (_tx_data.head + 1) % _tx_data.count;
  if (next == _tx_data.tail) return false;

  const uint32_t idx = _tx_data.head;
  std::memcpy(_tx_data.bufs[idx].va, frame, len);

  const uint16_t fc = static_cast<uint16_t>(frame[0] | (frame[1] << 8));
  const uint16_t sc = static_cast<uint16_t>(frame[22] | (frame[23] << 8));
  const uint8_t fc_type = static_cast<uint8_t>((fc >> 2) & 0x3u);
  const uint8_t fc_stype = static_cast<uint8_t>((fc >> 4) & 0xfu);
  const bool multicast = (frame[4] & 1u) != 0;
  const bool no_ack = multicast || params.no_ack;
  const size_t hdr_len = ieee80211_hdrlen(fc);
  const bool is_qos_data = (frame[0] & 0xfcu) == 0x88u;
  const uint8_t tid =
      (is_qos_data && len >= 26) ? static_cast<uint8_t>(frame[24] & 0x07u) : 0u;
  uint32_t wlan_idx = 0;
  bool use_tx_ba = false;
  bool use_fw_ra = false;
  if (is_qos_data && !multicast && !no_ack) {
    /* Register the peer WTBL entry on first unicast QoS Data frame so Connac2
     * firmware Rate Adaptation (HE80/VHT80 2SS with automatic fallback on
     * retries) drives TX instead of a fragile fixed rate on broadcast WCID 0 (#56, #66). */
    const int peer_wcid = get_or_add_peer_wcid(frame + 4);
    if (peer_wcid > 0) {
      wlan_idx = static_cast<uint32_t>(peer_wcid);
      use_fw_ra = true;
      use_tx_ba = _peers[peer_wcid].tx_ba_active[tid];
    }
  }

  auto *txwi = reinterpret_cast<uint32_t *>(_tx_data.txwi[idx].va);
  std::memset(txwi, 0, MT_TXD_SIZE + sizeof(MtkHwTxp));

  const uint32_t q_idx = (fc_type == 0) ? MT_LMAC_ALTX0 : MT_LMAC_AC00;
  txwi[0] = field_prep(MT_TXD0_TX_BYTES, static_cast<uint32_t>(len + MT_TXD_SIZE)) |
            field_prep(MT_TXD0_PKT_FMT, MT_TX_TYPE_CT) |
            field_prep(MT_TXD0_Q_IDX, q_idx);
  txwi[1] = MT_TXD1_LONG_FORMAT |
            field_prep(MT_TXD1_WLAN_IDX, wlan_idx) |
            field_prep(MT_TXD1_OWN_MAC, 0) |
            field_prep(MT_TXD1_TID, tid) |
            field_prep(MT_TXD1_HDR_FORMAT, MT_HDR_FORMAT_802_11) |
            field_prep(MT_TXD1_HDR_INFO, static_cast<uint32_t>(hdr_len / 2));
  txwi[2] = field_prep(MT_TXD2_FRAME_TYPE, fc_type) |
            field_prep(MT_TXD2_SUB_TYPE, fc_stype) |
            field_prep(MT_TXD2_MULTICAST, multicast ? 1u : 0u) |
            (use_fw_ra ? 0u : (MT_TXD2_FIX_RATE | MT_TXD2_HTC_VLD));
  txwi[3] = field_prep(MT_TXD3_REM_TX_COUNT, no_ack ? 1u : 15u) |
            (use_tx_ba ? 0u : MT_TXD3_BA_DISABLE) | MT_TXD3_SN_VALID |
            field_prep(MT_TXD3_SEQ, (sc >> 4) & 0xfffu) |
            (no_ack ? MT_TXD3_NO_ACK : 0u);
  if (use_fw_ra) {
    /* Let Connac2 firmware RA select HE80/VHT80 2SS MCS with automatic fallback. */
    txwi[6] = 0;
  } else if (params.is_vht) {
    txwi[6] = MT_TXD6_FIXED_BW | MT_TXD6_LDPC |
              field_prep(MT_TXD6_BW, params.bw) |
              field_prep(MT_TXD6_SGI, params.sgi ? 1u : 0u) |
              field_prep(MT_TXD6_TX_RATE, vht_rate_val(params.mcs, params.nss));
  } else if (params.is_ht) {
    txwi[6] = MT_TXD6_FIXED_BW | MT_TXD6_LDPC |
              field_prep(MT_TXD6_BW, params.bw) |
              field_prep(MT_TXD6_SGI, params.sgi ? 1u : 0u) |
              field_prep(MT_TXD6_TX_RATE, ht_rate_val(params.mcs));
  } else {
    txwi[6] = MT_TXD6_FIXED_BW |
              field_prep(MT_TXD6_TX_RATE, ofdm_rate_val(params.rate_mbps));
  }
  txwi[7] = field_prep(MT_TXD7_TYPE, fc_type) |
            field_prep(MT_TXD7_SUB_TYPE, fc_stype) |
            field_prep(MT_TXD7_SPE_IDX, 24u);

  auto *txp = reinterpret_cast<MtkHwTxp *>(_tx_data.txwi[idx].va + MT_TXD_SIZE);
  _tx_token = (_tx_token + 1) & 0x1fffu;
  txp->msdu_id[0] = static_cast<uint16_t>(_tx_token | MT_MSDU_ID_VALID);
  txp->ptr[0].buf0 = _tx_data.bufs[idx].pa;
  txp->ptr[0].len0 = static_cast<uint16_t>((len & MT_TXD_LEN_MASK) | MT_TXD_LEN_LAST);

  MtkDesc *d = _tx_data.descs();
  d[idx].buf0 = _tx_data.txwi[idx].pa;
  d[idx].buf1 = 0;
  d[idx].info = 0;
  plat::mem_barrier();
  d[idx].ctrl = field_prep(MT_DMA_CTL_SD_LEN0, static_cast<uint32_t>(MT_TXD_SIZE + sizeof(MtkHwTxp))) |
                MT_DMA_CTL_LAST_SEC0;
  plat::mem_barrier();

  _tx_data.head = next;
  if (kick) {
    wr(_tx_data.reg_base + 0x8, _tx_data.head);
    _tx_pending_kick = false;
  } else {
    _tx_pending_kick = true;
  }
  return true;
}

bool MtkDevice::parse_rx_packet(const uint8_t *buf, size_t len, RxFrame &out) {
  if (len < 24) return false;
  const auto *rxd = reinterpret_cast<const uint32_t *>(buf);
  const uint32_t rxd0 = rxd[0];
  const uint32_t rxd1 = rxd[1];
  const uint32_t rxd2 = rxd[2];
  const uint32_t rxd3 = rxd[3];

  uint32_t pkt_type = field_get(MT_RXD0_PKT_TYPE, rxd0);
  const uint32_t pkt_flag = field_get(MT_RXD0_PKT_FLAG, rxd0);
  if (pkt_type == PKT_TYPE_RX_EVENT && pkt_flag == 0x1) {
    pkt_type = PKT_TYPE_NORMAL_MCU;
  }
  if (pkt_type != PKT_TYPE_NORMAL && pkt_type != PKT_TYPE_NORMAL_MCU) return false;
  if (rxd1 & MT_RXD1_NORMAL_FCS_ERR) return false;
  if (rxd2 & (MT_RXD2_NORMAL_AMSDU_ERR | MT_RXD2_NORMAL_MAX_LEN_ERROR | MT_RXD2_NORMAL_HDR_TRANS))
    return false;

  const uint32_t total_len = field_get(MT_RXD0_LENGTH, rxd0);
  if (total_len > len) return false;

  size_t ofs = 6 * 4; /* 24-byte base RXD */
  if (rxd1 & MT_RXD1_NORMAL_GROUP_4) ofs += 4 * 4;
  if (rxd1 & MT_RXD1_NORMAL_GROUP_1) ofs += 4 * 4;
  if (rxd1 & MT_RXD1_NORMAL_GROUP_2) ofs += 2 * 4;

  int8_t dbm = -127;
  if (rxd1 & MT_RXD1_NORMAL_GROUP_3) {
    if (ofs + 8 > total_len) return false;
    const auto *rxv = reinterpret_cast<const uint32_t *>(buf + ofs);
    uint32_t v1 = rxv[1];
    ofs += 2 * 4;
    if (rxd1 & MT_RXD1_NORMAL_GROUP_5) {
      if (ofs + 18 * 4 > total_len) return false;
      v1 = *reinterpret_cast<const uint32_t *>(buf + ofs + 6 * 4);
      ofs += 18 * 4;
    }
    const int rcpi0 = static_cast<int>(field_get(MT_PRXV_RCPI0, v1));
    if (rcpi0 > 0) dbm = static_cast<int8_t>((rcpi0 - 220) / 2);
  }

  const uint32_t remove_pad = field_get(MT_RXD2_NORMAL_HDR_OFFSET, rxd2);
  ofs += 2 * remove_pad;
  if (ofs >= total_len) return false;

  const uint8_t *frame_ptr = buf + ofs;
  size_t frame_len = total_len - ofs;
  if (frame_len < 16) return false;

  /* Connac2 raw A-MSDU frames without header translation carry 2 bytes of
   * padding immediately after the 802.11 header (Linux mt7921_mac_fill_rx). */
  if (field_get(MT_RXD4_NORMAL_PAYLOAD_FORMAT, rxd[4]) != 0) {
    const uint16_t fc = read_le16(frame_ptr);
    const size_t hlen = ieee80211_hdrlen(fc);
    if (hlen >= 24 && hlen + 2 <= frame_len) {
      _rx_unpad_buf.resize(frame_len - 2);
      std::memcpy(_rx_unpad_buf.data(), frame_ptr, hlen);
      std::memcpy(_rx_unpad_buf.data() + hlen, frame_ptr + hlen + 2, frame_len - hlen - 2);
      frame_ptr = _rx_unpad_buf.data();
      frame_len -= 2;
    }
  }

  out.data = frame_ptr;
  out.len = frame_len;
  out.dbm = dbm;
  out.channel = static_cast<uint8_t>(field_get(MT_RXD3_NORMAL_CH_FREQ, rxd3));
  if (out.channel == 0) out.channel = _channel;
  return out.len >= 16;
}

void MtkDevice::handle_tx_free(const uint8_t *data, size_t len) { /* mt7921_mac_tx_free */
  if (len < 8) return;
  uint32_t rxd0 = 0;
  std::memcpy(&rxd0, data, sizeof(rxd0));
  unsigned count = static_cast<unsigned>(field_get(MT_TX_FREE_MSDU_CNT, rxd0));
  const auto *info = reinterpret_cast<const uint32_t *>(data + 8);
  const size_t entries = (len - 8) / 4;
  for (unsigned i = 0; i < count && i < entries; i++) {
    const uint32_t v = info[i];
    if (v & MT_TX_FREE_PAIR) {
      count++;
      continue;
    }
    const unsigned msdu = static_cast<unsigned>(field_get(MT_TX_FREE_MSDU_ID, v));
    const unsigned stat = static_cast<unsigned>(field_get(MT_TX_FREE_STATUS, v));
    const unsigned tries = static_cast<unsigned>(field_get(MT_TX_FREE_COUNT, v));
    if (_on_tx_status) _on_tx_status(msdu, stat == 0, tries);
  }
}

void MtkDevice::handle_rx_meta(const uint8_t *buf, size_t len) {
  if (len < 8) return;
  uint32_t rxd0 = 0;
  std::memcpy(&rxd0, buf, sizeof(rxd0));
  const uint32_t pkt_type = field_get(MT_RXD0_PKT_TYPE, rxd0);
  if (pkt_type == PKT_TYPE_TXRX_NOTIFY) {
    handle_tx_free(buf, len);
  } else if (pkt_type == PKT_TYPE_RX_EVENT && len >= sizeof(MtkMcuRxd)) {
    const auto *rxd = reinterpret_cast<const MtkMcuRxd *>(buf);
    if (rxd->eid == 0xf0u) {
      const std::string text(reinterpret_cast<const char *>(buf) + sizeof(MtkMcuRxd),
                             len - sizeof(MtkMcuRxd));
      if (text.find("ASSERT") != std::string::npos) {
        log("WARNING: firmware coredump ASSERT: " + text.substr(0, 120));
      }
    }
  }
}

size_t MtkDevice::poll(const RxHandler &on_rx) {
  if (!_dma_running) return 0;

  size_t count = 0;
  auto deliver = [&](const RxFrame &f) {
    snoop_rx_ba(f.data, f.len);
    if (on_rx) on_rx(f);
    count++;
  };
  // A callback may start another MCU command and enqueue more frames. Leave
  // those for the next poll rather than iterating an expanding queue.
  if (!_deferred_rx.empty()) {
    std::deque<DeferredRx> deferred;
    deferred.swap(_deferred_rx);
    for (const auto &f : deferred) deliver({f.frame.data(), f.frame.size(), f.dbm, f.channel});
  }
  auto drain_rx = [&](Ring &ring) {
    int uncommited = 0;
    for (int budget = 0; budget < 256; budget++) {
      if (!rx_ring_pop(ring, _rx_pkt_buf, false)) break;
      uncommited++;
      if (uncommited >= 32) {
        rx_ring_commit(ring);
        uncommited = 0;
      }
      RxFrame f;
      if (parse_rx_packet(_rx_pkt_buf.data(), _rx_pkt_buf.size(), f)) {
        deliver(f);
      } else {
        handle_rx_meta(_rx_pkt_buf.data(), _rx_pkt_buf.size());
      }
    }
    if (uncommited > 0) rx_ring_commit(ring);
  };
  /* Ring 0 usually carries async events and TX-free reports. */
  drain_rx(_rx_mcu);
  tx_ring_reclaim(_tx_data);
  /* MT7921/MT7922 delivers 802.11 frames on both RX ring 2 (data) and ring 4 (MCU/mgmt). */
  drain_rx(_rx_data);
  drain_rx(_rx_mcu_wa);
  return count;
}

void MtkDevice::close() {
  if (_dma_running && _proxy.is_open()) {
    if (_verbose) {
      log("close stats: tx_data cidx=" + std::to_string(rr(_tx_data.reg_base + 0x8)) +
          " didx=" + std::to_string(rr(_tx_data.reg_base + 0xc)) +
          " rx_data cidx=" + std::to_string(rr(_rx_data.reg_base + 0x8)) +
          " didx=" + std::to_string(rr(_rx_data.reg_base + 0xc)) +
          " rx_mcu didx=" + std::to_string(rr(_rx_mcu.reg_base + 0xc)) +
          " rx_wa didx=" + std::to_string(rr(_rx_mcu_wa.reg_base + 0xc)) +
          " mib_txtime=" + std::to_string(rr(0x820ed054u) & 0xffffffu) +
          " mib_rxtime=" + std::to_string(rr(0x820ed058u) & 0xffffffu) +
          " RFCR=" + hex32(rr(MT_WF_RFCR_BAND0)) +
          " MDP_DCR0=" + hex32(rr(MT_MDP_DCR0)));
    }
    std::string err;
    /* Stop WFDMA (including full WFDMA logic reset) and wait for TX/RX_DMA_BUSY == 0
     * BEFORE resetting WFSYS and BEFORE DmaPool32 frees any AWE physical pages (#72). */
    (void)dma_disable(true, err);
    if (wfsys_reset(err)) {
      /* Chip is cleanly halted and back in ROM-ready state; clear crash-only close_ops
       * so RpEvtFileCleanup does not re-assert WFSYS reset on normal handle close. */
      _proxy.set_close_ops({});
    }
  }
  _dma_running = false;
  _tx_pending_kick = false;
  _fw_loaded = false;
  _rx_path_set = false;
  _sta_iface_up = false;
  _deferred_rx.clear();
  _peers.fill(PeerEntry{});
  _pending_addba.fill(PendingAddba{});
  _pending_tx_addba.fill(PendingAddba{});
}

} // namespace mtk
