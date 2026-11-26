// SPDX-License-Identifier: GPL-2.0-only
/* nekodrop-radio-mt: owns a MediaTek MT7902 through the register proxy and
 * relays frames for one client (awdld) over winawdl's radio pipe
 * (radio_link.h), like nekodrop-radio-rtl does for the RTL8852CE.
 *
 * Usage: nekodrop-radio-mt <channel> [--width 20|40|80] [--country CC]
 *        [--fw-dir DIR] [--data-rate auto|MCSn|OFDMn] [--sta-config FILE]
 *        [--run-for SECONDS]
 *        nekodrop-radio-mt <channel> --test-rx SECONDS   (no pipe: what is heard)
 *        nekodrop-radio-mt <channel> --test-tx SECONDS   (probes an access point heard)
 *        nekodrop-radio-mt <channel> --test-switch N     (times N channel moves)
 *
 * With --sta-config the radio also runs a Wi-Fi station (Concurrent mode):
 * iwxradio's WifiStation through MtStation (mt_station.h). The card has one
 * channel at a time, so when the access point is on another channel than
 * AWDL the radio moves between the two in step with AWDL's 16-entry channel
 * sequence, naming the router's channel in the entries it spends there (the
 * coexistence plan of iwxradio's coex_plan.h). stdin takes "scan [SSID hex]"
 * and "rejoin" from the service, as the Intel radio's.
 *
 * One thread owns the card: it drains the RX rings and the client's queue.
 * Another reads the pipe. Lines starting "radio: " are for NekoDropService
 * (Supervisor.OnRadioLine): "ready on channel", "bring-up failed: ", "rx: ",
 * "event: ". */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <timeapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "awdl_policy.h"
#include "coex_plan.h"
#include "mt7902.h"
#include "mt_station.h"
#include "pipe_server.h"
#include "radio_link.h"
#include "wifi_sta.h"

namespace {

using Clock = std::chrono::steady_clock;
using Mac = std::array<uint8_t, 6>;

std::atomic<bool> g_stop{false};
/* The firmware asserted or the card stopped completing frames: the daemon
 * exits so NekoDropService starts a fresh radio (Supervisor's watchdog). */
std::atomic<bool> g_card_dead{false};
/* Peer stations exist: a firmware crash now is blamed on them. */
std::atomic<bool> g_stations_up{false};

/* After the firmware crashed with peer stations, the next radios run
 * without them for a while (fixed rates, no A-MPDU), so transfers finish. */
const char *crash_flag_path() { return R"(C:\ProgramData\nekodrop\mt7902-station-crash)"; }
bool recent_station_crash() {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExA(crash_flag_path(), GetFileExInfoStandard, &a)) return false;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const auto t = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return t(now) - t(a.ftLastWriteTime) < 30ull * 60 * 10000000;
}


BOOL WINAPI on_console(DWORD) {
  g_stop = true;
  return TRUE;
}

void say(const char *f, ...) {
  char line[600];
  va_list a;
  va_start(a, f);
  std::vsnprintf(line, sizeof(line), f, a);
  va_end(a);
  std::printf("radio: %s\n", line);
  std::fflush(stdout);
}

std::string mac_str(const uint8_t *m) {
  char s[18];
  std::snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return s;
}

std::string find_firmware_dir() {
  namespace fs = std::filesystem;
  std::error_code ec;
  /* Beside the program first (an installed nekodrop ships it), then the
   * MediaTek driver's own copy in the driver store. */
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const fs::path local = fs::path(self).parent_path() / "firmware";
  if (fs::exists(local / "WIFI_RAM_CODE_MT7902_1.bin", ec)) return local.string();
  fs::path best;
  fs::file_time_type best_time{};
  for (const auto &e : fs::directory_iterator(R"(C:\Windows\System32\DriverStore\FileRepository)", ec)) {
    if (!e.is_directory(ec) || e.path().filename().string().rfind("mtkwl", 0) != 0) continue;
    const fs::path ram = e.path() / "WIFI_RAM_CODE_MT7902_1.bin";
    if (!fs::exists(ram, ec) || !fs::exists(e.path() / "WIFI_MT7902_patch_mcu_1_1_hdr.bin", ec)) continue;
    const auto t = fs::last_write_time(ram, ec);
    if (best.empty() || t > best_time) {
      best = e.path();
      best_time = t;
    }
  }
  return best.string();
}

struct Options {
  uint8_t channel = 0;
  uint8_t width = 0;
  std::string country = "CN";
  std::string fw_dir;
  std::string data_rate = "auto";
  bool aggregate = true;
  unsigned test_rx = 0, test_tx = 0, test_kinds = 0, test_sta = 0, test_switch = 0, run_for = 0;
  bool test_scan = false;
  int only = -1;
  std::string sta_config;
};

std::optional<Options> parse(int argc, char **argv) {
  if (argc < 2) return std::nullopt;
  Options o;
  const int ch = std::atoi(argv[1]);
  if (ch < 1 || ch > 177) return std::nullopt;
  o.channel = static_cast<uint8_t>(ch);
  for (int i = 2; i < argc; i++) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    if (a == "--width" && more) o.width = static_cast<uint8_t>(std::atoi(argv[++i]));
    else if (a == "--country" && more) o.country = argv[++i];
    else if (a == "--fw-dir" && more) o.fw_dir = argv[++i];
    else if (a == "--data-rate" && more) o.data_rate = argv[++i];
    else if (a == "--no-agg") o.aggregate = false;
    else if (a == "--test-rx" && more) o.test_rx = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--test-tx" && more) o.test_tx = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--test-kinds" && more) o.test_kinds = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--only" && more) o.only = std::atoi(argv[++i]);
    else if (a == "--test-sta" && more) o.test_sta = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--test-switch" && more) o.test_switch = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--test-scan") o.test_scan = true;
    else if (a == "--run-for" && more) o.run_for = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--sta-config" && more) o.sta_config = argv[++i];
    else return std::nullopt;
  }
  if (o.width == 0) o.width = o.channel > 14 ? 80 : 20;
  if (o.width != 20 && o.width != 40 && o.width != 80) return std::nullopt;
  if (o.channel <= 14) o.width = 20;
  if (o.country.size() != 2) return std::nullopt;
  return o;
}

/* ---- 802.11 helpers ---- */
constexpr uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

uint16_t fc_of(const uint8_t *f) { return static_cast<uint16_t>(f[0] | f[1] << 8); }
bool is_mgmt(uint16_t fc) { return ((fc >> 2) & 3) == 0; }
bool is_data(uint16_t fc) { return ((fc >> 2) & 3) == 2; }
bool is_action(uint16_t fc) { return (fc & 0xfc) == 0xd0; }
bool is_beacon(uint16_t fc) { return (fc & 0xfc) == 0x80; }

/* AWDL action frames: category 127 (vendor), Apple OUI 00:17:f2, type 8. */
bool is_awdl(const uint8_t *f, size_t len) {
  return len >= 30 && is_action(fc_of(f)) && f[24] == 127 && f[25] == 0x00 && f[26] == 0x17 &&
         f[27] == 0xf2 && f[28] == 0x08;
}

/* Rates unicast data may go at, slowest first. */
struct RateLadder {
  std::vector<std::pair<std::string, uint16_t>> levels;
  size_t level = 0;
  unsigned sent = 0, failed = 0;
  uint64_t up = 0, down = 0;
  void result(bool acked) {
    sent++;
    failed += !acked;
    if (sent < 32) return;
    if (failed * 100 / sent > 30 && level > 0) {
      level--;
      down++;
    } else if (failed * 100 / sent < 8 && level + 1 < levels.size()) {
      level++;
      up++;
    }
    sent = failed = 0;
  }
};

RateLadder make_ladder(const std::string &spec) {
  RateLadder l;
  auto named = [](const std::string &s) -> std::optional<std::pair<std::string, uint16_t>> {
    if (s.rfind("MCS", 0) == 0) return std::make_pair(s, Mt7902::tx_rate_ht(std::atoi(s.c_str() + 3)));
    if (s.rfind("OFDM", 0) == 0) return std::make_pair(s, Mt7902::tx_rate_ofdm(std::atoi(s.c_str() + 4)));
    return std::nullopt;
  };
  if (spec != "auto") {
    if (auto r = named(spec)) l.levels.push_back(*r);
  }
  if (l.levels.empty()) {
    /* What iPhones were measured to receive on other backends (winawdl's ladder). */
    for (const char *s : {"OFDM24", "MCS2", "MCS4", "MCS5", "MCS7"}) l.levels.push_back(*named(s));
    l.level = 2;
  }
  return l;
}

/* Networks heard in beacons and probe responses, for the service's Wi-Fi
 * list (as iwxradio's radio_main). */
struct BssSeen {
  std::string ssid;
  uint8_t channel = 0;
  int8_t dbm = -127;
  std::string security;
  Clock::time_point seen{};
};

std::string bss_security(const uint8_t *ies, size_t len, uint16_t capability) {
  bool psk = false, sae = false, dot1x = false, rsn = false, owe = false;
  for (size_t i = 0; i + 2 <= len;) {
    const uint8_t id = ies[i], n = ies[i + 1];
    if (i + 2 + n > len) break;
    if (id == 48 && n >= 8) {
      rsn = true;
      const uint8_t *r = ies + i + 2;
      size_t o = 2 + 4;
      const unsigned pc = r[o] | r[o + 1] << 8;
      o += 2 + 4u * pc;
      if (o + 2 <= n) {
        const unsigned ac = r[o] | r[o + 1] << 8;
        o += 2;
        for (unsigned k = 0; k < ac && o + 4 <= n; k++, o += 4) {
          if (r[o] != 0x00 || r[o + 1] != 0x0f || r[o + 2] != 0xac) continue;
          if (r[o + 3] == 2 || r[o + 3] == 6) psk = true;
          else if (r[o + 3] == 8) sae = true;
          else if (r[o + 3] == 18) owe = true;
          else if (r[o + 3] == 1 || r[o + 3] == 5) dot1x = true;
        }
      }
    }
    i += 2 + n;
  }
  if (rsn) {
    if (psk && sae) return "wpa2/3-psk";
    if (sae) return "wpa3-sae";
    if (psk) return "wpa2-psk";
    if (owe) return "owe";
    if (dot1x) return "802.1x";
    return "rsn";
  }
  return (capability & 0x10) ? "wep" : "open";
}

