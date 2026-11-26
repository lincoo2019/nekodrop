// SPDX-License-Identifier: GPL-2.0-only
/* The Wi-Fi station's radio side on the MT7902 (StaDevice, iwxradio's
 * sta_device.h), for WifiStation to run the join, WPA2/WPA3 and the TAP
 * bridge as it does on Intel cards.
 *
 * The card stays in sniffer mode for AWDL, so the station is done on the
 * host: a second own address (own MAC 1) makes the MAC acknowledge frames to
 * the station, and CCMP is sealed and opened here (ccmp.h). Once associated,
 * the AP gets a STA record of its own (Mt7902::kApWcid): data goes out at the
 * firmware's rate, and the AP's Block Ack sessions are taken. The card hands
 * received A-MPDUs over in the order heard, so frames under a session are
 * reordered here before WifiStation sees them.
 *
 * The card has one channel at a time (mt7921: num_different_channels 1): the
 * radio's scheduler moves it between the access point and AWDL, and frames
 * for the AP wait here while it is away (on_channel). */
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ccmp.h"
#include "mt7902.h"
#include "sta_device.h"

class MtStation final : public StaDevice {
public:
  using Logger = std::function<void(const std::string &)>;
  MtStation(Mt7902 &dev, Logger log) : _dev(dev), _log(std::move(log)) {}

  /* ---- StaDevice ---- */
  bool sta_join(const StaParams &p) override;
  bool sta_associated(uint16_t aid, uint8_t dtim_period) override;
  void sta_join_update(const StaParams &p) override { _params = p; }
  bool sta_set_key(bool group, unsigned key_index, const uint8_t key[16]) override;
  bool sta_set_mfp(bool on) override {
    _mfp = on;
    return true;
  }
  /* Broadcast management frames' integrity (BIP) is not checked on the host. */
  bool sta_set_igtk(unsigned, const uint8_t *, uint64_t) override { return true; }
  bool sta_rate_control() override { return _fw_sta; }
  /* Data at the firmware's rate once the AP has its STA record. */
  bool sta_tlc() const override { return _fw_sta; }
  int sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt) override;
  bool sta_add_rx_ba(unsigned tid, uint16_t ssn, uint16_t window) override;
  bool sta_remove_rx_ba(unsigned tid) override;
  void sta_leave() override;
  const StaParams &sta_params() const override { return _params; }

  /* ---- The radio's side ---- */
  bool joined() const { return _active; }
  /* The AP's channel and the width to receive it at. */
  uint8_t channel() const { return _params.channel; }
  uint8_t width() const { return static_cast<uint8_t>(_associated ? _params.width : 20); }
  const uint8_t *ap() const { return _params.bssid; }
  bool associated() const { return _associated; }

  /* The card is on the AP's channel (true) or away. Queued frames go out
   * when it comes back. */
  void on_channel(bool here);
  bool here() const { return _here; }
  /* A null data frame with the power-management bit: true before leaving the
   * AP's channel (it holds our frames), false on coming back. Token or -1. */
  int send_null(bool sleep);
  bool null_pending() const { return _null_token >= 0; }

  /* A frame from the AP. Frames for WifiStation come out of take(), in
   * order: protected ones opened, the CCMP header in place and no MIC, with
   * status kStaRxDecDone | kStaRxMicOk. Repeats, replays and frames that do
   * not decrypt are dropped. */
  void receive(const Mt7902::RxFrame &in);
  bool take(RxFrame &out);
  /* Lets go of reordered frames that waited too long for a gap to fill. */
  void tick();
  /* TX status: true when the token was the station's. */
  bool on_tx_status(unsigned token, bool acked);

  struct Stats {
    uint64_t tx = 0, tx_acked = 0, tx_failed = 0, tx_held = 0, tx_dropped = 0, tx_fw = 0;
    uint64_t rx = 0, rx_opened = 0, rx_bad_mic = 0, rx_no_key = 0, rx_replay = 0, rx_repeat = 0;
    uint64_t rx_others = 0;                 /* the AP's frames to its other clients */
    uint64_t rx_retry = 0;                  /* the AP sent it again: it did not hear our acknowledgement */
    uint64_t rx_reordered = 0, rx_late = 0; /* held for a gap; came after the window moved on */
    uint64_t sleep_sent = 0, sleep_acked = 0, wake_sent = 0, wake_acked = 0;
    unsigned rx_ba = 0; /* Block Ack sessions from the AP */
  };
  const Stats &stats() const { return _stats; }
  std::string rate_name() const;

private:
  using Clock = std::chrono::steady_clock;
  struct Held {
    std::vector<uint8_t> frame;
    uint16_t rate;
    bool data, fw;
  };
  /* A received frame on its way to WifiStation: opened, its packet number
   * still to be checked. */
  struct Rx {
    std::vector<uint8_t> frame;
    int8_t dbm = 0;
    uint8_t channel = 0;
    bool opened = false, group = false, drop = false; /* drop: a place held for a frame lost here */
    unsigned replay = 0; /* which counter: a TID, 16 management, 17+ a group key */
    uint64_t pn = 0;
  };
  /* A receive Block Ack session's reorder buffer (as mac80211's tid_ampdu_rx). */
  struct Reorder {
    bool on = false;
    uint16_t head = 0, window = 64;
    std::map<uint16_t, Rx> held; /* by distance from head */
    Clock::time_point since{};   /* the oldest frame held, counted only while on the channel */
  };

  bool send_now(const std::vector<uint8_t> &f, uint16_t rate, bool data, bool fw);
  void flush();
  uint16_t data_rate() const;
  void deliver(Rx &&r);
  void reorder(unsigned tid, uint16_t seq, Rx &&r);
  void release(Reorder &q, uint16_t upto);
  void release_ready(Reorder &q);

  Mt7902 &_dev;
  Logger _log;
  StaParams _params{};
  bool _active = false, _associated = false, _here = false, _mfp = false, _fw_sta = false;
  uint16_t _aid = 0;
  ccmp::Key _ptk, _gtk[4];
  uint64_t _tx_pn = 0;
  /* Replay counters: per TID for unicast (16 = management), per key ID for group. */
  uint64_t _rx_pn[17] = {}, _rx_group_pn[4] = {};
  uint16_t _tx_seq = 0, _qos_seq = 0; /* management and plain data; QoS data on TID 0 */
  int32_t _last_rx_seq[2] = {-1, -1}; /* management, data: the last sequence number heard */
  std::deque<Held> _held;
  std::map<unsigned, bool> _inflight; /* token -> fixed-rate unicast data (the rate ladder counts it) */
  int _null_token = -1;
  bool _null_sleep = false;
  Reorder _reorder[8];
  std::deque<Rx> _ready;
  std::vector<uint8_t> _out; /* the frame take() last handed out */
  bool _ba_broken = false;    /* the card did not take a session: decline the rest */
  /* Fixed rates for data before the AP's STA record, slowest first; moved by the acknowledgements. */
  unsigned _level = 2, _sent = 0, _failed = 0;
  Stats _stats;
};
