// SPDX-License-Identifier: GPL-2.0-only
#include "mt_station.h"

#include <cstdio>
#include <cstring>

namespace {

/* Fixed data rates for the AP, slowest first (as the AWDL ladder). */
struct Level {
  const char *name;
  uint16_t rate;
};
const Level kLevels[] = {{"OFDM24", Mt7902::tx_rate_ofdm(24)}, {"MCS2", Mt7902::tx_rate_ht(2)},
                         {"MCS4", Mt7902::tx_rate_ht(4)},      {"MCS5", Mt7902::tx_rate_ht(5)},
                         {"MCS7", Mt7902::tx_rate_ht(7)}};
constexpr unsigned kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);
constexpr size_t kHeldMax = 4096; /* a quarter second away at full speed */
/* How long a reorder buffer waits on the AP's channel for a gap to fill
 * (mac80211: 100 ms; the AP retries within a few). */
constexpr auto kReorderWait = std::chrono::milliseconds(20);
constexpr unsigned kReplayNone = 99, kReplayGroup = 17;

uint16_t seq_of(const uint8_t *f) { return static_cast<uint16_t>((f[22] | f[23] << 8) >> 4); }
/* How far `a` is after `b` in sequence number space. */
uint16_t seq_after(uint16_t a, uint16_t b) { return static_cast<uint16_t>((a - b) & 0xfff); }

} // namespace

bool MtStation::sta_join(const StaParams &p) {
  if (_fw_sta) _dev.sta_remove(Mt7902::kApWcid, _params.bssid, Mt7902::kStationOmac);
  _fw_sta = false;
  _params = p;
  _active = true;
  _associated = false;
  _aid = 0;
  _ptk.clear();
  for (auto &k : _gtk) k.clear();
  _tx_pn = 0;
  std::memset(_rx_pn, 0, sizeof(_rx_pn));
  std::memset(_rx_group_pn, 0, sizeof(_rx_group_pn));
  _last_rx_seq[0] = _last_rx_seq[1] = -1;
  _held.clear();
  for (auto &q : _reorder) q = Reorder{};
  _ready.clear();
  _ba_broken = false;
  if (!_dev.set_station_identity(p.addr, p.bssid)) {
    _log("station: the card refused the station's own address");
    return false;
  }
  return true;
}

bool MtStation::sta_associated(uint16_t aid, uint8_t) {
  _aid = aid;
  _associated = true;
  /* The AP's STA record, from its beacon's capabilities: the firmware's rate
   * control then sends at what both ends can do (HT/VHT, up to the
   * association's width), and the AP's Block Ack sessions can be set up.
   * Ours towards it (A-MPDUs we send) are not asked for: with both
   * directions aggregated the AP dropped us within seconds (reason 1), and
   * sending is mostly TCP acknowledgements here. */
  if (_params.ht) {
    Mt7902::PeerCaps c;
    c.ht_cap = _params.ap_ht_cap;
    c.ampdu_params = _params.ap_ampdu;
    c.rx_mcs[0] = 0xff;
    c.rx_mcs[1] = _params.mimo ? 0xff : 0;
    c.vht = _params.vht;
    c.vht_cap = _params.ap_vht_cap;
    c.vht_mcs = _params.ap_vht_mcs;
    c.bw = static_cast<uint8_t>(_params.width >= 80 ? 2 : _params.width >= 40 ? 1 : 0);
    c.nss = static_cast<uint8_t>(_params.mimo ? 2 : 1);
    _fw_sta = _dev.sta_add(Mt7902::kApWcid, _params.bssid, c, Mt7902::kStationOmac);
    if (!_fw_sta) _log("station: the card refused the access point's STA record; fixed rates");
  }
  return true;
}

bool MtStation::sta_set_key(bool group, unsigned key_index, const uint8_t key[16]) {
  if (group) {
    const unsigned id = key_index & 3;
    _rx_group_pn[id] = 0;
    return _gtk[id].set(key);
  }
  _tx_pn = 0;
  std::memset(_rx_pn, 0, sizeof(_rx_pn));
  return _ptk.set(key);
}

