// SPDX-License-Identifier: GPL-2.0-only
/* The MT7902 as an AWDL listener and injector: the firmware's sniffer on a
 * fixed channel for receiving everything, one ad-hoc interface whose own
 * address the MAC acknowledges, and frames sent through the cut-through TX
 * path (TXD + TXP per token, released by the firmware's TX-free reports).
 * Follows mt7921 (mcu.c, mac.c, main.c) and mt76_connac (mac.c, mcu.c). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "mt7902.h"

using namespace mt;

namespace {

std::string fmt(const char *f, ...) {
  char line[512];
  va_list a;
  va_start(a, f);
  std::vsnprintf(line, sizeof(line), f, a);
  va_end(a);
  return line;
}

#pragma pack(push, 1)
struct ChanInfo { /* mt7921_mcu_set_chan_info */
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
};
static_assert(sizeof(ChanInfo) == 76, "chan info");

struct DevInfoReq { /* mt76_connac_mcu_uni_add_dev: dev_req */
  uint8_t omac_idx;
  uint8_t band_idx;
  uint16_t pad;
  uint16_t tag;
  uint16_t len;
  uint8_t active;
  uint8_t link_idx;
  uint8_t omac_addr[6];
};

struct BssBasicReq { /* basic_req with mt76_connac_bss_basic_tlv */
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
};
static_assert(sizeof(BssBasicReq) - 4 == 32, "bss basic tlv");

struct SnifferEnable {
  uint8_t band_idx;
  uint8_t pad[3];
  uint16_t tag;
  uint16_t len;
  uint8_t enable;
  uint8_t pad2[3];
};

struct SnifferConfig { /* mt7921_mcu_config_sniffer */
  uint8_t band_idx;
  uint8_t pad[3];
  uint16_t tag;
  uint16_t len;
  uint16_t aid;
  uint8_t ch_band;
  uint8_t bw;
  uint8_t control_ch;
  uint8_t sco;
  uint8_t center_ch;
  uint8_t center_ch2;
  uint8_t drop_err;
  uint8_t pad2[3];
};
static_assert(sizeof(SnifferConfig) - 4 == 16, "sniffer tlv");

struct RxFilterReq { /* mt7921_mcu_set_rxfilter */
  uint8_t rsv[4];
  uint8_t mode;
  uint8_t rsv2[3];
  uint32_t fif;
  uint32_t bit_map;
  uint8_t bit_op;
  uint8_t pad[51];
};

struct HwTxp { /* mt76_connac_hw_txp */
  uint16_t msdu_id[4];
  struct {
    uint32_t buf0;
    uint16_t len0;
    uint16_t len1;
    uint32_t buf1;
  } ptr[2];
};
static_assert(sizeof(HwTxp) == 32, "hw txp");
#pragma pack(pop)

constexpr uint8_t kWcidGlobal = 0; /* mt792x_init_wcid: management frames */
constexpr uint8_t kOmacIdx = 0; /* AWDL: own MAC 0, BSS 0 */

/* The 20/40/80 MHz block a 5 GHz channel belongs to. */
uint8_t center_channel(uint8_t ch, uint8_t width) {
  if (ch <= 14 || width == 20) return ch;
  if (width == 40) return static_cast<uint8_t>(((ch - 36) / 8) * 8 + 38);
  if (ch >= 149) return static_cast<uint8_t>(((ch - 149) / 16) * 16 + 155);
  return static_cast<uint8_t>(((ch - 36) / 16) * 16 + 42);
}

uint8_t cbw(uint8_t width) { return width == 80 ? 2 : width == 40 ? 1 : 0; }

size_t ieee80211_hdrlen(uint16_t fc) {
  const unsigned type = (fc >> 2) & 3, subtype = (fc >> 4) & 0xf;
  if (type == 1) { /* control */
    if (subtype == 0xc || subtype == 0xd) return 10; /* CTS, ACK */
    return 16;
  }
  size_t len = 24;
  if (type == 2) {
    if ((fc & 0x0300) == 0x0300) len += 6;
    if (subtype & 0x8) {
      len += 2;
      if (fc & 0x8000) len += 4;
    }
  } else if (type == 0 && (fc & 0x8000)) {
    len += 4;
  }
  return len;
}

} // namespace