/* Our AWDL schedule's clock, from the sync TLV (4) of the frames awdld sends:
 * the AW counter and period. Entry k of the 16-entry channel sequence spans
 * AWs 4k..4k+3 (iwxradio's coex_plan.h in_step). A frame leaves at or after
 * the start of its AW, never before, so the earliest estimate of when AW 0
 * began is kept; a counter jump (awdld adopting a peer's) starts over. */
struct AwClock {
  bool valid = false;
  double aw_us = 16 * 1024.0;
  double base_us = 0; /* when AW 0 began, on our clock */
  uint64_t last_note_us = 0;

  void note(uint64_t now_us, unsigned counter, unsigned period_tu) {
    if (period_tu < 8 || period_tu > 64) return;
    aw_us = period_tu * 1024.0;
    const double candidate = static_cast<double>(now_us) - counter * aw_us;
    if (!valid || candidate < base_us || candidate - base_us > 1.5 * aw_us || now_us - last_note_us > 5000000)
      base_us = candidate;
    valid = true;
    last_note_us = now_us;
  }
  /* The entry (0-15) at `t_us`. Without awdld's frames, our own clock. */
  unsigned entry(uint64_t t_us) const {
    const double aws = (static_cast<double>(t_us) - (valid ? base_us : 0.0)) / aw_us;
    const int64_t aw = static_cast<int64_t>(aws >= 0 ? aws : aws - 1);
    return static_cast<unsigned>(((aw / 4) % 16 + 16) % 16);
  }
};

/* The AW counter and period in an AWDL action frame's sync TLV (4). */
bool frame_sync(const uint8_t *f, size_t len, unsigned &counter, unsigned &period_tu) {
  for (size_t i = 40; i + 3 <= len;) {
    const size_t n = size_t(f[i + 1]) | size_t(f[i + 2]) << 8;
    if (i + 3 + n > len) return false;
    const uint8_t *v = f + i + 3;
    if (f[i] == 4 && n >= 31) {
      period_tu = static_cast<unsigned>(v[5] | v[6] << 8);
      counter = static_cast<unsigned>(v[29] | v[30] << 8);
      return true;
    }
    i += 3 + n;
  }
  return false;
}

uint64_t now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

/* ---- The relay ---- */
struct ClientMsg {
  uint16_t type;
  std::vector<uint8_t> payload;
  Clock::time_point at = Clock::now();
};

/* Local messages, beside the pipe's radio_link types. */
constexpr uint16_t kStaEthernet = 0x100;   /* a frame from the TAP adapter */
constexpr uint16_t kScanRequest = 0x101;   /* "scan [SSID hex]" on stdin */
constexpr uint16_t kRejoinRequest = 0x102; /* "rejoin" on stdin */

/* While AirDrop moves no data, Wi-Fi gets 12 of the 16 entries: AWDL keeps
 * the first four (a quarter second in every second) to be found. */
constexpr uint8_t kNoPeerAway[16] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

class Relay {
public:
  Relay(Mt7902 &dev, PipeServer &pipe, RateLadder ladder) : _dev(dev), _pipe(pipe), _ladder(std::move(ladder)) {
    /* Channel moves (scans, the station's time sharing, AWDL's channel)
     * retune the RF only: with the sniffer's own channel too a move took
     * 21 ms (median) and up to 0.36 s, which a router took for a lost
     * station; without it 10.5 ms, at most 11.6 ms, and the card hears the
     * new channel. Not waiting for the firmware's answer left it on the old
     * channel (--test-switch, --test-scan, 2026-11-24). */
    dev.move_sniffer = false;
    /* The answer comes in ~10 ms; now and then not for 0.35 s, so a move
     * goes on after 60 ms (the switch itself still happens). */
    dev.move_timeout_ms = 60;
    dev.sta_cmd_timeout_ms = 500;
    dev.set_rx_handler([this](const Mt7902::RxFrame &f) { on_rx(f); });
    dev.set_tx_status_handler([this](unsigned token, bool acked, unsigned tries) { on_tx_status(token, acked, tries); });
  }

  void push(ClientMsg m) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_queue.size() < 4096) _queue.push_back(std::move(m));
  }

  static constexpr unsigned kReservedSlots = 16;

  /* A TX message whose frame is data to one station. */
  static bool is_unicast_data(const ClientMsg &m) {
    if (m.type != RADIO_LINK_TX || m.payload.size() < 4) return false;
    const size_t rt = m.payload[2] | static_cast<size_t>(m.payload[3]) << 8;
    if (m.payload.size() < rt + 10) return false;
    const uint8_t *f = m.payload.data() + rt;
    return is_data(fc_of(f)) && !(f[4] & 1);
  }

  void client_gone() {
    std::lock_guard<std::mutex> lock(_mutex);
    _queue.clear();
  }

  /* Starts the Wi-Fi station beside AWDL (Concurrent mode). */
  bool start_station(StaConfig cfg) {
    /* Block Ack from the AP: MtStation reorders on the host. */
    cfg.block_ack = true;
    const auto log = [](const std::string &l) { say("%s", l.c_str()); };
    if (cfg.channel <= 14) {
      /* The card completes no frame that waits for an acknowledgement on
       * 2.4 GHz (--test-kinds), and a station's every frame does. */
      say("station: \"%s\" is on 2.4 GHz (channel %u); the MT7902 joins only 5 GHz networks beside AirDrop",
          cfg.ssid.c_str(), cfg.channel);
      say("event: wifi failed");
      return false;
    }
    _sta_dev = std::make_unique<MtStation>(_dev, log);
    _sta = std::make_unique<WifiStation>(*_sta_dev, log);
    std::string err;
    if (!_sta->start(cfg, err)) {
      say("station: %s", err.c_str());
      say("event: wifi failed");
      _sta.reset();
      _sta_dev.reset();
      return false;
    }
    say("station: joining \"%s\" on channel %u, AWDL on %u%s", cfg.ssid.c_str(), cfg.channel, channel,
        cfg.channel == channel ? " (one channel for both)" : " (the card moves between the two)");
    return true;
  }
  WifiStation *station() { return _sta.get(); }

  /* The card's thread. */
  void run() {
    auto last_report = Clock::now();
    unsigned idle = 0;
    /* Three lanes: AWDL's own frames (action frames carrying the sync
     * timestamps, multicast, settings) never wait behind a burst of unicast
     * data, and data never takes the last kReservedSlots TX slots. Late
     * sync frames make an iPhone drop us from AWDL mid-transfer. Local
     * messages (settings, the station's Ethernet frames, scans) never wait
     * for AWDL frames, which wait while the card is with the router. */
    std::deque<ClientMsg> ctrl, data, local;
    while (!g_stop) {
      {
        std::lock_guard<std::mutex> lock(_mutex);
        while (!_queue.empty() && ctrl.size() + data.size() + local.size() < 1024) {
          ClientMsg &m = _queue.front();
          if (m.type != RADIO_LINK_TX) local.push_back(std::move(m));
          else (is_unicast_data(m) ? data : ctrl).push_back(std::move(m));
          _queue.pop_front();
        }
      }
      unsigned did = 0;
      while (!local.empty()) {
        handle(local.front());
        local.pop_front();
        did++;
      }
      did += schedule();
      if (_dev.channel() == channel && !_scan.active) {
        while (!ctrl.empty()) {
          /* Held while the card was away: its sync timestamps are stale. */
          if (Clock::now() - ctrl.front().at > std::chrono::milliseconds(40)) {
            ctrl.pop_front();
            _tx_stale++;
            continue;
          }
          if (!handle(ctrl.front())) break; /* no TX slot: try again after polling */
          ctrl.pop_front();
          did++;
        }
        while (!data.empty() && _dev.tx_slots_free() > kReservedSlots) {
          if (!handle(data.front())) break;
          data.pop_front();
          did++;
        }
      } else {
        while (ctrl.size() > 256) {
          ctrl.pop_front();
          _tx_stale++;
        }
      }
      did += _dev.poll();
      if (_sta) did += station_tick();
      if (_pipe.connected()) _pipe.flush_send();
      /* Every TX slot out for seconds: the card stopped reporting sent
       * frames, and nothing more can go out. */
      if (_dev.tx_slots_free() == 0) {
        if (_stalled_since == Clock::time_point{}) _stalled_since = Clock::now();
        else if (Clock::now() - _stalled_since > std::chrono::seconds(3) && !g_card_dead) {
          say("TX stalled: no frame reported sent for 3 s; restarting the radio");
          g_card_dead = true;
          g_stop = true;
        }
      } else {
        _stalled_since = Clock::time_point{};
      }
      if (did) idle = 0;
      else if (++idle > 200) Sleep(1);
      else SwitchToThread();
      if (Clock::now() - last_report > std::chrono::seconds(10)) {
        report();
        last_report = Clock::now();
      }
    }
  }

  void report() {
    say("rx: %llu frames, %llu awdl, %llu addressed to us (%llu%% repeats); tx: %llu sent, %llu acked, "
        "%llu failed, %llu refused; data rate %s (up %llu, down %llu); ADDBA declined %llu; "
        "stations %u, firmware-rate data %llu, A-MPDU rx sessions %llu tx sessions %llu; power-save bit on %llu",
        _rx, _rx_awdl, _rx_to_us, _rx_to_us ? _rx_to_us_retry * 100 / _rx_to_us : 0ull, _tx, _tx_acked,
        _tx_failed, _tx_refused, _ladder.levels[_ladder.level].first.c_str(), _ladder.up, _ladder.down,
        _addba_declined, static_cast<unsigned>(_peers.size()), _tx_sta, _rx_ba_sessions, _tx_ba_sessions,
        _rx_to_us_pm);
    if (_sta) {
      const MtStation::Stats &s = _sta_dev->stats();
      unsigned router = 0;
      for (uint8_t a : _away) router += a;
      say("wifi: %s, %llu packets in, %llu out; frames to the AP %llu sent, %llu acked, %llu failed, %llu held, "
          "%llu dropped, rate %s (%llu at it); from the AP %llu (and %llu to others), %llu opened, %llu bad MIC, "
          "%llu without key, %llu replayed, %llu repeats, %llu sent again by the AP, A-MPDU sessions %u (%llu "
          "reordered, %llu late); sleep/wake frames %llu/%llu sent, %llu/%llu acked; channel moves "
          "%llu (%llu us average, %llu us longest), %u of 16 entries on the router's channel%s, %llu%% of the time "
          "there; AWDL frames held too long %llu",
          _sta->state_name(), _sta->rx_packets(), _sta->tx_packets(), s.tx, s.tx_acked, s.tx_failed, s.tx_held,
          s.tx_dropped, _sta_dev->rate_name().c_str(), s.tx_fw, s.rx, s.rx_others, s.rx_opened, s.rx_bad_mic,
          s.rx_no_key, s.rx_replay, s.rx_repeat, s.rx_retry, s.rx_ba, s.rx_reordered, s.rx_late, s.sleep_sent,
          s.wake_sent, s.sleep_acked, s.wake_acked, _moves,
          _moves ? _move_us / _moves : 0ull, _move_us_max, _timeshare ? router : 0u,
          _aw.valid ? "" : " (no AWDL clock yet)", _here_total_us ? _here_us * 100 / _here_total_us : 0ull, _tx_stale);
    }
  }

  uint8_t channel = 0, width = 20;
  bool aggregate = true;

