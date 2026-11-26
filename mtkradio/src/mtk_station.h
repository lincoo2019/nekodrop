// SPDX-License-Identifier: GPL-2.0-only
/* The Wi-Fi station's radio side on MediaTek MT7922 / MT7921 (StaDevice,
 * iwxradio/src/sta_device.h), for WifiStation to run the join, WPA2/WPA3 and
 * TAP bridge as it does on Intel and MT7902 cards.
 *
 * Mirrors mtradio/src/mt_station.h and reuses mtradio/src/ccmp.h directly:
 * a second own address (own MAC 1, BSS 1) makes the MAC acknowledge frames to
 * the station, and CCMP is sealed and opened on the host. Once associated, the
 * AP gets a STA record (MtkDevice::kApWcid) built from its beacon's HT/VHT
 * capabilities so data goes out as QoS data at the firmware's rate, and the
 * AP's receive Block Ack sessions are accepted with a per-TID host reorder
 * buffer (no TX BA towards the AP).
 *
 * The card tunes one channel at a time: the radio's scheduler moves it between
 * the access point and AWDL, holding station frames while off channel. */
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ccmp.h"
#include "mtk_dev.h"
#include "sta_device.h"

class MtkStation final : public StaDevice {
public:
  using Logger = std::function<void(const std::string &)>;
  MtkStation(mtk::MtkDevice &dev, Logger log) : _dev(dev), _log(std::move(log)) {}

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
  uint8_t channel() const { return _params.channel; }
  uint8_t width() const { return static_cast<uint8_t>(_associated ? _params.width : 20); }
  const uint8_t *ap() const { return _params.bssid; }
  bool associated() const { return _associated; }

  /* The card is on the AP's channel (true) or away. Queued frames go out when it comes back. */
  void on_channel(bool here);
  bool here() const { return _here; }
  /* Null data frame with the power-management bit: true before leaving the AP's
   * channel, false on returning. Returns TX token or -1. */
  int send_null(bool sleep);
  bool null_pending() const { return _null_token >= 0; }

  /* Frame from the AP. Frames for WifiStation come out of take(), in order:
   * protected ones opened with CCMP header in place and no MIC, status
   * kStaRxDecDone | kStaRxMicOk. Repeats, replays, frames to other clients,
   * and bad MICs are dropped. */
  void receive(const mtk::RxFrame &in);
  bool take(RxFrame &out);
  /* Releases reordered frames that waited too long on channel for a gap to fill. */
  void tick();
  /* TX status: true when the token belonged to the station. */
  bool on_tx_status(unsigned token, bool acked);

  struct Stats {
    uint64_t tx = 0, tx_acked = 0, tx_failed = 0, tx_held = 0, tx_dropped = 0, tx_fw = 0;
    uint64_t rx = 0, rx_opened = 0, rx_bad_mic = 0, rx_no_key = 0, rx_replay = 0, rx_repeat = 0;
    uint64_t rx_others = 0;                 /* AP's unicast frames to its other clients */
    uint64_t rx_retry = 0;                  /* AP retried: it did not hear our ACK */
    uint64_t rx_reordered = 0, rx_late = 0; /* held for a gap; arrived behind the window */
    uint64_t sleep_sent = 0, sleep_acked = 0, wake_sent = 0, wake_acked = 0;
    unsigned rx_ba = 0; /* receive Block Ack sessions from the AP */
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
  struct Rx {
    std::vector<uint8_t> frame;
    int8_t dbm = 0;
    uint8_t channel = 0;
    bool opened = false, group = false, drop = false;
    unsigned replay = 0; /* TID (0-15), 16 = management, 17+ = group key */
    uint64_t pn = 0;
  };
  struct Reorder {
    bool on = false;
    uint16_t head = 0, window = 64;
    std::map<uint16_t, Rx> held; /* by sequence number */
    Clock::time_point since{};   /* oldest held frame, counted only while on channel */
  };

  bool send_now(const std::vector<uint8_t> &f, uint16_t rate, bool data, bool fw);
  void flush();
  uint16_t data_rate() const;
  void deliver(Rx &&r);
  void reorder(unsigned tid, uint16_t seq, Rx &&r);
  void release(Reorder &q, uint16_t upto);
  void release_ready(Reorder &q);

  mtk::MtkDevice &_dev;
  Logger _log;
  StaParams _params{};
  bool _active = false, _associated = false, _here = false, _mfp = false, _fw_sta = false;
  uint16_t _aid = 0;
  ccmp::Key _ptk, _gtk[4];
  uint64_t _tx_pn = 0;
  uint64_t _rx_pn[17] = {}, _rx_group_pn[4] = {};
  uint16_t _tx_seq = 0, _qos_seq = 0;
  int32_t _last_rx_seq[2] = {-1, -1};
  std::deque<Held> _held;
  std::map<unsigned, bool> _inflight; /* token -> fixed-rate unicast data */
  int _null_token = -1;
  bool _null_sleep = false;
  Reorder _reorder[8];
  std::deque<Rx> _ready;
  std::vector<uint8_t> _out;
  bool _ba_broken = false;
  unsigned _level = 2, _sent = 0, _failed = 0;
  Stats _stats;
};