uint16_t Mt7902::tx_rate_ofdm(unsigned mbps) {
  unsigned idx = 11;
  switch (mbps) {
  case 6: idx = 11; break;
  case 9: idx = 15; break;
  case 12: idx = 10; break;
  case 18: idx = 14; break;
  case 24: idx = 9; break;
  case 36: idx = 13; break;
  case 48: idx = 8; break;
  case 54: idx = 12; break;
  default: break;
  }
  return static_cast<uint16_t>(kPhyTypeOfdm << 6 | idx);
}

uint16_t Mt7902::tx_rate_cck(unsigned mbps_x2) {
  const unsigned idx = mbps_x2 >= 22 ? 3 : mbps_x2 >= 11 ? 2 : mbps_x2 >= 4 ? 1 : 0;
  return static_cast<uint16_t>(kPhyTypeCck << 6 | idx);
}

uint16_t Mt7902::tx_rate_ht(unsigned mcs) { return static_cast<uint16_t>(kPhyTypeHt << 6 | (mcs & 0x3f)); }

bool Mt7902::channel_switch(uint8_t ext_cmd_id, bool wait) {
  ChanInfo req{};
  req.control_ch = _channel;
  req.center_ch = center_channel(_channel, _width);
  req.bw = cbw(_width);
  req.tx_streams_num = 1;
  req.rx_streams = 1;
  req.switch_reason = 0; /* CH_SWITCH_NORMAL */
  req.channel_band = _channel > 14 ? 1 : 0;
  /* A move (set_channel) waits at most move_timeout_ms; bring-up as long as it takes. */
  const bool move = ext_cmd_id == kMcuExtChannelSwitch && _iface_up;
  return mcu_send(ext_cmd(ext_cmd_id), &req, sizeof(req), wait, nullptr, move ? move_timeout_ms : 3000, !move);
}

void Mt7902::set_timing() { /* mt792x_mac_set_timeing, coverage class 0, short slot */
  const bool is_2ghz = _channel <= 14;
  const uint32_t cck = 231u | 48u << 16, ofdm = 60u | 28u << 16;
  const uint32_t sifs = is_2ghz ? 10 : 16, slot = 9;
  set(0x820e3000 + 0x080, bit(8) | bit(9)); /* MT_ARB_SCR(0): TX/RX off while changing */
  wr(0x820e4000 + 0x090, cck);             /* MT_TMAC_CDTR(0) */
  wr(0x820e4000 + 0x094, ofdm);            /* MT_TMAC_ODTR(0) */
  wr(0x820e4000 + 0x0a4, 360u | 2u << 10 | sifs << 16 | slot << 24); /* MT_TMAC_ICR0(0) */
  rmw(0x820e2000 + 0x084, 0x3fff, 0x49);   /* MT_AGG_ACR0(0) CF-End at OFDM 24M */
  clear(0x820e3000 + 0x080, bit(8) | bit(9));
}

bool Mt7902::sniffer(bool enable) {
  SnifferEnable req{};
  req.tag = 0;
  req.len = sizeof(req) - 4;
  req.enable = enable;
  return mcu_send(uni_cmd(kMcuUniSniffer), &req, sizeof(req), true);
}

bool Mt7902::sniffer_config(bool wait) {
  SnifferConfig req{};
  req.tag = 1;
  req.len = sizeof(req) - 4;
  req.ch_band = _channel > 14 ? 2 : 1;
  req.bw = _width == 80 ? 1 : 0;
  req.control_ch = _channel;
  req.center_ch = center_channel(_channel, _width);
  req.sco = req.control_ch < req.center_ch ? 1 : req.control_ch > req.center_ch ? 3 : 0;
  req.drop_err = 1;
  return mcu_send(uni_cmd(kMcuUniSniffer), &req, sizeof(req), wait);
}

bool Mt7902::add_interface(bool enable) { return add_omac(kOmacIdx, _own, _bssid, enable); }

/* mt76_connac_mcu_uni_add_dev for own MAC `omac` with BSS `omac`. Both kinds
 * the radio adds are monitor-like (the sniffer receives, the host runs the
 * protocol); the own address is what makes the MAC acknowledge. */