private:
  /* ---- Wi-Fi station: events, answers, the channel scheduler, scans ---- */

  unsigned station_tick() {
    unsigned did = 0;
    _sta->tick(now_us());
    _sta_dev->tick();
    RxFrame late{};
    while (_sta_dev->take(late)) {
      _sta->on_frame(late);
      did++;
    }
    if (_sta->state() != _published_wifi) {
      _published_wifi = _sta->state();
      if (_published_wifi == WifiStation::State::Running) _running_since = Clock::now();
      say("event: wifi %s", _sta->state_name());
    }
    std::vector<uint8_t> ans;
    while (_sta->take_addba(ans)) {
      TxRate r; /* 6 Mb/s */
      _sta_dev->sta_transmit(ans.data(), ans.size(), r, (ans[1] & 0x40) != 0);
      did++;
    }
    return did;
  }

  bool station_on_air() const { return _sta && _sta_dev->joined(); }

  /* The channel the card should be on now, and moves it there. */
  unsigned schedule() {
    if (_scan.active) return scan_step();
    uint8_t want = channel, want_w = width;
    _timeshare = false;
    if (station_on_air()) {
      const uint8_t s = _sta_dev->channel();
      const WifiStation::State st = _sta->state();
      const bool joining = st == WifiStation::State::Listening || st == WifiStation::State::Authenticating ||
                           st == WifiStation::State::Associating || st == WifiStation::State::Handshake;
      if (s == channel) {
        want_w = width;
      } else if (joining) {
        /* A join takes the card for the second or so it needs, at AWDL's
         * width (the sniffer's): the moves stay the RF switch alone. */
        want = s;
        want_w = width;
      } else if (st == WifiStation::State::Running && Clock::now() - _running_since < std::chrono::seconds(3)) {
        /* The first seconds after joining (DHCP, ARP, the router's own
         * checks) stay with the router: leaving at once got us
         * disconnected, reason 1, twice in a row. */
        want = s;
        want_w = _sta_dev->width();
      } else if (st == WifiStation::State::Running) {
        _timeshare = true;
        const uint64_t t = now_us();
        plan(t, s);
        /* A little early, so the card is there when the entry begins. */
        if (_away[_aw.entry(t + 2000)]) {
          want = s;
          want_w = _sta_dev->width();
        }
      }
    }
    return move_to(want, want > 14 ? want_w : 20);
  }

  /* Which of the 16 entries go to the router. While AirDrop moves data, the
   * plan's tables (Apple's, coex_plan.h) give AWDL most of them. Otherwise
   * Wi-Fi keeps 12: Apple's idle table (4 for the router) assumes AWDL is up
   * only while something uses it, and nekodrop's is up all the time, with
   * iPhones nearby talking AWDL now and then (2026-11-24: 130 KB/s). */
  void plan(uint64_t t, uint8_t s) {
    const bool moving = _last_awdl_data_us && t - _last_awdl_data_us < awdl_plan::kDataHoldUS;
    _meter.note(t, _sta->rx_packets() + _sta->tx_packets(), _sta->rx_bytes() + _sta->tx_bytes());
    uint8_t away[16];
    if (!moving || !awdl_plan::plan_entries(true, s, _sta_dev->width(), channel, true, _meter.cls, away))
      std::memcpy(away, kNoPeerAway, 16);
    if (std::memcmp(away, _away, 16) != 0) {
      std::memcpy(_away, away, 16);
      unsigned n = 0;
      for (uint8_t a : away) n += a;
      say("timeshare: %u of 16 entries on the router's channel %u (%s)", n, s,
          moving ? "AirDrop moving data" : "AirDrop idle");
    }
  }

  unsigned move_to(uint8_t want, uint8_t w) {
    const uint8_t at = _dev.channel();
    const bool at_router = station_on_air() && at == _sta_dev->channel();
    /* Time on the router's channel while joined (the report's share). */
    const uint64_t t = now_us();
    if (_here_last_us && _sta && _sta->state() == WifiStation::State::Running) {
      _here_total_us += t - _here_last_us;
      if (at_router) _here_us += t - _here_last_us;
    }
    _here_last_us = t;
    if (want == at && w == _dev.width_mhz() && !_move_failed) {
      if (_sta) _sta_dev->on_channel(at_router);
      return 0;
    }
    /* Leaving the router: it holds our frames once it knows we sleep. */
    const bool running = _sta && _sta->state() == WifiStation::State::Running;
    if (want != at && at_router && _sta_dev->associated() && running) {
      if (!_ps_asked) {
        _sta_dev->send_null(true);
        _ps_asked = true;
        _ps_at = Clock::now();
        return 1;
      }
      if (_sta_dev->null_pending() && Clock::now() - _ps_at < std::chrono::milliseconds(8)) return 0;
    }
    _ps_asked = false;
    if (_sta) _sta_dev->on_channel(false);
    const auto t0 = Clock::now();
    const bool ok = _dev.set_channel(want, w);
    const uint64_t us =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
    _moves++;
    _move_us += us;
    if (us > _move_us_max) _move_us_max = us;
    if (!ok && _move_fail_logged++ < 5) say("channel move to %u failed", want);
    /* The switch may not have happened: the next round does it once more. */
    _move_failed = !ok && !_move_failed;
    const bool to_router = station_on_air() && want == _sta_dev->channel();
    if (to_router) {
      _sta_dev->on_channel(true);
      if (_sta_dev->associated() && running && want != channel) _sta_dev->send_null(false);
    }
    return 1;
  }

  /* ---- Scans for the service's Wi-Fi list: every channel in turn ---- */
  struct Scan {
    bool active = false, ps_done = false;
    std::vector<uint8_t> channels, ssid;
    size_t next = 0;
    Clock::time_point until{}, started{};
    uint64_t frames = 0, beacons = 0;
    std::map<uint8_t, std::pair<uint64_t, uint64_t>> heard; /* channel -> frames, beacons */
  } _scan;

  void start_scan(const std::vector<uint8_t> &ssid) {
    if (_scan.active) return;
    _scan = Scan{};
    _scan.active = true;
    _scan.ssid = ssid;
    _scan.started = Clock::now();
    for (uint8_t c = 36; c <= 64; c += 4) _scan.channels.push_back(c);
    for (uint8_t c = 100; c <= 144; c += 4) _scan.channels.push_back(c);
    for (uint8_t c = 149; c <= 165; c += 4) _scan.channels.push_back(c);
    for (uint8_t c = 1; c <= 13; c++) _scan.channels.push_back(c);
    say("event: scan-started");
  }

  unsigned scan_step() {
    const auto now = Clock::now();
    if (!_scan.ps_done) {
      const bool running = _sta && _sta->state() == WifiStation::State::Running;
      if (station_on_air() && running && _dev.channel() == _sta_dev->channel()) {
        if (!_ps_asked) {
          _sta_dev->send_null(true);
          _ps_asked = true;
          _ps_at = now;
          return 1;
        }
        if (_sta_dev->null_pending() && now - _ps_at < std::chrono::milliseconds(8)) return 0;
      }
      _ps_asked = false;
      _scan.ps_done = true;
      if (_sta) _sta_dev->on_channel(false);
    }
    if (_scan.until != Clock::time_point{} && now < _scan.until) return 0;
    if (_scan.next >= _scan.channels.size()) {
      _scan.active = false;
      std::string heard;
      for (const auto &[ch, c] : _scan.heard)
        heard += " " + std::to_string(ch) + ":" + std::to_string(c.first) + "/" + std::to_string(c.second);
      say("scan: %llu frames, %llu beacons or probe responses (channel:frames/beacons%s)", _scan.frames,
          _scan.beacons, heard.c_str());
      unsigned n = 0;
      for (const auto &[bssid, b] : _bss) {
        if (b.seen < _scan.started || b.ssid.empty()) continue;
        std::string hex;
        char h[3];
        for (unsigned char c : b.ssid) {
          std::snprintf(h, sizeof(h), "%02x", c);
          hex += h;
        }
        say("event: bss %s %u %d %s %s", bssid.c_str(), b.channel, b.dbm, b.security.c_str(), hex.c_str());
        n++;
      }
      say("event: scan-done %u", n);
      return 1;
    }
    const uint8_t ch = _scan.channels[_scan.next++];
    _dev.set_channel(ch, 20);
    const bool radar = ch >= 52 && ch <= 144;
    if (!radar) probe(ch);
    /* From when the card is there: a beacon every 102 ms, so a radar
     * channel (listening only) gets two intervals; one interval (120 ms)
     * often missed the AP when the move ran late. */
    _scan.until = Clock::now() + std::chrono::milliseconds(radar ? 250 : 150);
    return 1;
  }

  /* A broadcast probe request, for the wildcard SSID or the one asked for. */
  void probe(uint8_t ch) {
    std::vector<uint8_t> f(24, 0);
    f[0] = 0x40;
    std::memset(&f[4], 0xff, 6);
    std::memcpy(&f[10], _own_set ? _own.data() : _dev.mac_address(), 6);
    std::memset(&f[16], 0xff, 6);
    _seq = static_cast<uint16_t>((_seq + 1) & 0xfff);
    f[22] = static_cast<uint8_t>(_seq << 4);
    f[23] = static_cast<uint8_t>(_seq >> 4);
    f.push_back(0);
    f.push_back(static_cast<uint8_t>(_scan.ssid.size()));
    f.insert(f.end(), _scan.ssid.begin(), _scan.ssid.end());
    if (ch <= 14) f.insert(f.end(), {1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24});
    else f.insert(f.end(), {1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c});
    _dev.force_no_ack = 1;
    _dev.transmit(f.data(), f.size(), Mt7902::tx_rate_ofdm(6));
    _dev.force_no_ack = -1;
  }

  void note_bss(const Mt7902::RxFrame &f) {
    const uint16_t fc = fc_of(f.data);
    const bool beacon = f.len >= 36 && ((fc & 0xfc) == 0x80 || (fc & 0xfc) == 0x50);
    if (_scan.active) {
      _scan.frames++;
      _scan.beacons += beacon;
      auto &h = _scan.heard[f.channel];
      h.first++;
      h.second += beacon;
    }
    if (!beacon) return;
    const std::string key = mac_str(f.data + 16);
    if (_bss.size() >= 256 && !_bss.count(key)) return;
    BssSeen &b = _bss[key];
    const uint8_t *ies = f.data + 36;
    const size_t n = f.len - 36;
    uint8_t ch = f.channel;
    for (size_t i = 0; i + 2 <= n;) {
      const uint8_t id = ies[i], l = ies[i + 1];
      if (i + 2 + l > n) break;
      if (id == 0 && l <= 32) b.ssid.assign(reinterpret_cast<const char *>(ies + i + 2), l);
      else if ((id == 3 || id == 61) && l >= 1 && ies[i + 2]) ch = ies[i + 2];
      i += 2 + l;
    }
    b.channel = ch;
    b.dbm = f.dbm;
    b.security = bss_security(ies, n, static_cast<uint16_t>(f.data[34] | f.data[35] << 8));
    b.seen = Clock::now();
  }

  bool handle(const ClientMsg &m) {
    const bool six = m.payload.size() == 6;
    if (m.type == RADIO_LINK_TX) return send_client_frame(m);
    if (m.type == kStaEthernet) {
      if (_sta) _sta->on_ethernet(m.payload.data(), m.payload.size());
      return true;
    }
    if (m.type == kScanRequest) {
      start_scan(m.payload);
      return true;
    }
    if (m.type == kRejoinRequest) {
      if (_sta) {
        say("event: wifi-stall");
        _sta->rejoin("the gateway stopped answering");
      }
      return true;
    }
    if (m.type == RADIO_LINK_SET_ADDRESS && six) {
      std::memcpy(_own.data(), m.payload.data(), 6);
      _own_set = true;
      adopt_identity();
    } else if (m.type == RADIO_LINK_SET_BSSID_FILTER && six) {
      std::memcpy(_bssid.data(), m.payload.data(), 6);
      _bssid_set = _bssid != Mac{};
      adopt_identity();
    } else if (m.type == RADIO_LINK_SET_CHANNEL && m.payload.size() == 1 && m.payload[0] != channel) {
      /* AWDL's channel: the scheduler takes the card there. */
      say("channel %u -> %u", channel, m.payload[0]);
      channel = m.payload[0];
    }
    return true;
  }

  void adopt_identity() {
    if (!_own_set) return;
    _peers.clear(); /* re-adding the interface drops its stations */
    const Mac bssid = _bssid_set ? _bssid : Mac{kAwdlBssid[0], kAwdlBssid[1], kAwdlBssid[2], kAwdlBssid[3], kAwdlBssid[4], kAwdlBssid[5]};
    if (_dev.set_identity(_own.data(), bssid.data()))
      say("acknowledging frames for %s (BSSID %s)", mac_str(_own.data()).c_str(), mac_str(bssid.data()).c_str());
    else
      say("the card will not acknowledge frames for the client; peers will repeat everything they send it");
  }

  bool send_client_frame(const ClientMsg &m) {
    const std::vector<uint8_t> &msg = m.payload;
    if (msg.size() < 4) return true;
    const size_t rt = msg[2] | static_cast<size_t>(msg[3]) << 8;
    if (rt >= msg.size() || msg.size() - rt < 10) return true;
    const uint8_t *f = msg.data() + rt;
    const size_t len = msg.size() - rt;
    if (is_awdl(f, len)) {
      unsigned counter = 0, period = 0;
      if (frame_sync(f, len, counter, period)) {
        const uint64_t at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(m.at.time_since_epoch()).count());
        _aw.note(at, counter, period);
      }
      if (_timeshare && station_on_air()) {
        /* Our channel sequence names the router where the card will be there. */
        _rewritten.assign(f, f + len);
        if (awdl_plan::rewrite_frame(_rewritten.data(), len, _away, _sta_dev->channel()))
          return send_frame(_rewritten.data(), len, false);
      }
    }
    return send_frame(f, len, false);
  }

  /* Returns false only when the card has no room: the caller retries. */
  bool send_frame(const uint8_t *f, size_t len, bool ours) {
    const uint16_t fc = fc_of(f);
    const bool group = (f[4] & 1) != 0;
    const bool unicast_data = is_data(fc) && !group;
    if (unicast_data && !ours) _last_awdl_data_us = now_us();
    if (unicast_data && !ours && len >= 24) {
      Peer *p = station(f + 4);
      if (p && p->sta_ok) return send_to_station(*p, f, len);
    }
    const uint16_t rate = unicast_data ? _ladder.levels[_ladder.level].second
                                       : (channel > 14 ? Mt7902::tx_rate_ofdm(6) : Mt7902::tx_rate_ofdm(6));
    /* On 2.4 GHz the MT7902 never completes a frame that waits for an
     * acknowledgement (it stays queued and blocks the queue; 5 GHz is fine,
     * measured with --test-kinds): send there without one. The service moves
     * AWDL to the peer's 5 GHz channel as soon as it hears it. */
    _dev.force_no_ack = channel <= 14 ? 1 : -1;
    const int token = _dev.transmit(f, len, rate);
    if (token < 0) {
      if (_dev.tx_slots_free() == 0) return false;
      _tx_refused++;
      return true;
    }
    _tx++;
    _inflight[static_cast<unsigned>(token)] = unicast_data;
    (void)ours;
    return true;
  }

  void on_tx_status(unsigned token, bool acked, unsigned) {
    if (_sta_dev && _sta_dev->on_tx_status(token, acked)) return;
    auto it = _inflight.find(token);
    const bool unicast_data = it != _inflight.end() && it->second;
    if (it != _inflight.end()) _inflight.erase(it);
    acked ? _tx_acked++ : _tx_failed++;
    if (unicast_data) _ladder.result(acked);
  }

  void on_rx(const Mt7902::RxFrame &f) {
    _rx++;
    if (f.len < 10) return;
    const uint16_t fc = fc_of(f.data);
    note_bss(f);
    if (station_on_air() && f.len >= 20) {
      const RxFrame r{f.data, f.len, f.dbm, f.channel, 0, 0, false};
      /* Block Ack requests from the AP, for the reorder buffers (WifiStation
       * leaves control frames to the card). */
      const bool bar = (f.data[0] & 0xfc) == 0x84 && std::memcmp(f.data + 10, _sta_dev->ap(), 6) == 0;
      if (bar || _sta->owns(r)) {
        _sta_dev->receive(f);
        RxFrame opened{};
        while (_sta_dev->take(opened)) _sta->on_frame(opened);
        return;
      }
    }
    if (is_awdl(f.data, f.len)) {
      _rx_awdl++;
      /* NekoDropService moves the radio to the peer's 5 GHz channel, and
       * learns its country, from these (Supervisor.OnEvent), as with the
       * Intel radio. */
      if (!_peer_heard_here && f.channel == channel && channel > 14) {
        _peer_heard_here = true;
        say("AWDL peer heard on channel %u: transmitting allowed here", channel);
      }
      uint8_t seq[16];
      if (awdl::peer_sequence(f.data, f.len, seq) && std::memcmp(seq, _peer_seq, 16) != 0) {
        std::memcpy(_peer_seq, seq, 16);
        const unsigned ch = awdl::peer_channel(seq);
        if (ch && ch != _peer5g) {
          _peer5g = ch;
          say("event: peer-channel %u", ch);
        }
      }
      char c[3];
      if (awdl::peer_country(f.data, f.len, c) && _peer_country.note(c)) say("event: peer-country %s", _peer_country.confirmed);
    }
    const bool to_us = _own_set && std::memcmp(f.data + 4, _own.data(), 6) == 0;
    if (to_us && is_data(fc)) {
      _rx_to_us++;
      _last_awdl_data_us = now_us();
      if (f.data[1] & 0x08) _rx_to_us_retry++;
      if (f.data[1] & 0x10) _rx_to_us_pm++; /* the peer says it is going to sleep */
    }
    if (to_us && is_action(fc) && f.len >= 24 + 4 && f.data[24] == 3) block_ack_action(f);
    if (is_awdl(f.data, f.len)) learn_caps(f);
    if (f.len < 24 || !_pipe.connected()) return;
    if (_bssid_set && std::memcmp(f.data + 16, _bssid.data(), 6) != 0) return;
    /* Radiotap: FLAGS and DBM_ANTSIGNAL only, as winawdl's radio. */
    _msg.assign(10, 0);
    _msg[2] = 10;
    const uint32_t present = (1u << 1) | (1u << 5);
    std::memcpy(&_msg[4], &present, 4);
    _msg[9] = static_cast<uint8_t>(f.dbm);
    _msg.insert(_msg.end(), f.data, f.data + f.len);
    if (_msg.size() <= RADIO_LINK_MAX_PAYLOAD)
      _pipe.queue_send(RADIO_LINK_RX, {_msg.data(), _msg.size()});
  }

  /* ---- Peer stations and Block Ack (A-MPDU) ----
   * A peer we exchange unicast data with on 5 GHz gets a STA record: the
   * firmware then picks the rate (VHT at 80 MHz included) and the MAC keeps
   * Block Ack sessions, set up from the ADDBA handshake: the peer's requests
   * are accepted once the card is told (iPhone to PC in A-MPDUs), and we ask
   * the peer ourselves for TID 0 (PC to iPhone). Data to a station goes as
   * QoS data, TID 0, with our own sequence numbers. Anything failing falls
   * back to the fixed-rate path. */
  struct Peer {
    Mac addr{};
    uint8_t wcid = 0;
    bool sta_ok = false;
    Mt7902::PeerCaps caps;
    uint16_t seq = 0; /* TID 0 sequence number, last used */
    uint64_t data = 0;
    bool tx_ba = false, addba_pending = false;
    unsigned addba_tries = 0;
    uint16_t addba_ssn = 0;
    uint8_t dialog = 0;
    Clock::time_point addba_at{};
  };

  Peer *find_peer(const uint8_t *mac) {
    for (Peer &p : _peers)
      if (std::memcmp(p.addr.data(), mac, 6) == 0) return &p;
    return nullptr;
  }

  /* The peer's station, set up on first use; nullptr where not possible. */
  Peer *station(const uint8_t *mac) {
    if (!aggregate || channel <= 14) return nullptr;
    if (Peer *p = find_peer(mac)) return p;
    if (_peers.size() >= Mt7902::kLastPeerWcid - Mt7902::kFirstPeerWcid + 1u) return nullptr;
    Peer p;
    std::memcpy(p.addr.data(), mac, 6);
    p.wcid = static_cast<uint8_t>(Mt7902::kFirstPeerWcid + _peers.size());
    auto caps = _caps.find(mac_str(mac));
    if (caps != _caps.end()) p.caps = caps->second;
    p.sta_ok = _dev.sta_add(p.wcid, mac, p.caps);
    if (p.sta_ok) g_stations_up = true;
    say("station %s: wcid %u, %s (HT cap %04x, A-MPDU params %02x)", mac_str(mac).c_str(), p.wcid,
        p.sta_ok ? "firmware rate control" : "not accepted, fixed rates", p.caps.ht_cap, p.caps.ampdu_params);
    _peers.push_back(p);
    return &_peers.back();
  }

  /* AWDL TLV 7 (enhanced data rate capabilities): the sender's HT
   * capabilities, A-MPDU parameters and MCS set (OWL's frame.h). */
  void learn_caps(const Mt7902::RxFrame &f) {
    for (size_t i = 40; i + 3 <= f.len;) {
      const size_t value = size_t(f.data[i + 1]) | size_t(f.data[i + 2]) << 8;
      if (i + 3 + value > f.len) return;
      if (f.data[i] == 7 && value >= 6) {
        Mt7902::PeerCaps c;
        c.ht_cap = static_cast<uint16_t>(f.data[i + 5] | f.data[i + 6] << 8);
        c.ampdu_params = f.data[i + 7];
        c.rx_mcs[0] = f.data[i + 8];
        _caps[mac_str(f.data + 10)] = c;
        return;
      }
      i += 3 + value;
    }
  }

  bool send_to_station(Peer &p, const uint8_t *f, size_t len) {
    const uint16_t fc = fc_of(f);
    const size_t hdr = 24;
    if ((fc & 0x0300) || len > 3800) return send_frame(f, len, true); /* 4-address or oversized */
    /* QoS data, TID 0: A-MPDUs carry QoS data only. */
    _qos.assign(f, f + hdr);
    _qos[0] = static_cast<uint8_t>(_qos[0] | 0x80);
    if (fc & 0x80) {
      _qos.insert(_qos.end(), f + hdr, f + len);
      _qos[hdr] = static_cast<uint8_t>(_qos[hdr] & 0xf0); /* TID 0, normal ack */
    } else {
      _qos.push_back(0);
      _qos.push_back(0);
      _qos.insert(_qos.end(), f + hdr, f + len);
    }
    const uint16_t seq = static_cast<uint16_t>((p.seq + 1) & 0xfff);
    _qos[22] = static_cast<uint8_t>(seq << 4);
    _qos[23] = static_cast<uint8_t>(seq >> 4);
    const int token = _dev.transmit_sta(_qos.data(), _qos.size(), p.wcid);
    if (token < 0) {
      if (_dev.tx_slots_free() == 0) return false;
      _tx_refused++;
      return true;
    }
    p.seq = seq;
    p.data++;
    _tx++;
    _tx_sta++;
    _inflight[static_cast<unsigned>(token)] = false;
    maybe_ask_aggregation(p);
    return true;
  }

  /* We ask the peer to take A-MPDUs on TID 0 once data flows. */
  void maybe_ask_aggregation(Peer &p) {
    if (p.tx_ba || p.data < 8 || p.addba_tries >= 5) return;
    const auto now = Clock::now();
    if (p.addba_pending && now - p.addba_at < std::chrono::seconds(2)) return;
    p.addba_pending = true;
    p.addba_tries++;
    p.addba_at = now;
    p.dialog = static_cast<uint8_t>(p.dialog == 255 ? 1 : p.dialog + 1);
    p.addba_ssn = static_cast<uint16_t>((p.seq + 1) & 0xfff);
    uint8_t r[24 + 9] = {};
    mgmt_header(r, p.addr.data());
    r[24] = 3; /* Block Ack */
    r[25] = 0; /* ADDBA request */
    r[26] = p.dialog;
    const uint16_t params = static_cast<uint16_t>(1u << 1 | 64u << 6); /* immediate, TID 0, 64 frames */
    r[27] = static_cast<uint8_t>(params);
    r[28] = static_cast<uint8_t>(params >> 8);
    r[31] = static_cast<uint8_t>(p.addba_ssn << 4);
    r[32] = static_cast<uint8_t>(p.addba_ssn >> 4);
    send_frame(r, sizeof(r), true);
  }

  void mgmt_header(uint8_t *r, const uint8_t *to) {
    r[0] = 0xd0;
    std::memcpy(r + 4, to, 6);
    std::memcpy(r + 10, _own.data(), 6);
    const Mac bssid = _bssid_set ? _bssid : Mac{kAwdlBssid[0], kAwdlBssid[1], kAwdlBssid[2], kAwdlBssid[3], kAwdlBssid[4], kAwdlBssid[5]};
    std::memcpy(r + 16, bssid.data(), 6);
    _seq = static_cast<uint16_t>((_seq + 1) & 0xfff);
    r[22] = static_cast<uint8_t>(_seq << 4);
    r[23] = static_cast<uint8_t>(_seq >> 4);
  }

  void block_ack_action(const Mt7902::RxFrame &f) {
    const uint8_t action = f.data[25];
    const uint8_t *from = f.data + 10;
    if (action == 0 && f.len >= 24 + 9) { /* ADDBA request */
      Peer *p = station(from);
      if (!p || !p->sta_ok) return decline_addba(f);
      const uint16_t params = static_cast<uint16_t>(f.data[27] | f.data[28] << 8);
      const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0xf);
      uint16_t win = static_cast<uint16_t>(params >> 6);
      if (!win || win > 64) win = 64;
      const uint16_t ssn = static_cast<uint16_t>((f.data[31] | f.data[32] << 8) >> 4);
      if (!_dev.sta_ba(p->wcid, from, tid, ssn, win, false, true)) return decline_addba(f);
      uint8_t r[24 + 9] = {};
      mgmt_header(r, from);
      r[24] = 3;
      r[25] = 1;          /* ADDBA response */
      r[26] = f.data[26]; /* dialog token */
      const uint16_t out = static_cast<uint16_t>(1u << 1 | static_cast<unsigned>(tid) << 2 | win << 6);
      r[29] = static_cast<uint8_t>(out);
      r[30] = static_cast<uint8_t>(out >> 8);
      send_frame(r, sizeof(r), true);
      _rx_ba_sessions++;
      say("A-MPDU from %s accepted: TID %u, window %u", mac_str(from).c_str(), tid, win);
    } else if (action == 1 && f.len >= 24 + 9) { /* ADDBA response */
      Peer *p = find_peer(from);
      if (!p || !p->addba_pending || f.data[26] != p->dialog) return;
      p->addba_pending = false;
      const uint16_t status = static_cast<uint16_t>(f.data[27] | f.data[28] << 8);
      const uint16_t params = static_cast<uint16_t>(f.data[29] | f.data[30] << 8);
      uint16_t win = static_cast<uint16_t>(params >> 6);
      if (!win || win > 64) win = 64;
      if (status == 0 && _dev.sta_ba(p->wcid, from, 0, p->addba_ssn, win, true, true)) {
        p->tx_ba = true;
        _tx_ba_sessions++;
        say("A-MPDU to %s agreed: window %u", mac_str(from).c_str(), win);
      } else {
        say("A-MPDU to %s refused (status %u)", mac_str(from).c_str(), status);
      }
    } else if (action == 2 && f.len >= 24 + 6) { /* DELBA */
      Peer *p = find_peer(from);
      if (!p) return;
      const uint16_t params = static_cast<uint16_t>(f.data[26] | f.data[27] << 8);
      const bool peer_was_originator = (params & 0x0800) != 0;
      const uint8_t tid = static_cast<uint8_t>(params >> 12);
      _dev.sta_ba(p->wcid, from, tid, 0, 0, !peer_was_originator, false);
      if (!peer_was_originator && tid == 0) {
        p->tx_ba = false;
        p->addba_tries = 0;
      }
      say("A-MPDU with %s ended by the peer (TID %u)", mac_str(from).c_str(), tid);
    }
  }

  /* Without a station for the peer: decline, so it sends single frames,
   * which the card acknowledges. Status 37: request declined. */
  void decline_addba(const Mt7902::RxFrame &f) {
    uint8_t r[24 + 9] = {};
    r[0] = 0xd0;
    std::memcpy(r + 4, f.data + 10, 6); /* to the sender */
    std::memcpy(r + 10, _own.data(), 6);
    std::memcpy(r + 16, f.data + 16, 6);
    _seq = static_cast<uint16_t>((_seq + 1) & 0xfff);
    r[22] = static_cast<uint8_t>(_seq << 4);
    r[23] = static_cast<uint8_t>(_seq >> 4);
    r[24] = 3;              /* Block Ack */
    r[25] = 1;              /* ADDBA response */
    r[26] = f.data[26];     /* dialog token */
    r[27] = 37;             /* status: declined */
    r[28] = 0;
    r[29] = f.data[27];     /* the parameter set asked for */
    r[30] = f.data[28];
    send_frame(r, sizeof(r), true);
    _addba_declined++;
  }

  Mt7902 &_dev;
  PipeServer &_pipe;
  RateLadder _ladder;
  std::mutex _mutex;
  std::deque<ClientMsg> _queue;
  std::map<unsigned, bool> _inflight;
  std::vector<uint8_t> _msg;
  Mac _own{}, _bssid{};
  bool _own_set = false, _bssid_set = false;
  uint16_t _seq = 0;
  bool _peer_heard_here = false;
  uint8_t _peer_seq[16] = {};
  unsigned _peer5g = 0;
  awdl::PeerCountry _peer_country;
  uint64_t _rx = 0, _rx_awdl = 0, _rx_to_us = 0, _rx_to_us_retry = 0;
  uint64_t _tx = 0, _tx_acked = 0, _tx_failed = 0, _tx_refused = 0, _addba_declined = 0;
  std::vector<Peer> _peers;
  std::map<std::string, Mt7902::PeerCaps> _caps;
  std::vector<uint8_t> _qos;
  uint64_t _tx_sta = 0, _rx_ba_sessions = 0, _tx_ba_sessions = 0;
  Clock::time_point _stalled_since{};
  uint64_t _rx_to_us_pm = 0;

  /* The Wi-Fi station (Concurrent mode); _sta drives _sta_dev, so it goes first. */
  std::unique_ptr<MtStation> _sta_dev;
  std::unique_ptr<WifiStation> _sta;
  WifiStation::State _published_wifi = WifiStation::State::Idle;
  std::vector<uint8_t> _rewritten;
  std::map<std::string, BssSeen> _bss;
  AwClock _aw;
  awdl_plan::InfraMeter _meter;
  uint8_t _away[16] = {};
  bool _timeshare = false, _ps_asked = false;
  Clock::time_point _ps_at{}, _running_since{};
  uint64_t _last_awdl_data_us = 0;
  uint64_t _here_us = 0, _here_total_us = 0, _here_last_us = 0;
  uint64_t _moves = 0, _move_us = 0, _move_us_max = 0, _tx_stale = 0;
  unsigned _move_fail_logged = 0;
  bool _move_failed = false;
};