bool MtStation::sta_add_rx_ba(unsigned tid, uint16_t ssn, uint16_t window) {
  if (!_fw_sta || _ba_broken || tid >= 8) return false;
  Reorder &q = _reorder[tid];
  /* A new request for a TID replaces its session: the firmware's goes first. */
  if (q.on) {
    release(q, static_cast<uint16_t>((q.head + q.window) & 0xfff));
    q.on = false;
    _dev.sta_ba(Mt7902::kApWcid, _params.bssid, static_cast<uint8_t>(tid), 0, 0, false, false,
                Mt7902::kStationOmac);
  }
  if (!_dev.sta_ba(Mt7902::kApWcid, _params.bssid, static_cast<uint8_t>(tid), ssn, window, false, true,
                   Mt7902::kStationOmac)) {
    /* Each unanswered command holds the radio up; the AP sends single frames from now on. */
    _ba_broken = true;
    _log("station: the card did not take a Block Ack session; declining them from now on");
    return false;
  }
  q = Reorder{};
  q.on = true;
  q.head = static_cast<uint16_t>(ssn & 0xfff);
  q.window = static_cast<uint16_t>(window && window < 64 ? window : 64);
  _stats.rx_ba++;
  return true;
}

bool MtStation::sta_remove_rx_ba(unsigned tid) {
  if (tid >= 8) return true;
  Reorder &q = _reorder[tid];
  if (q.on) {
    release(q, static_cast<uint16_t>((q.head + q.window) & 0xfff));
    q = Reorder{};
    if (_fw_sta)
      _dev.sta_ba(Mt7902::kApWcid, _params.bssid, static_cast<uint8_t>(tid), 0, 0, false, false,
                  Mt7902::kStationOmac);
  }
  return true;
}

void MtStation::sta_leave() {
  if (_fw_sta) _dev.sta_remove(Mt7902::kApWcid, _params.bssid, Mt7902::kStationOmac);
  if (_active) _dev.clear_station_identity();
  _active = _associated = _fw_sta = false;
  _ptk.clear();
  for (auto &k : _gtk) k.clear();
  _held.clear();
  _inflight.clear();
  _null_token = -1;
  for (auto &q : _reorder) q = Reorder{};
  _ready.clear();
}

uint16_t MtStation::data_rate() const { return kLevels[_level].rate; }

std::string MtStation::rate_name() const { return _fw_sta ? "firmware's" : kLevels[_level].name; }

int MtStation::sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt) {
  if (!_active || len < 24) return -1;
  std::vector<uint8_t> f(frame, frame + len);
  const bool unicast_data = (f[0] & 0x0c) == 0x08 && !(f[4] & 1);
  /* Data at the firmware's rate goes through the AP's STA record, which
   * takes QoS data: TID 0, best effort. */
  const bool fw = _fw_sta && rate.kind == TxRate::Auto && f[0] == 0x08 && (f[1] & 0x03) == 0x01 && unicast_data;
  if (fw) {
    f[0] = 0x88;
    const uint8_t qos[2] = {0, 0};
    f.insert(f.begin() + 24, qos, qos + 2);
    _qos_seq = static_cast<uint16_t>((_qos_seq + 1) & 0xfff);
    f[22] = static_cast<uint8_t>((f[22] & 0x0f) | (_qos_seq << 4));
    f[23] = static_cast<uint8_t>(_qos_seq >> 4);
  } else {
    /* One sequence number space for management and plain data: WifiStation
     * leaves data frames at 0 (Intel's firmware numbers them). */
    _tx_seq = static_cast<uint16_t>((_tx_seq + 1) & 0xfff);
    f[22] = static_cast<uint8_t>((f[22] & 0x0f) | (_tx_seq << 4));
    f[23] = static_cast<uint8_t>(_tx_seq >> 4);
  }
  uint16_t r = 0;
  if (!fw) {
    if (rate.kind == TxRate::Legacy) {
      const unsigned x2 = rate.mbps_x2;
      const bool cck = _params.channel <= 14 && (x2 == 2 || x2 == 4 || x2 == 11 || x2 == 22);
      r = cck ? Mt7902::tx_rate_cck(x2) : Mt7902::tx_rate_ofdm(x2 / 2);
    } else {
      r = data_rate();
    }
  }
  if (encrypt) {
    if (!_ptk.valid()) return -1;
    std::vector<uint8_t> sealed;
    f[1] &= static_cast<uint8_t>(~0x40);
    if (!_ptk.seal(f.data(), f.size(), ++_tx_pn, 0, sealed)) return -1;
    f.swap(sealed);
  }
  if (!_here) {
    if (_held.size() >= kHeldMax) {
      _held.pop_front();
      _stats.tx_dropped++;
    }
    _held.push_back({std::move(f), r, unicast_data && !fw, fw});
    _stats.tx_held++;
  } else if (!send_now(f, r, unicast_data && !fw, fw)) {
    return -1;
  }
  return 0;
}

