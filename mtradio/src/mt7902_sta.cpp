// SPDX-License-Identifier: GPL-2.0-only
/* Peer stations on the MT7902: a STA record and WTBL entry per AWDL peer,
 * so the firmware picks the rate (its rate control, VHT included) and the
 * MAC keeps Block Ack sessions (A-MPDU both ways). Follows
 * mt76_connac_mcu_sta_cmd, mt76_connac_mcu_sta_tlv, mt76_connac_mcu_wtbl_*
 * and mt76_connac_mcu_sta_ba (mt76_connac_mcu.c) for an ad-hoc interface. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "mt7902.h"

using namespace mt;

namespace {

/* STA_REC_* and WTBL_* tags (mt76_connac_mcu.h) */
constexpr uint16_t kStaRecBasic = 0, kStaRecRa = 1, kStaRecBa = 6, kStaRecState = 7, kStaRecHt = 9,
                   kStaRecVht = 10, kStaRecWtbl = 13, kStaRecPhy = 0x15;
constexpr uint16_t kWtblGeneric = 0, kWtblRx = 1, kWtblHt = 2, kWtblVht = 3, kWtblHdrTrans = 6, kWtblBa = 8,
                   kWtblSmps = 13;
constexpr uint8_t kWtblResetAndSet = 1, kWtblSet = 2;
constexpr uint8_t kConnStateDisconnect = 0, kConnStatePortSecure = 2;
constexpr uint8_t kBaTypeOriginator = 1, kBaTypeRecipient = 2;
constexpr uint8_t kStaStateAssoc = 2;
constexpr uint8_t kMcuUniStaRecUpdate = 0x03;

/* A STA_REC_UPDATE message: sta_req_hdr, then TLVs; one of them may be
 * STA_REC_WTBL holding a wtbl_req_hdr and nested TLVs. Offsets, not
 * pointers: the buffer grows. */
class StaReq {
public:
  StaReq(uint8_t wcid, uint8_t omac) {
    _b = {omac /* bss_idx */, wcid, 0, 0 /* tlv_num */, 1 /* is_tlv_append */, omac /* muar */, 0 /* wlan_idx_hi */, 0};
  }
  uint8_t *add(uint16_t tag, uint16_t len) {
    const size_t at = put(tag, len);
    bump(2);
    return _b.data() + at;
  }
  void wtbl(uint8_t op, uint8_t wcid) {
    _wtbl = put(kStaRecWtbl, 4);
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
    uint16_t n;
    std::memcpy(&n, &_b[at], 2);
    n++;
    std::memcpy(&_b[at], &n, 2);
  }
  void grow_wtbl(uint16_t by) {
    uint16_t len;
    std::memcpy(&len, &_b[_wtbl + 2], 2);
    len = static_cast<uint16_t>(len + by);
    std::memcpy(&_b[_wtbl + 2], &len, 2);
  }
  std::vector<uint8_t> _b;
  size_t _wtbl = 0, _wtbl_hdr = 0;
};

template <class T> void put(uint8_t *at, size_t off, T v) { std::memcpy(at + off, &v, sizeof(v)); }

} // namespace