/* ---- Self tests on the air ---- */

int test_rx(Mt7902 &dev, unsigned seconds) {
  uint64_t total = 0, beacons = 0, awdl = 0, data = 0, mgmt = 0, ctrl = 0;
  std::map<std::string, std::pair<int, std::string>> aps; /* bssid -> (dbm, ssid) */
  std::set<std::string> awdl_peers;
  std::map<uint8_t, uint64_t> channels;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    total++;
    channels[f.channel]++;
    const uint16_t fc = fc_of(f.data);
    if (is_data(fc)) data++;
    else if (is_mgmt(fc)) mgmt++;
    else ctrl++;
    if (f.len >= 38 && is_beacon(fc)) {
      beacons++;
      std::string ssid;
      if (f.data[36] == 0 && f.data[37] <= 32 && 38u + f.data[37] <= f.len)
        ssid.assign(reinterpret_cast<const char *>(f.data + 38), f.data[37]);
      aps[mac_str(f.data + 16)] = {f.dbm, ssid};
    }
    if (is_awdl(f.data, f.len)) {
      awdl++;
      awdl_peers.insert(mac_str(f.data + 10));
    }
  });
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  while (Clock::now() < end && !g_stop) {
    if (!dev.poll()) Sleep(1);
  }
  say("test-rx: %llu frames in %u s on channel %u: %llu mgmt (%llu beacons, %llu AWDL), %llu data, %llu control",
      total, seconds, dev.channel(), mgmt, beacons, awdl, data, ctrl);
  for (const auto &c : channels) say("test-rx: %llu frames reported on channel %u", c.second, c.first);
  int shown = 0;
  for (const auto &ap : aps) {
    if (shown++ >= 12) break;
    say("test-rx: AP %s %d dBm \"%s\"", ap.first.c_str(), ap.second.first, ap.second.second.c_str());
  }
  for (const auto &p : awdl_peers) say("test-rx: AWDL peer %s", p.c_str());
  return total ? 0 : 3;
}