bool MtStation::send_now(const std::vector<uint8_t> &f, uint16_t rate, bool data, bool fw) {
  int token;
  if (fw) {
    token = _dev.transmit_sta(f.data(), f.size(), Mt7902::kApWcid, Mt7902::kStationOmac);
  } else {
    /* The station is on 5 GHz only, where the card completes frames that
     * wait for an acknowledgement. */
    _dev.force_no_ack = -1;
    _dev.force_altx = -1;
    token = _dev.transmit(f.data(), f.size(), rate, Mt7902::kStationOmac);
  }
  if (token < 0) return false;
  _inflight[static_cast<unsigned>(token)] = data;
  _stats.tx++;
  if (fw) _stats.tx_fw++;
  return true;
}

void MtStation::flush() {
  while (_here && !_held.empty()) {
    const Held &h = _held.front();
    if (!send_now(h.frame, h.rate, h.data, h.fw)) break;
    _held.pop_front();
  }
}

void MtStation::on_channel(bool here) {
  here = here && _active;
  /* Reorder buffers wait only while the AP can send. */
  if (here && !_here)
    for (auto &q : _reorder) q.since = Clock::now();
  _here = here;
  flush();
}

int MtStation::send_null(bool sleep) {
  if (!_active || !_associated) return -1;
  uint8_t f[24] = {0x48, static_cast<uint8_t>(0x01 | (sleep ? 0x10 : 0))}; /* null data, to DS */
  std::memcpy(f + 4, _params.bssid, 6);
  std::memcpy(f + 10, _params.addr, 6);
  std::memcpy(f + 16, _params.bssid, 6);
  _tx_seq = static_cast<uint16_t>((_tx_seq + 1) & 0xfff);
  f[22] = static_cast<uint8_t>(_tx_seq << 4);
  f[23] = static_cast<uint8_t>(_tx_seq >> 4);
  _dev.force_no_ack = -1;
  _dev.force_altx = 1;
  const int token = _dev.transmit(f, sizeof(f), Mt7902::tx_rate_ofdm(6), Mt7902::kStationOmac);
  _dev.force_altx = -1;
  if (token >= 0) {
    _inflight[static_cast<unsigned>(token)] = false;
    _null_token = token;
    _null_sleep = sleep;
    sleep ? _stats.sleep_sent++ : _stats.wake_sent++;
  }
  return token;
}