bool Mt7902::add_omac(uint8_t omac, const uint8_t addr[6], const uint8_t bssid[6], bool enable) {
  DevInfoReq dev{};
  dev.omac_idx = omac;
  dev.tag = 0; /* DEV_INFO_ACTIVE */
  dev.len = sizeof(dev) - 4;
  dev.active = enable;
  std::memcpy(dev.omac_addr, addr, 6);
  BssBasicReq bss{};
  bss.bss_idx = omac;
  bss.tag = 0; /* UNI_BSS_INFO_BASIC */
  bss.len = sizeof(bss) - 4;
  bss.active = enable;
  bss.omac_idx = omac;
  bss.hw_bss_idx = omac;
  bss.conn_type = kConnectionInfraAp; /* as mt76 for a monitor interface */
  bss.conn_state = 1;
  std::memcpy(bss.bssid, bssid, 6);
  bss.bmc_tx_wlan_idx = kWcidGlobal;
  bss.sta_idx = kWcidGlobal;
  bss.bcn_interval = 100;
  bss.dtim_period = 1;
  bss.phymode = 0x3f; /* A, B, G, GN, AN, AC */
  if (enable)
    return mcu_send(uni_cmd(kMcuUniDevInfoUpdate), &dev, sizeof(dev), true) &&
           mcu_send(uni_cmd(kMcuUniBssInfoUpdate), &bss, sizeof(bss), true);
  return mcu_send(uni_cmd(kMcuUniBssInfoUpdate), &bss, sizeof(bss), true) &&
         mcu_send(uni_cmd(kMcuUniDevInfoUpdate), &dev, sizeof(dev), true);
}