bool Mt7902::sta_add(uint8_t wcid, const uint8_t peer[6], const PeerCaps &caps, uint8_t omac) {
  StaReq r(wcid, omac);
  { /* mt76_connac_mcu_sta_basic_tlv, ad-hoc */
    uint8_t *t = r.add(kStaRecBasic, 20);
    put<uint32_t>(t, 4, kConnectionInfraSta);
    t[8] = kConnStatePortSecure;
    t[9] = 1; /* qos */
    std::memcpy(t + 12, peer, 6);
    put<uint16_t>(t, 18, 0x3); /* EXTRA_INFO_VER | EXTRA_INFO_NEW */
  }
  { /* mt76_connac_mcu_sta_tlv */
    uint8_t *ht = r.add(kStaRecHt, 8);
    put<uint16_t>(ht, 4, caps.ht_cap);
    if (caps.vht) {
      uint8_t *vht = r.add(kStaRecVht, 16);
      put<uint32_t>(vht, 4, caps.vht_cap);
      put<uint16_t>(vht, 8, caps.vht_mcs);
      put<uint16_t>(vht, 10, caps.vht_mcs);
    }
    uint8_t *phy = r.add(kStaRecPhy, 12);
    put<uint16_t>(phy, 4, _channel > 14 ? 0x15 : 0x0f); /* basic: 6/12/24 Mb/s, or CCK */
    phy[6] = static_cast<uint8_t>(_channel > 14 ? (bit(3) | bit(4) | (caps.vht ? bit(5) : 0))
                                                : (bit(0) | bit(1) | bit(4)));
    phy[7] = static_cast<uint8_t>((caps.ampdu_params & 0x3) | (caps.ampdu_params & 0x1c));
    uint8_t *ra = r.add(kStaRecRa, 16);
    put<uint16_t>(ra, 4, static_cast<uint16_t>(_channel > 14 ? 0xff << 6 : (0xff << 6) | 0xf));
    ra[6] = caps.rx_mcs[0];
    ra[7] = caps.rx_mcs[1];
    uint8_t *st = r.add(kStaRecState, 12);
    st[8] = kStaStateAssoc;
    if (caps.vht || omac) st[9] = static_cast<uint8_t>(caps.bw | ((caps.nss - 1) << 4));
  }
  r.wtbl(kWtblResetAndSet, wcid);
  { /* mt76_connac_mcu_wtbl_generic_tlv, _hdr_trans_tlv, _ht_tlv */
    uint8_t *g = r.wadd(kWtblGeneric, 20);
    std::memcpy(g + 4, peer, 6);
    g[10] = omac; /* muar_idx */
    g[13] = 1; /* qos */
    uint8_t *rx = r.wadd(kWtblRx, 12);
    rx[5] = 0; /* rca1: 0 under an AP interface (mt76_connac_mcu_wtbl_generic_tlv) */
    rx[6] = 1; /* rca2 */
    rx[7] = 1; /* rv */
    uint8_t *h = r.wadd(kWtblHdrTrans, 8);
    h[5] = 1; /* from_ds, as mt76 for a non-station interface */
    h[6] = 1; /* no_rx_trans: we want 802.11 frames */
    uint8_t af = caps.ampdu_params & 0x3;
    uint8_t *ht = r.wadd(kWtblHt, 12);
    ht[4] = 1;
    ht[5] = (caps.ht_cap & 0x1) ? 1 : 0; /* LDPC */
    ht[7] = static_cast<uint8_t>((caps.ampdu_params >> 2) & 0x7);
    if (caps.vht) {
      uint8_t *vht = r.wadd(kWtblVht, 12);
      vht[4] = (caps.vht_cap & bit(4)) ? 1 : 0; /* RXLDPC */
      vht[6] = 1;
      af = std::max<uint8_t>(af, static_cast<uint8_t>((caps.vht_cap >> 23) & 0x7));
    }
    ht[6] = af;
    r.wadd(kWtblSmps, 8);
  }
  const auto &b = r.bytes();
  const bool ok = mcu_send(uni_cmd(kMcuUniStaRecUpdate), b.data(), b.size(), true, nullptr, sta_cmd_timeout_ms);
  log(std::string("mt7902: station ") + std::to_string(wcid) + (ok ? " added" : " not added"));
  return ok;
}

bool Mt7902::sta_remove(uint8_t wcid, const uint8_t peer[6], uint8_t omac) {
  StaReq r(wcid, omac);
  uint8_t *t = r.add(kStaRecBasic, 20);
  put<uint32_t>(t, 4, kConnectionInfraSta);
  t[8] = kConnStateDisconnect;
  std::memcpy(t + 12, peer, 6);
  put<uint16_t>(t, 18, 0x1);
  r.wtbl(kWtblResetAndSet, wcid);
  const auto &b = r.bytes();
  return mcu_send(uni_cmd(kMcuUniStaRecUpdate), b.data(), b.size(), true, nullptr, sta_cmd_timeout_ms);
}

bool Mt7902::sta_ba(uint8_t wcid, const uint8_t peer[6], uint8_t tid, uint16_t ssn, uint16_t win, bool tx,
                    bool enable, uint8_t omac) {
  { /* mt76_connac_mcu_wtbl_ba_tlv (connac2) */
    StaReq r(wcid, omac);
    r.wtbl(kWtblSet, wcid);
    uint8_t *ba = r.wadd(kWtblBa, 28);
    ba[4] = tid;
    if (tx) {
      ba[5] = kBaTypeOriginator;
      put<uint16_t>(ba, 8, enable ? ssn : 0);
      ba[10] = enable;
    } else {
      ba[5] = kBaTypeRecipient;
      std::memcpy(ba + 14, peer, 6);
      ba[20] = tid; /* rst_ba_tid */
      ba[21] = 0;   /* RST_BA_MAC_TID_MATCH */
      ba[22] = 1;   /* rst_ba_sb */
    }
    put<uint16_t>(ba, 12, enable ? win : 0);
    const auto &b = r.bytes();
    if (!mcu_send(uni_cmd(kMcuUniStaRecUpdate), b.data(), b.size(), true, nullptr, sta_cmd_timeout_ms)) return false;
  }
  { /* mt76_connac_mcu_sta_ba_tlv */
    StaReq r(wcid, omac);
    uint8_t *ba = r.add(kStaRecBa, 12);
    ba[4] = tid;
    ba[5] = tx ? kBaTypeOriginator : kBaTypeRecipient;
    ba[6] = 0; /* no A-MSDU inside */
    ba[7] = static_cast<uint8_t>(enable ? 1u << tid : 0);
    put<uint16_t>(ba, 8, ssn);
    put<uint16_t>(ba, 10, win);
    const auto &b = r.bytes();
    return mcu_send(uni_cmd(kMcuUniStaRecUpdate), b.data(), b.size(), true, nullptr, sta_cmd_timeout_ms);
  }
}