bool MtStation::on_tx_status(unsigned token, bool acked) {
  auto it = _inflight.find(token);
  if (it == _inflight.end()) return false;
  const bool data = it->second;
  _inflight.erase(it);
  if (static_cast<int>(token) == _null_token) {
    _null_token = -1;
    if (acked) _null_sleep ? _stats.sleep_acked++ : _stats.wake_acked++;
  }
  acked ? _stats.tx_acked++ : _stats.tx_failed++;
  if (data) {
    _sent++;
    _failed += !acked;
    if (_sent >= 32) {
      if (_failed * 100 / _sent > 30 && _level > 0) _level--;
      else if (_failed * 100 / _sent < 8 && _level + 1 < kLevelCount) _level++;
      _sent = _failed = 0;
    }
  }
  return true;
}

/* ---- Receiving ---- */

void MtStation::receive(const Mt7902::RxFrame &in) {
  const uint8_t *f = in.data;
  if (in.len < 16) return;
  const uint8_t type = (f[0] >> 2) & 3;
  /* The card hears the whole BSS: the AP's unicast frames to its other
   * clients are not ours (Intel's firmware filters them; here they spoiled
   * the reorder windows and counted as bad MICs). */
  if (!(f[4] & 1) && std::memcmp(f + 4, _params.addr, 6) != 0) {
    _stats.rx_others++;
    return;
  }
  if (type == 1) {
    /* Block Ack request (control subtype 8): the AP gave up on frames before
     * its starting sequence number. */
    if ((f[0] & 0xf0) == 0x80 && in.len >= 20) {
      const unsigned tid = f[17] >> 4;
      const uint16_t ssn = static_cast<uint16_t>((f[18] | f[19] << 8) >> 4);
      if (tid < 8 && _reorder[tid].on) {
        Reorder &q = _reorder[tid];
        if (seq_after(ssn, q.head) < 2048) {
          release(q, ssn);
          release_ready(q);
        }
      }
    }
    return;
  }
  _stats.rx++;
  if (in.len < 24) return;
  const bool data = type == 2;
  const bool group = (f[4] & 1) != 0;
  const size_t h = ccmp::header_len(f, in.len);
  if (!h) return;
  const bool qos = data && (f[0] & 0x80);
  const unsigned tid = qos ? f[h - 2] & 0x0f : 0;
  const bool ba = qos && !group && tid < 8 && _reorder[tid].on;
  if (f[1] & 0x08) _stats.rx_retry++;
  /* Repeats: the card acknowledges, but a lost acknowledgement still brings
   * the frame again with the retry bit. Under a Block Ack session the
   * reorder buffer drops them. */
  if (!group && !ba) {
    const int32_t seq = seq_of(f);
    int32_t &last = _last_rx_seq[data ? 1 : 0];
    if ((f[1] & 0x08) && seq == last) {
      _stats.rx_repeat++;
      return;
    }
    last = seq;
  }
  Rx r;
  r.dbm = in.dbm;
  r.channel = in.channel;
  r.group = group;
  r.replay = kReplayNone;
  if (!(f[1] & 0x40)) {
    r.frame.assign(f, f + in.len);
  } else {
    if (in.len < h + ccmp::kHeaderLen + ccmp::kMicLen) return;
    const unsigned key_id = f[h + 3] >> 6;
    const ccmp::Key &key = group ? _gtk[key_id] : _ptk;
    if (!key.valid()) {
      /* WifiStation counts and drops it as undecrypted. */
      _stats.rx_no_key++;
      r.frame.assign(f, f + in.len);
      _ready.push_back(std::move(r));
      return;
    }
    if (!key.open(f, in.len, r.frame)) {
      _stats.rx_bad_mic++;
      if (_stats.rx_bad_mic <= 5) {
        char b[160];
        std::snprintf(b, sizeof(b),
                      "station: bad MIC: %zu bytes, fc %02x %02x, QoS %02x, seq %u%s, key %u, PN %llu (last %llu)%s",
                      in.len, f[0], f[1], qos ? f[h - 2] : 0, seq_of(f), (f[1] & 0x08) ? " (retry)" : "", key_id,
                      static_cast<unsigned long long>(ccmp::packet_number(f + h)),
                      static_cast<unsigned long long>(_rx_pn[tid & 0x0f]), ba ? ", under Block Ack" : "");
        _log(b);
      }
      /* The card acknowledged it (its FCS was good), so the AP will not
       * send it again: its place in the reorder buffer is filled empty. */
      if (ba) {
        r.drop = true;
        reorder(tid, seq_of(f), std::move(r));
      }
      return;
    }
    r.opened = true;
    r.pn = ccmp::packet_number(f + h);
    r.replay = group ? kReplayGroup + key_id : (!data ? 16 : tid);
  }
  if (ba) reorder(tid, seq_of(f), std::move(r));
  else deliver(std::move(r));
}