/* Unicast probe requests to an access point heard on the channel: it
 * acknowledges each one it hears, so the TX reports say whether the frames
 * left the antenna at all. */
int test_tx(Mt7902 &dev, unsigned seconds) {
  std::optional<Mac> ap;
  int best = -128;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    if (f.len >= 24 && is_beacon(fc_of(f.data)) && f.dbm > best) {
      best = f.dbm;
      Mac m;
      std::memcpy(m.data(), f.data + 16, 6);
      ap = m;
    }
  });
  const auto listen_end = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < listen_end && !g_stop)
    if (!dev.poll()) Sleep(1);
  if (!ap) {
    say("test-tx: no access point heard on channel %u", dev.channel());
    return 4;
  }
  say("test-tx: probing %s (%d dBm)", mac_str(ap->data()).c_str(), best);
  uint64_t sent = 0, acked = 0, failed = 0;
  dev.set_tx_status_handler([&](unsigned, bool ok, unsigned) { ok ? acked++ : failed++; });
  const uint8_t *me = dev.mac_address();
  /* The access point's probe responses come to our address: if the card
   * acknowledges them, almost none is a retry. */
  uint64_t responses = 0, retried = 0;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    if (f.len >= 24 && (fc_of(f.data) & 0xfc) == 0x50 && std::memcmp(f.data + 4, me, 6) == 0) {
      responses++;
      retried += (f.data[1] & 0x08) != 0;
    }
  });
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  uint16_t seq = 0;
  auto next = Clock::now();
  while (Clock::now() < end && !g_stop) {
    if (Clock::now() >= next) {
      uint8_t f[24 + 2 + 8] = {0x40, 0x00};
      std::memcpy(f + 4, ap->data(), 6);
      std::memcpy(f + 10, me, 6);
      std::memcpy(f + 16, ap->data(), 6);
      seq = static_cast<uint16_t>((seq + 1) & 0xfff);
      f[22] = static_cast<uint8_t>(seq << 4);
      f[23] = static_cast<uint8_t>(seq >> 4);
      f[24] = 0; f[25] = 0;                     /* wildcard SSID */
      const uint8_t rates[] = {1, 6, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48}; /* 6,9,12,18,24,36 */
      std::memcpy(f + 26, rates, sizeof(rates));
      if (dev.transmit(f, sizeof(f), Mt7902::tx_rate_ofdm(6)) >= 0) sent++;
      next = Clock::now() + std::chrono::milliseconds(20);
    }
    if (!dev.poll()) Sleep(1);
  }
  const auto drain = Clock::now() + std::chrono::milliseconds(500);
  while (Clock::now() < drain) if (!dev.poll()) Sleep(1);
  say("test-tx: %llu probe requests sent, %llu acknowledged, %llu not", sent, acked, failed);
  say("test-tx: %llu probe responses to us, %llu of them retries (%llu%%)", responses, retried,
      responses ? retried * 100 / responses : 0ull);
  say("test-tx: %s", dev.tx_ring_state().c_str());
  std::string types;
  for (unsigned t = 0; t < 32; t++)
    if (dev.rx_types[t]) types += " type" + std::to_string(t) + "=" + std::to_string(dev.rx_types[t]);
  say("test-tx: RX packets by type:%s", types.c_str());
  return acked ? 0 : 5;
}