bool Mt7902::radio_start(uint8_t channel, uint8_t width_mhz, const std::string &country,
                         std::string &error) {
  _channel = channel;
  _width = channel > 14 ? width_mhz : 20;
  std::memcpy(_own, _mac, 6);

  /* mt76_connac_mcu_set_deep_sleep(false), as mt7921 does for the MT7902
   * (no runtime PM) and in monitor mode: without it the firmware's power
   * management asserts (pm_cmm.c) under a heavy A-MPDU receive. */
  {
    uint8_t cfg[8 + 320] = {}; /* mt76_connac_config: id, type, resp_type, data_size, resv, data */
    const char keep[] = "KeepFullPwr 1";
    std::memcpy(cfg + 8, keep, sizeof(keep));
    if (!mcu_send(ce_cmd(0xca /* CHIP_CONFIG */), cfg, sizeof(cfg), false)) log("mt7902: KeepFullPwr not sent");
  }
  /* mt7921_run_firmware: firmware log to the host (shows up as events) */
  const uint8_t fwlog[4] = {1, 0, 0, 0};
  mcu_send(ce_cmd(kMcuCeCmdFwlog2Host), fwlog, sizeof(fwlog), false);
  /* mt7921_mcu_set_eeprom: calibration from efuse */
  const uint8_t ee[4] = {0 /* EE_MODE_EFUSE */, 1 /* EE_FORMAT_WHOLE */, 0, 0};
  if (!mcu_send(ext_cmd(kMcuExtEfuseBufferMode), ee, sizeof(ee), true)) {
    error = "EFUSE_BUFFER_MODE failed";
    return false;
  }
  /* mt7921_mac_init, without RX header translation: we want 802.11 frames. */
  rmw(0x820cd000 + 0x004, 0x1fffu << 3, 1536u << 3); /* MT_MDP_DCR1 MAX_RX_LEN */
  clear(0x820cd000 + 0x000, bit(19));                 /* MT_MDP_DCR0: no RX_HDR_TRANS */

  /* __mt7921_start */
  const uint8_t mac_on[4] = {1, 0, 0, 0};
  if (!mcu_send(ext_cmd(kMcuExtMacInitCtrl), mac_on, sizeof(mac_on), true)) {
    error = "MAC_INIT_CTRL failed";
    return false;
  }
  { /* mt76_connac_mcu_set_channel_domain */
    static const uint8_t ch2[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    static const uint8_t ch5[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120,
                                  124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
    /* alpha2[4], bw_2g, bw_5g, bw_6g, pad, n_2ch, n_5ch, n_6ch, pad2; then
     * per channel: hw_value (le16), pad (le16), flags (le32). */
    std::vector<uint8_t> req(12);
    for (size_t i = 0; i < 2 && i < country.size(); i++) req[i] = static_cast<uint8_t>(country[i]);
    req[4] = 0; /* 2.4 GHz: 20/40 MHz */
    req[5] = 3; /* 5 GHz: up to 160 MHz */
    req[6] = 3;
    req[8] = sizeof(ch2);
    req[9] = sizeof(ch5);
    for (uint8_t ch : ch2) req.insert(req.end(), {ch, 0, 0, 0, 0, 0, 0, 0});
    for (uint8_t ch : ch5) req.insert(req.end(), {ch, 0, 0, 0, 0, 0, 0, 0});
    if (!mcu_send(ce_cmd(kMcuCeSetChanDomain), req.data(), req.size(), false))
      log("mt7902: SET_CHAN_DOMAIN not sent");
  }
  if (!channel_switch(kMcuExtSetRxPath)) {
    error = "SET_RX_PATH failed";
    return false;
  }
  _iface_up = add_interface(true);
  if (!_iface_up) log("mt7902: adding the interface failed; listening without one");
  const uint32_t rfcr_before = rx_filter_reg();
  RxFilterReq f{};
  f.mode = 1;
  f.fif = kFilterEnable | kFilterOtherBss | kFilterControl;
  mcu_send(ce_cmd(kMcuCeSetRxFilter), &f, sizeof(f), false);
  if (!sniffer(true) || !sniffer_config()) log("mt7902: the firmware did not confirm sniffer mode");
  if (!channel_switch(kMcuExtChannelSwitch)) {
    error = "CHANNEL_SWITCH failed";
    return false;
  }
  set_timing();
  poll_events();
  log(fmt("mt7902: radio on channel %u/%u MHz (center %u), RFCR %08x -> %08x", _channel, _width,
          center_channel(_channel, _width), rfcr_before, rx_filter_reg()));
  return !_proxy.failed();
}

bool Mt7902::set_channel(uint8_t channel, uint8_t width_mhz) {
  _channel = channel;
  _width = channel > 14 ? width_mhz : 20;
  using C = std::chrono::steady_clock;
  const auto us = [](C::time_point a, C::time_point b) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
  };
  const auto t0 = C::now();
  bool ok = channel_switch(kMcuExtChannelSwitch, move_wait);
  const auto t1 = C::now();
  if (move_sniffer) ok = sniffer_config(move_wait) && ok;
  const auto t2 = C::now();
  set_timing();
  const auto t3 = C::now();
  last_move_us[0] = us(t0, t1);
  last_move_us[1] = us(t1, t2);
  last_move_us[2] = us(t2, t3);
  return ok;
}

bool Mt7902::set_identity(const uint8_t own[6], const uint8_t bssid[6]) {
  if (_iface_up) add_interface(false);
  std::memcpy(_own, own, 6);
  std::memcpy(_bssid, bssid, 6);
  _iface_up = add_interface(true);
  /* Re-adding the interface may reset the sniffer: put it back. */
  return _iface_up && sniffer(true) && sniffer_config();
}

bool Mt7902::set_station_identity(const uint8_t addr[6], const uint8_t bssid[6]) {
  if (_sta_iface_up) add_omac(kStationOmac, _sta_addr, _sta_bssid, false);
  std::memcpy(_sta_addr, addr, 6);
  std::memcpy(_sta_bssid, bssid, 6);
  _sta_iface_up = add_omac(kStationOmac, _sta_addr, _sta_bssid, true);
  return _sta_iface_up && sniffer(true) && sniffer_config();
}

void Mt7902::clear_station_identity() {
  if (!_sta_iface_up) return;
  add_omac(kStationOmac, _sta_addr, _sta_bssid, false);
  _sta_iface_up = false;
  sniffer(true);
  sniffer_config();
}

int Mt7902::transmit(const uint8_t *frame, size_t len, uint16_t rate, uint8_t omac) {
  if (len < 10) return -1;
  const uint16_t fc = static_cast<uint16_t>(frame[0] | frame[1] << 8);
  const uint32_t type = (fc >> 2) & 3, subtype = (fc >> 4) & 0xf;
  const bool data = type == 2;
  const bool group = (frame[4] & 1) != 0;
  const size_t hdrlen = ieee80211_hdrlen(fc);
  uint32_t tid = 0;
  if (data && (subtype & 8) && len >= hdrlen) tid = frame[hdrlen - 2] & 0x7;

  uint32_t txd[8] = {};
  uint32_t q = data && !group ? kLmacAc01 : kLmacAltx0;
  if (force_altx >= 0) q = force_altx ? kLmacAltx0 : kLmacAc01;
  txd[0] = static_cast<uint32_t>(len + kTxdSize) | kTxTypeCt << 23 | q << 25;
  txd[1] = kTxd1LongFormat | kWcidGlobal | static_cast<uint32_t>(omac) << 24 |
           kHdrFormat80211 << 16 | static_cast<uint32_t>(hdrlen / 2) << 11 | tid << 20;
  txd[2] = type << 4 | subtype | (group ? kTxd2Multicast : 0) | kTxd2FixRate | kTxd2HtcVld;
  const bool no_ack = force_no_ack >= 0 ? force_no_ack != 0 : group;
  txd[3] = 15u << 11 | kTxd3BaDisable | (no_ack ? kTxd3NoAck : 0);
  if (type != 1 && len >= 24) {
    const uint16_t sc = static_cast<uint16_t>(frame[22] | frame[23] << 8);
    txd[3] |= kTxd3SnValid | static_cast<uint32_t>(sc >> 4) << 16;
  }
  txd[6] = kTxd6FixedBw | static_cast<uint32_t>(rate) << 16;
  txd[7] = type << 20 | subtype << 16;
  return queue_frame(frame, len, txd);
}

int Mt7902::transmit_sta(const uint8_t *frame, size_t len, uint8_t wcid, uint8_t omac) {
  /* mt76_connac2_mac_write_txwi for a station's data: no fixed rate (the
   * firmware's rate control), Block Ack allowed, BE queue. */
  if (len < 26) return -1;
  const uint16_t fc = static_cast<uint16_t>(frame[0] | frame[1] << 8);
  const uint32_t type = (fc >> 2) & 3, subtype = (fc >> 4) & 0xf;
  const size_t hdrlen = ieee80211_hdrlen(fc);
  if (type != 2 || !(subtype & 8) || len < hdrlen) return -1;
  const uint32_t tid = frame[hdrlen - 2] & 0x7;
  uint32_t txd[8] = {};
  txd[0] = static_cast<uint32_t>(len + kTxdSize) | kTxTypeCt << 23 | kLmacAc01 << 25;
  txd[1] = kTxd1LongFormat | wcid | static_cast<uint32_t>(omac) << 24 | kHdrFormat80211 << 16 |
           static_cast<uint32_t>(hdrlen / 2) << 11 | tid << 20;
  txd[2] = type << 4 | subtype;
  const uint16_t sc = static_cast<uint16_t>(frame[22] | frame[23] << 8);
  txd[3] = 15u << 11 | kTxd3SnValid | static_cast<uint32_t>(sc >> 4) << 16;
  txd[7] = type << 20 | subtype << 16;
  return queue_frame(frame, len, txd);
}

int Mt7902::queue_frame(const uint8_t *frame, size_t len, uint32_t txd[8]) {
  if (len > DmaPool::kPageBytes - kTxFrameOffset || _tx_free_slots.empty()) return -1;
  const uint16_t token = _tx_free_slots.back();
  const DmaBuf &page = _tx_slots[token].page;
  std::memcpy(page.va + kTxFrameOffset, frame, len);
  HwTxp txp{};
  txp.msdu_id[0] = static_cast<uint16_t>(token | kMsduIdValid);
  txp.ptr[0].buf0 = static_cast<uint32_t>(page.pa + kTxFrameOffset);
  txp.ptr[0].len0 = static_cast<uint16_t>((len & 0xfff) | kTxdLenLast);
  std::memcpy(page.va, txd, 8 * sizeof(uint32_t));
  std::memcpy(page.va + kTxdSize, &txp, sizeof(txp));

  /* One descriptor: the TXD+TXP; the card fetches the frame itself. */
  TxRing &r = _tx_data;
  const unsigned done = rr(0xd4300 + r.hw * 0x10 + 0xc) % r.n;
  if ((r.head + 1) % r.n == done) return -1;
  volatile Desc *d = reinterpret_cast<volatile Desc *>(r.desc.va) + r.head;
  d->buf0 = static_cast<uint32_t>(page.pa);
  d->buf1 = 0;
  d->info = 0;
  d->ctrl = static_cast<uint32_t>(kTxdSize + sizeof(HwTxp)) << kDescSdLen0Shift | kDescLastSec0;
  r.head = (r.head + 1) % r.n;
  _tx_free_slots.pop_back();
  MemoryBarrier();
  wr(0xd4300 + r.hw * 0x10 + 0x8, r.head);
  return _proxy.failed() ? -1 : token;
}

void Mt7902::handle_tx_free(const uint8_t *data, size_t len) { /* mt7921_mac_tx_free */
  if (len < 8) return;
  uint32_t rxd0;
  std::memcpy(&rxd0, data, 4);
  unsigned count = (rxd0 >> 16) & 0x3ff;
  const uint32_t *info = reinterpret_cast<const uint32_t *>(data + 8);
  const size_t entries = (len - 8) / 4;
  for (unsigned i = 0; i < count && i < entries; i++) {
    const uint32_t v = info[i];
    if (v & bit(31)) { /* a new wcid pair: does not count */
      count++;
      continue;
    }
    const uint16_t msdu = static_cast<uint16_t>((v >> 16) & 0x7fff);
    const unsigned stat = (v >> 13) & 3, tries = v & 0x1fff;
    if (msdu < _tx_slots.size()) {
      bool already = false;
      for (uint16_t s : _tx_free_slots) already |= s == msdu;
      if (!already) _tx_free_slots.push_back(msdu);
    }
    if (_on_tx_status) _on_tx_status(msdu, stat == 0, tries);
  }
}

void Mt7902::handle_rx(const std::vector<uint8_t> &buf) { /* mt7921_queue_rx_skb + fill_rx */
  if (buf.size() < 8) return;
  const uint32_t *rxd = reinterpret_cast<const uint32_t *>(buf.data());
  const uint32_t type = rxd[0] >> kRxd0PktTypeShift;
  rx_types[type & 31]++;
  if (type == kPktTypeTxrxNotify) { /* as short as 16 bytes */
    handle_tx_free(buf.data(), buf.size());
    return;
  }
  if (buf.size() < 24) return;
  if (type == kPktTypeRxEvent || type == kPktTypeTxs) {
    if (type == kPktTypeRxEvent && _on_event) _on_event(buf.data(), buf.size());
    return;
  }
  if (type != kPktTypeNormal && type != kPktTypeNormalMcu) return;
  const uint32_t rxd1 = rxd[1], rxd2 = rxd[2], rxd3 = rxd[3], rxd4 = rxd[4];
  if ((rxd1 & (kRxd1BandIdx | kRxd1FcsErr)) || (rxd2 & (kRxd2AmsduErr | kRxd2MaxLenError | kRxd2HdrTrans)))
    return;
  size_t dw = 6;
  uint32_t rcpi = 0;
  if (rxd1 & kRxd1Group4) dw += 4;
  if (rxd1 & kRxd1Group1) dw += 4;
  if (rxd1 & kRxd1Group2) dw += 2;
  if (rxd1 & kRxd1Group3) {
    if ((dw + 2) * 4 > buf.size()) return;
    rcpi = rxd[dw + 1];
    dw += 2;
    if (rxd1 & kRxd1Group5) {
      dw += 6;
      if ((dw + 1) * 4 > buf.size()) return;
      rcpi = rxd[dw];
      dw += 12;
    }
  }
  const size_t gap = dw * 4 + 2 * ((rxd2 >> kRxd2HdrOffsetShift) & 3);
  const size_t total = std::min<size_t>(rxd[0] & 0xffff, buf.size());
  if (gap + 10 > total || (rxd4 & kRxd4PayloadFormatMask)) return;
  RxFrame f;
  f.data = buf.data() + gap;
  f.len = total - gap;
  f.channel = static_cast<uint8_t>(rxd3 >> kRxd3ChFreqShift);
  const int r = static_cast<int>(rcpi & 0xff);
  f.dbm = (rxd1 & kRxd1Group3) && r ? static_cast<int8_t>((r - 220) / 2) : int8_t{-127};
  if (_on_rx) _on_rx(f);
}

unsigned Mt7902::poll() {
  unsigned n = 0;
  while (n < 64 && rx_take(_rx_data, _rx_scratch)) {
    handle_rx(_rx_scratch);
    n++;
  }
  while (rx_take(_rx_mcu, _rx_scratch)) {
    handle_rx(_rx_scratch);
    n++;
  }
  return n;
}

std::string Mt7902::tx_ring_state() {
  const TxRing &r = _tx_data;
  return fmt("data TX ring cpu %u dma %u (head %u); event ring cpu %u dma %u; free TX slots %zu; GLO_CFG %08x",
             rr(0xd4300 + r.hw * 0x10 + 0x8), rr(0xd4300 + r.hw * 0x10 + 0xc), r.head,
             rr(0xd4500 + _rx_mcu.hw * 0x10 + 0x8), rr(0xd4500 + _rx_mcu.hw * 0x10 + 0xc),
             _tx_free_slots.size(), rr(kGloCfg));
}