void MtStation::deliver(Rx &&r) {
  if (r.drop) return;
  if (r.opened) {
    uint64_t *last = r.replay >= kReplayGroup ? &_rx_group_pn[(r.replay - kReplayGroup) & 3] : &_rx_pn[r.replay];
    if (r.pn <= *last) {
      _stats.rx_replay++;
      return;
    }
    *last = r.pn;
    _stats.rx_opened++;
  }
  _ready.push_back(std::move(r));
}

void MtStation::reorder(unsigned tid, uint16_t seq, Rx &&r) {
  Reorder &q = _reorder[tid];
  uint16_t d = seq_after(seq, q.head);
  if (d >= 2048) { /* before the window: a repeat, or one we stopped waiting for */
    _stats.rx_late++;
    return;
  }
  if (d >= q.window) { /* past the window: it moves so this frame is its last */
    release(q, static_cast<uint16_t>((seq - q.window + 1) & 0xfff));
    d = seq_after(seq, q.head);
  }
  if (d == 0 && q.held.empty()) {
    deliver(std::move(r));
    q.head = static_cast<uint16_t>((q.head + 1) & 0xfff);
    return;
  }
  if (q.held.count(seq)) {
    _stats.rx_repeat++;
    return;
  }
  if (q.held.empty()) q.since = Clock::now();
  q.held.emplace(seq, std::move(r));
  if (d) _stats.rx_reordered++;
  release_ready(q);
}

/* Hands over everything before `upto`, in order, skipping the gaps. */
void MtStation::release(Reorder &q, uint16_t upto) {
  while (q.head != upto && !q.held.empty()) {
    auto it = q.held.find(q.head);
    if (it != q.held.end()) {
      deliver(std::move(it->second));
      q.held.erase(it);
    }
    q.head = static_cast<uint16_t>((q.head + 1) & 0xfff);
  }
  q.head = upto;
}

/* Hands over the frames from the head on that need nothing before them. */
void MtStation::release_ready(Reorder &q) {
  bool moved = false;
  for (auto it = q.held.find(q.head); it != q.held.end(); it = q.held.find(q.head)) {
    deliver(std::move(it->second));
    q.held.erase(it);
    q.head = static_cast<uint16_t>((q.head + 1) & 0xfff);
    moved = true;
  }
  if (moved) q.since = Clock::now();
}

void MtStation::tick() {
  if (!_here) return;
  const auto now = Clock::now();
  for (auto &q : _reorder) {
    if (!q.on || q.held.empty() || now - q.since < kReorderWait) continue;
    /* The gap is not coming: skip to the first frame held. */
    uint16_t first = q.head, best = 0xffff;
    for (const auto &e : q.held) {
      const uint16_t d = seq_after(e.first, q.head);
      if (d < best) {
        best = d;
        first = e.first;
      }
    }
    release(q, first);
    release_ready(q);
    q.since = now;
  }
}

bool MtStation::take(RxFrame &out) {
  if (_ready.empty()) return false;
  Rx &r = _ready.front();
  _out.swap(r.frame);
  out = RxFrame{_out.data(), _out.size(), r.dbm, r.channel, 0, 0, false};
  if (r.opened) {
    out.status = kStaRxDecDone | kStaRxMicOk;
    out.mic_stripped = true;
  }
  _ready.pop_front();
  return true;
}