/* Which kinds of frame the card completes: group action frames (as awdld
 * sends AWDL), with and without NO_ACK, and unicast data on the AC and ALTX
 * queues, to an access point heard here. */
int test_kinds(Mt7902 &dev, unsigned per_kind, int only) {
  std::optional<Mac> ap;
  int best = -128;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    if (f.len >= 24 && is_beacon(fc_of(f.data)) && f.dbm > best) {
      best = f.dbm;
      Mac m;
      std::memcpy(m.data(), f.data + 16, 6);
      ap = m;
    }
  });
  const auto listen_end = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < listen_end && !g_stop)
    if (!dev.poll()) Sleep(1);
  if (!ap) {
    say("test-kinds: no access point heard");
    return 4;
  }
  dev.set_rx_handler(nullptr);
  struct Kind { const char *name; bool group; bool data; int no_ack; int altx; uint64_t sent = 0, ok = 0, bad = 0; };
  Kind kinds[] = {{"group action, NO_ACK", true, false, 1, 1},
                  {"group action, no NO_ACK", true, false, 0, 1},
                  {"unicast data, AC queue", false, true, 0, 0},
                  {"unicast data, ALTX queue", false, true, 0, 1},
                  {"unicast action, ALTX", false, false, 0, 1}};
  std::map<unsigned, int> owner;
  dev.set_tx_status_handler([&](unsigned token, bool ok, unsigned) {
    auto it = owner.find(token);
    if (it == owner.end()) return;
    ok ? kinds[it->second].ok++ : kinds[it->second].bad++;
    owner.erase(it);
  });
  const uint8_t *me = dev.mac_address();
  uint16_t seq = 0;
  for (int k = 0; k < 5; k++) {
    if (only >= 0 && k != only) continue;
    Kind &K = kinds[k];
    dev.force_no_ack = K.no_ack;
    dev.force_altx = K.altx;
    for (unsigned i = 0; i < per_kind && !g_stop; i++) {
      uint8_t f[24 + 8 + 16] = {};
      f[0] = K.data ? 0x08 : 0xd0;
      if (K.data) f[1] = 0x01; /* to DS */
      if (K.group) std::memcpy(f + 4, kAwdlBssid, 6), f[4] = 0x01; else std::memcpy(f + 4, ap->data(), 6);
      std::memcpy(f + 10, me, 6);
      std::memcpy(f + 16, ap->data(), 6);
      seq = static_cast<uint16_t>((seq + 1) & 0xfff);
      f[22] = static_cast<uint8_t>(seq << 4);
      f[23] = static_cast<uint8_t>(seq >> 4);
      if (K.data) { const uint8_t llc[8] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e}; std::memcpy(f + 24, llc, 8); }
      else { f[24] = 127; f[25] = 0x00; f[26] = 0x17; f[27] = 0xf2; f[28] = 0x08; }
      const int t = dev.transmit(f, sizeof(f), Mt7902::tx_rate_ofdm(6));
      if (t >= 0) { owner[static_cast<unsigned>(t)] = k; K.sent++; }
      const auto gap = Clock::now() + std::chrono::milliseconds(15);
      while (Clock::now() < gap) if (!dev.poll()) Sleep(1);
    }
    const auto drain = Clock::now() + std::chrono::milliseconds(800);
    while (Clock::now() < drain) if (!dev.poll()) Sleep(1);
    say("test-kinds: %-26s sent %llu, completed ok %llu, failed %llu, never reported %llu", K.name, K.sent, K.ok,
        K.bad, K.sent - K.ok - K.bad);
  }
  dev.force_no_ack = dev.force_altx = -1;
  say("test-kinds: %s", dev.tx_ring_state().c_str());
  return 0;
}

/* The station path against an access point heard here: a STA record for
 * it, QoS data through it at the firmware's rate, and the Block Ack
 * commands accepted (the AP never agreed to one, so they are undone). */
int test_sta(Mt7902 &dev, unsigned frames) {
  std::optional<Mac> ap;
  int best = -128;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    if (f.len >= 24 && is_beacon(fc_of(f.data)) && f.dbm > best) {
      best = f.dbm;
      Mac m;
      std::memcpy(m.data(), f.data + 16, 6);
      ap = m;
    }
  });
  const auto listen_end = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < listen_end && !g_stop)
    if (!dev.poll()) Sleep(1);
  dev.set_rx_handler(nullptr);
  if (!ap) {
    say("test-sta: no access point heard");
    return 4;
  }
  Mt7902::PeerCaps caps;
  const bool added = dev.sta_add(1, ap->data(), caps);
  say("test-sta: STA record for %s (%d dBm): %s", mac_str(ap->data()).c_str(), best, added ? "accepted" : "refused");
  if (!added) return 6;
  uint64_t sent = 0, ok = 0, bad = 0;
  dev.set_tx_status_handler([&](unsigned, bool acked, unsigned) { acked ? ok++ : bad++; });
  const uint8_t *me = dev.mac_address();
  for (unsigned i = 0; i < frames && !g_stop; i++) {
    uint8_t f[26 + 8 + 1200] = {};
    f[0] = 0x88; /* QoS data */
    f[1] = 0x01; /* to DS */
    std::memcpy(f + 4, ap->data(), 6);
    std::memcpy(f + 10, me, 6);
    std::memcpy(f + 16, ap->data(), 6);
    const uint16_t seq = static_cast<uint16_t>(i + 1);
    f[22] = static_cast<uint8_t>(seq << 4);
    f[23] = static_cast<uint8_t>(seq >> 4);
    const uint8_t llc[8] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e};
    std::memcpy(f + 26, llc, 8);
    if (dev.transmit_sta(f, sizeof(f), 1) >= 0) sent++;
    const auto gap = Clock::now() + std::chrono::milliseconds(10);
    while (Clock::now() < gap) if (!dev.poll()) Sleep(1);
  }
  const auto drain = Clock::now() + std::chrono::milliseconds(800);
  while (Clock::now() < drain) if (!dev.poll()) Sleep(1);
  say("test-sta: %llu QoS data frames at the firmware's rate, %llu acknowledged, %llu not, %llu unreported", sent, ok,
      bad, sent - ok - bad);
  const bool rx_ba = dev.sta_ba(1, ap->data(), 0, 1, 64, false, true) && dev.sta_ba(1, ap->data(), 0, 0, 0, false, false);
  const bool tx_ba = dev.sta_ba(1, ap->data(), 0, static_cast<uint16_t>(frames + 1), 64, true, true) && dev.sta_ba(1, ap->data(), 0, 0, 0, true, false);
  say("test-sta: Block Ack commands: receive %s, send %s", rx_ba ? "accepted" : "refused", tx_ba ? "accepted" : "refused");
  say("test-sta: station removed: %s", dev.sta_remove(1, ap->data()) ? "yes" : "no");
  return ok ? 0 : 7;
}

/* How long a channel move takes, between the given channel and the other
 * 5 GHz block (the station/AWDL time sharing pays it twice an entry). */
int test_switch(Mt7902 &dev, unsigned moves) {
  const uint8_t a = dev.channel(), b = a == 100 ? 44 : 100;
  const uint8_t wa = dev.width_mhz();
  /* Frames heard after each move, and on which channel the card says. */
  uint64_t heard = 0, heard_there = 0;
  uint8_t target = 0;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    heard++;
    heard_there += f.channel == target;
  });
  struct Way {
    const char *name;
    bool sniffer, wait;
  };
  const Way ways[] = {{"switch + sniffer, waiting", true, true},
                      {"switch only, waiting", false, true},
                      {"switch + sniffer, not waiting", true, false},
                      {"switch only, not waiting", false, false}};
  for (const Way &w : ways) {
    dev.move_sniffer = w.sniffer;
    dev.move_wait = w.wait;
    std::vector<uint64_t> total;
    uint64_t steps[3] = {}, failed = 0;
    heard = heard_there = 0;
    for (unsigned i = 0; i < moves && !g_stop; i++) {
      target = i % 2 ? a : b;
      const auto t0 = Clock::now();
      if (!dev.set_channel(target, target > 14 ? wa : 20)) failed++;
      total.push_back(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()));
      for (int k = 0; k < 3; k++) steps[k] += dev.last_move_us[k];
      const auto listen = Clock::now() + std::chrono::milliseconds(120);
      while (Clock::now() < listen)
        if (!dev.poll()) Sleep(1);
    }
    std::sort(total.begin(), total.end());
    const size_t n = total.size();
    say("test-switch: %-30s %zu moves %u<->%u: median %llu us, 90%% %llu us, longest %llu us (RF %llu, sniffer %llu, "
        "timing %llu us average); %llu frames heard after the moves, %llu%% on the channel moved to; %llu failed",
        w.name, n, a, b, n ? total[n / 2] : 0ull, n ? total[n * 9 / 10] : 0ull, n ? total[n - 1] : 0ull,
        n ? steps[0] / n : 0ull, n ? steps[1] / n : 0ull, n ? steps[2] / n : 0ull, heard,
        heard ? heard_there * 100 / heard : 0ull, failed);
  }
  dev.move_sniffer = dev.move_wait = true;
  return 0;
}

/* What the card hears while it visits channels as a Wi-Fi scan does: as
 * brought up, then with AWDL's identity set (as awdld does), each with and
 * without a probe request after every move. */
int test_scan(Mt7902 &dev) {
  const uint8_t home = dev.channel(), home_w = dev.width_mhz();
  const uint8_t chans[] = {36, 44, 100, 108, 149, 157, 1, 6};
  uint64_t frames = 0, beacons = 0, wrong = 0;
  uint8_t target = 0;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    frames++;
    beacons += f.len >= 36 && ((fc_of(f.data) & 0xfc) == 0x80 || (fc_of(f.data) & 0xfc) == 0x50);
    wrong += f.channel != target;
  });
  const auto visit = [&](const char *name, bool probe) {
    frames = beacons = wrong = 0;
    std::string per;
    for (uint8_t ch : chans) {
      const uint64_t before = frames;
      target = ch;
      dev.set_channel(ch, 20);
      if (probe) {
        uint8_t f[24 + 2 + 10] = {0x40};
        std::memset(f + 4, 0xff, 6);
        std::memcpy(f + 10, dev.mac_address(), 6);
        std::memset(f + 16, 0xff, 6);
        f[24] = 0;
        f[25] = 0;
        const uint8_t rates[] = {1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
        std::memcpy(f + 26, rates, sizeof(rates));
        dev.force_no_ack = 1;
        dev.transmit(f, sizeof(f), Mt7902::tx_rate_ofdm(6));
        dev.force_no_ack = -1;
      }
      const auto end = Clock::now() + std::chrono::milliseconds(150);
      while (Clock::now() < end)
        if (!dev.poll()) Sleep(1);
      per += " " + std::to_string(ch) + ":" + std::to_string(frames - before);
    }
    dev.set_channel(home, home_w);
    say("test-scan: %-34s %llu frames, %llu beacons, %llu on another channel (per channel:%s)", name, frames, beacons,
        wrong, per.c_str());
  };
  visit("as brought up, listening only", false);
  visit("as brought up, probing", true);
  uint8_t own[6] = {0x42, 0x9a, 0xa1, 0x2c, 0x2f, 0xc5};
  say("test-scan: AWDL identity set: %s", dev.set_identity(own, kAwdlBssid) ? "yes" : "no");
  visit("with AWDL's identity, listening only", false);
  visit("with AWDL's identity, probing", true);
  dev.move_wait = false;
  visit("identity, probing, not waiting", true);
  dev.move_wait = true;
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const auto opt = parse(argc, argv);
  if (!opt) {
    std::fprintf(stderr,
                 "usage: %s <channel> [--width 20|40|80] [--country CC] [--fw-dir DIR] "
                 "[--data-rate auto|MCSn|OFDMn] [--test-rx S] [--test-tx S]\n",
                 argv[0]);
    return 2;
  }
  SetConsoleCtrlHandler(on_console, TRUE);
  timeBeginPeriod(1);
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  Mt7902 dev;
  dev.set_logger([](const std::string &l) { std::printf("radio: %s\n", l.c_str()); std::fflush(stdout); });
  dev.set_event_handler([](const uint8_t *d, size_t len) {
    /* Unsolicited firmware events (and answers nobody waited for). */
    if (len < sizeof(mt::McuRxd)) return;
    const auto *r = reinterpret_cast<const mt::McuRxd *>(d);
    /* MCU_EVENT_COREDUMP (0xf0): the firmware asserted. It no longer answers
     * commands nor reports sent frames; only a fresh start brings it back. */
    if (r->eid == 0xf0 && !g_card_dead) {
      const std::string text(reinterpret_cast<const char *>(d) + sizeof(mt::McuRxd), len - sizeof(mt::McuRxd));
      if (text.find("ASSERT") != std::string::npos) {
        say("firmware crashed (%.100s): restarting the radio", text.c_str());
        if (g_stations_up) {
          if (FILE *flag = std::fopen(crash_flag_path(), "w")) std::fclose(flag);
          say("peer stations off for 30 minutes after this crash");
        }
        g_card_dead = true;
        g_stop = true;
      }
    }
    static unsigned shown = 0;
    if (shown++ < 60) {
      std::string text;
      for (size_t i = sizeof(mt::McuRxd); i < len && text.size() < 160; i++)
        text += (d[i] >= 32 && d[i] < 127) ? static_cast<char>(d[i]) : '.';
      say("fw-event: eid %02x ext %02x seq %u option %02x len %zu %s", r->eid, r->ext_eid, r->seq, r->option, len,
          text.c_str());
    }
  });
  const std::string fw = opt->fw_dir.empty() ? find_firmware_dir() : opt->fw_dir;
  std::string error;
  if (fw.empty()) error = "no MT7902 firmware found (WIFI_RAM_CODE_MT7902_1.bin)";
  else if (!dev.open(fw, error)) {}
  else if (!dev.radio_start(opt->channel, opt->width, opt->country, error)) {}
  if (!error.empty()) {
    say("bring-up failed: %s", error.c_str());
    dev.close();
    return 1;
  }
  say("MT7902 firmware %s, MAC %s", dev.firmware_version().c_str(), mac_str(dev.mac_address()).c_str());

  int rc = 0;
  if (opt->test_rx) rc = test_rx(dev, opt->test_rx);
  else if (opt->test_tx) rc = test_tx(dev, opt->test_tx);
  else if (opt->test_kinds) rc = test_kinds(dev, opt->test_kinds, opt->only);
  else if (opt->test_sta) rc = test_sta(dev, opt->test_sta);
  else if (opt->test_switch) rc = test_switch(dev, opt->test_switch);
  else if (opt->test_scan) rc = test_scan(dev);
  else {
    PipeServer pipe(RADIO_LINK_PIPE_NAME, [] { return g_stop.load(); });
    if (!pipe.valid()) {
      say("bring-up failed: cannot create %s (%lu): already running?", RADIO_LINK_PIPE_NAME, GetLastError());
      dev.close();
      return 1;
    }
    Relay relay(dev, pipe, make_ladder(opt->data_rate));
    relay.channel = opt->channel;
    relay.width = dev.width_mhz();
    relay.aggregate = opt->aggregate;
    if (relay.aggregate && recent_station_crash()) {
      relay.aggregate = false;
      say("peer stations off: the firmware crashed with one less than 30 minutes ago");
    }
    std::thread reader([&] {
      while (!g_stop && pipe.wait_for_client()) {
        radio_link_hello hello{RADIO_LINK_VERSION, relay.channel, relay.width, {}};
        pipe.send(RADIO_LINK_HELLO, {reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)});
        say("client connected");
        radio_link_header h{};
        while (!g_stop && pipe.read_exact(&h, sizeof(h))) {
          if (h.length > RADIO_LINK_MAX_PAYLOAD) break;
          ClientMsg m{h.type, std::vector<uint8_t>(h.length)};
          if (h.length && !pipe.read_exact(m.payload.data(), h.length)) break;
          relay.push(std::move(m));
        }
        relay.client_gone();
        pipe.drop_client();
        say("client gone");
      }
    });
    say("ready on channel %u, waiting for a client", opt->channel);
    /* The Wi-Fi station (Concurrent mode). */
    std::thread tap_thread;
    if (!opt->sta_config.empty()) {
      StaConfig cfg;
      std::string err;
      const bool loaded = cfg.load(opt->sta_config, err);
      /* The file holds the Wi-Fi password: gone once read. */
      DeleteFileA(opt->sta_config.c_str());
      if (!loaded) {
        say("station config: %s", err.c_str());
        say("event: wifi failed");
      } else if (relay.start_station(cfg)) {
        WifiStation *sta = relay.station();
        tap_thread = std::thread([&relay, sta] {
          std::vector<uint8_t> eth;
          while (!g_stop) {
            if (!sta->tap().read(eth, 200)) {
              Sleep(20);
              continue;
            }
            if (!eth.empty()) relay.push({kStaEthernet, eth});
          }
        });
      }
    }
    /* The service's Wi-Fi scans and rejoins, as the Intel radio takes them. */
    std::thread([&relay] {
      char line[128];
      while (!g_stop && std::fgets(line, sizeof(line), stdin)) {
        if (std::strncmp(line, "rejoin", 6) == 0) {
          relay.push({kRejoinRequest, {}});
          continue;
        }
        if (std::strncmp(line, "scan", 4) != 0) continue;
        std::vector<uint8_t> ssid;
        for (const char *h = line + 4; *h;) {
          if (*h == ' ') {
            h++;
            continue;
          }
          if (!std::isxdigit(static_cast<unsigned char>(h[0])) || !std::isxdigit(static_cast<unsigned char>(h[1]))) break;
          const char b[3] = {h[0], h[1], 0};
          if (ssid.size() < 32) ssid.push_back(static_cast<uint8_t>(std::strtoul(b, nullptr, 16)));
          h += 2;
        }
        relay.push({kScanRequest, ssid});
      }
    }).detach();
    std::thread timer;
    if (opt->run_for)
      timer = std::thread([secs = opt->run_for] {
        const auto end = Clock::now() + std::chrono::seconds(secs);
        while (!g_stop && Clock::now() < end) Sleep(100);
        g_stop = true;
      });
    relay.run();
    relay.report();
    g_stop = true;
    reader.join();
    if (tap_thread.joinable()) tap_thread.join();
    if (timer.joinable()) timer.join();
    if (WifiStation *sta = relay.station()) sta->stop();
  }
  dev.close();
  timeEndPeriod(1);
  return g_card_dead ? 3 : rc;
}
