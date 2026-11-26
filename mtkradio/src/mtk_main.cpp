// SPDX-License-Identifier: GPL-2.0-only
/*
 * mtkradio: user-mode MediaTek MT7922 / MT7921 radio daemon for nekodrop / winawdl.
 *
 * Modes:
 *   mtkradio --probe                     Safe test: open regproxy, read chip ID & driver-own
 *   mtkradio --reset                     CBTOP RGU whole-path Wi-Fi subsystem reset
 *   mtkradio --boot-test [ch] [--width]  Boot firmware, tune, sniff 802.11/AWDL frames for 10s
 *   mtkradio <channel> [--width 20|40|80] [--country CC] [--firmware DIR]
 *            [--sta-config FILE] [--run-for SECONDS] [--verbose]
 *                                        Serve \\.\pipe\winawdl-radio for awdld / nekodrop
 *
 * With --sta-config the radio also runs a 5 GHz Wi-Fi station beside AWDL
 * (Concurrent mode): iwxradio's WifiStation through MtkStation (mtk_station.h)
 * with mtradio's host-side CCMP. When the router is on another channel than
 * AWDL, the scheduler time-shares the single channel in step with awdld's
 * 16-entry channel sequence, naming the router's channel in the away entries
 * (iwxradio's coex_plan.h). stdin takes "scan [SSID hex]" and "rejoin" from
 * NekoDropService.
 */
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
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "awdl_policy.h"
#include "awdl_schedule.h"
#include "coex_plan.h"
#include "mtk_dev.h"
#include "mtk_station.h"
#include "pipe_server.h"
#include "plat.h"
#include "radio_link.h"
#include "rx_ba_reorder.h"
#include "wifi_sta.h"

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};
bool stopping() { return g_stop.load(); }

BOOL WINAPI on_console(DWORD) {
  g_stop = true;
  return TRUE;
}

void log_line(const std::string &line) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(stderr, "%02u:%02u:%02u.%03u mtkradio: %s\n", t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, line.c_str());
  std::fflush(stderr);
}

void log_fmt(const char *fmt, ...) {
  char buf[768];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_line(buf);
}

std::string mac_str(const uint8_t *m) {
  char s[18];
  std::snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return s;
}

std::string exe_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string s = path;
  const size_t slash = s.find_last_of("\\/");
  return slash == std::string::npos ? "." : s.substr(0, slash);
}

uint16_t channel_to_mhz(uint8_t ch) {
  if (ch >= 1 && ch <= 13) return static_cast<uint16_t>(2407 + ch * 5);
  if (ch == 14) return 2484;
  return static_cast<uint16_t>(5000 + ch * 5);
}

/* Minimal 18-byte radiotap header carrying channel + dBm signal for awdld. */
#pragma pack(push, 1)
struct MinimalRadiotap {
  uint8_t version;
  uint8_t pad;
  uint16_t length;
  uint32_t present; /* bit 3 = Channel, bit 5 = Antenna signal (dBm) */
  uint16_t freq_mhz;
  uint16_t flags;
  int8_t dbm;
  uint8_t pad2;
};
#pragma pack(pop)

void append_rx_wire(const mtk::RxFrame &f, std::vector<uint8_t> &batch) {
  MinimalRadiotap rt{};
  rt.version = 0;
  rt.length = sizeof(rt);
  rt.present = (1u << 3) | (1u << 5);
  rt.freq_mhz = channel_to_mhz(f.channel);
  rt.flags = (f.channel <= 14) ? 0x00a0u : 0x0140u; /* 2 GHz CCK/OFDM vs 5 GHz OFDM */
  rt.dbm = f.dbm;

  const radio_link_header hdr{static_cast<uint16_t>(RADIO_LINK_RX), 0,
                              static_cast<uint32_t>(sizeof(rt) + f.len)};
  const size_t old_sz = batch.size();
  batch.resize(old_sz + sizeof(hdr) + sizeof(rt) + f.len);
  uint8_t *dst = batch.data() + old_sz;
  std::memcpy(dst, &hdr, sizeof(hdr));
  std::memcpy(dst + sizeof(hdr), &rt, sizeof(rt));
  std::memcpy(dst + sizeof(hdr) + sizeof(rt), f.data, f.len);
}

/* Split an 802.11 QoS Data A-MSDU (QoS Control bit 7 = 0x80) into standard
 * non-A-MSDU QoS Data MPDUs for awdld (owl does not de-aggregate A-MSDUs). */
void append_rx_deamsdu(const mtk::RxFrame &f, std::vector<uint8_t> &batch) {
  if (f.len >= 26 && (f.data[0] & 0x8cu) == 0x88u) {
    const uint16_t fc = static_cast<uint16_t>(f.data[0] | (f.data[1] << 8));
    const size_t qos_off = ((fc & 0x0300u) == 0x0300u) ? 30u : 24u;
    const size_t hdr_len = qos_off + 2u + ((fc & 0x8000u) ? 4u : 0u);
    if (f.len > hdr_len + 14u && (f.data[qos_off] & 0x80u) != 0) {
      std::vector<uint8_t> mpdu;
      size_t off = hdr_len;
      unsigned emitted = 0;
      while (off + 14u <= f.len) {
        const size_t sub_len = (static_cast<size_t>(f.data[off + 12]) << 8) | f.data[off + 13];
        if (sub_len == 0 || off + 14u + sub_len > f.len) break;
        mpdu.resize(hdr_len + sub_len);
        std::memcpy(mpdu.data(), f.data, hdr_len);
        mpdu[qos_off] &= static_cast<uint8_t>(~0x80u); /* clear A-MSDU Present */
        std::memcpy(mpdu.data() + hdr_len, f.data + off + 14u, sub_len);
        append_rx_wire({mpdu.data(), mpdu.size(), f.dbm, f.channel}, batch);
        emitted++;
        const size_t step = (14u + sub_len + 3u) & ~size_t(3u);
        if (off + step > f.len) break;
        off += step;
      }
      if (emitted > 0) return;
    }
  }
  append_rx_wire(f, batch);
}

bool is_awdl_bssid(const uint8_t *frame, size_t len) {
  if (len < 22) return false;
  static constexpr uint8_t kAwdl[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
  return std::memcmp(frame + 16, kAwdl, 6) == 0;
}

/* Local messages alongside radio_link_type. */
constexpr uint16_t kStaEthernet = 0x100;   /* Ethernet frame from the TAP adapter */
constexpr uint16_t kScanRequest = 0x101;   /* "scan [SSID hex]" on stdin */
constexpr uint16_t kRejoinRequest = 0x102; /* "rejoin" on stdin */
constexpr uint16_t kBackgroundScan = 0x103; /* "scan-bg [SSID hex]" on stdin */

struct PendingCmd {
  uint16_t type = 0;
  std::vector<uint8_t> payload;
  uint64_t at_us = 0;
};

/* Networks heard in beacons and probe responses, for NekoDropService's Wi-Fi list. */
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
      const unsigned pc = r[o] | (r[o + 1] << 8);
      o += 2 + 4u * pc;
      if (o + 2 <= n) {
        const unsigned ac = r[o] | (r[o + 1] << 8);
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

/* Our AWDL schedule's clock, tracked from the sync TLV (4) of outgoing awdld frames. */
struct AwClock {
  bool valid = false;
  double aw_us = 16 * 1024.0;
  double base_us = 0;
  uint64_t last_note_us = 0;

  void note(uint64_t t_us, unsigned counter, unsigned period_tu) {
    if (period_tu < 8 || period_tu > 64) return;
    aw_us = period_tu * 1024.0;
    const double candidate = static_cast<double>(t_us) - counter * aw_us;
    if (!valid || candidate < base_us || candidate - base_us > 1.5 * aw_us ||
        t_us - last_note_us > 5000000) {
      base_us = candidate;
    }
    valid = true;
    last_note_us = t_us;
  }

  unsigned entry(uint64_t t_us) const {
    const double aws = (static_cast<double>(t_us) - (valid ? base_us : 0.0)) / aw_us;
    const int64_t aw = static_cast<int64_t>(aws >= 0 ? aws : aws - 1);
    return static_cast<unsigned>(((aw / 4) % 16 + 16) % 16);
  }
};

bool frame_sync(const uint8_t *f, size_t len, unsigned &counter, unsigned &period_tu) {
  for (size_t i = 40; i + 3 <= len;) {
    const size_t n = static_cast<size_t>(f[i + 1]) | (static_cast<size_t>(f[i + 2]) << 8);
    if (i + 3 + n > len) return false;
    const uint8_t *v = f + i + 3;
    if (f[i] == 4 && n >= 31) {
      period_tu = static_cast<unsigned>(v[5] | (v[6] << 8));
      counter = static_cast<unsigned>(v[29] | (v[30] << 8));
      return true;
    }
    i += 3 + n;
  }
  return false;
}

/* While AirDrop moves no data, Wi-Fi gets 12 of 16 entries (AWDL keeps 4 to stay discoverable). */
constexpr uint8_t kNoPeerAway[16] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

} // namespace

int main(int argc, char **argv) {
  SetConsoleCtrlHandler(on_console, TRUE);
  timeBeginPeriod(1);

  bool probe_only = false;
  bool reset_only = false;
  bool boot_test = false;
  bool no_block_ack = false;
  bool he80 = false;
  uint8_t channel = 149;
  unsigned width = 80;
  unsigned run_for = 0;
  std::string sta_config_path;
  mtk::MtkDevice::Options dev_opts;
  dev_opts.firmware_dir = exe_dir() + "\\firmware";

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const auto next_val = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--probe") {
      probe_only = true;
    } else if (a == "--reset") {
      reset_only = true;
    } else if (a == "--boot-test") {
      boot_test = true;
    } else if (a == "--width") {
      width = static_cast<unsigned>(std::atoi(next_val().c_str()));
    } else if (a == "--firmware" || a == "--fw-dir") {
      dev_opts.firmware_dir = next_val();
    } else if (a == "--country") {
      dev_opts.country = next_val();
    } else if (a == "--sta-config") {
      sta_config_path = next_val();
    } else if (a == "--run-for") {
      run_for = static_cast<unsigned>(std::atoi(next_val().c_str()));
    } else if (a == "--verbose") {
      dev_opts.verbose = true;
    } else if (a == "--no-block-ack") {
      no_block_ack = true;
    } else if (a == "--he80") {
      he80 = true;
    } else if (a == "--tap" || a == "--profile" || a == "--mac" || a == "--pipe") {
      (void)next_val(); /* accepted for compatibility with NekoDropService */
    } else if (!a.empty() && a[0] >= '0' && a[0] <= '9') {
      const int ch = std::atoi(a.c_str());
      if (ch > 0 && ch <= 233) channel = static_cast<uint8_t>(ch);
    }
  }
  if (channel <= 14) width = 20;

  mtk::MtkDevice dev(log_line);
  dev.set_stop_check(stopping);
  std::string err;

  if (probe_only) {
    if (!dev.probe(err)) {
      log_line("PROBE FAILED: " + err);
      return 1;
    }
    log_line("PROBE OK! MTK chip ready for bring-up.");
    return 0;
  }

  if (reset_only) {
    if (!dev.hard_reset(err)) {
      log_line("RESET FAILED: " + err);
      return 1;
    }
    return 0;
  }

  if (!dev.open(dev_opts, err)) {
    log_line("bring-up failed: " + err);
    return 1;
  }

  /* Default locally-administered MAC until awdld sends RADIO_LINK_SET_ADDRESS. */
  const uint8_t default_mac[6] = {0x02, 0x79, 0x22, 0xaa, 0xbb, 0xcc};
  if (!dev.start_monitor(channel, width, default_mac)) {
    log_line("bring-up failed: start_monitor failed on ch " + std::to_string(channel));
    return 1;
  }

  if (boot_test) {
    log_line("boot-test: sniffing ch " + std::to_string(channel) + " for 10 seconds...");
    const uint8_t test_awdl_frame[36] = {
        0xd0, 0x00, 0x00, 0x00,                         /* Action (No-Ack) */
        0x00, 0x25, 0x00, 0xff, 0x94, 0x73,             /* DA: AWDL multicast */
        0x02, 0x79, 0x22, 0xaa, 0xbb, 0xcc,             /* SA: our local MAC */
        0x00, 0x25, 0x00, 0xff, 0x94, 0x73,             /* BSSID: AWDL BSSID */
        0x10, 0x00,                                     /* SeqCtl = 1 */
        0x7f, 0x00, 0x17, 0xf2, 0x08, 0x00, 0x00, 0x00, /* Apple AWDL OUI header */
        0x00, 0x00, 0x00, 0x00};
    const bool tx_ok = dev.transmit(test_awdl_frame, sizeof(test_awdl_frame), 6);
    if (dev_opts.verbose) {
      log_line(std::string("test TX injection: ") + (tx_ok ? "queued OK" : "FAILED"));
    }
    uint64_t rx_total = 0, rx_awdl = 0;
    const uint64_t end_ms = GetTickCount64() + 10000;
    while (!stopping() && GetTickCount64() < end_ms) {
      const size_t n = dev.poll([&](const mtk::RxFrame &f) {
        rx_total++;
        if (is_awdl_bssid(f.data, f.len)) rx_awdl++;
      });
      if (n == 0) Sleep(1);
    }
    log_line("boot-test done: total_80211=" + std::to_string(rx_total) +
             " awdl_frames=" + std::to_string(rx_awdl));
    dev.close();
    return 0;
  }

  /* Full winawdl radio link daemon mode */
  PipeServer pipe(RADIO_LINK_PIPE_NAME, stopping, 1u << 19);
  if (!pipe.valid()) {
    log_line("bring-up failed: cannot create named pipe " RADIO_LINK_PIPE_NAME);
    dev.close();
    return 1;
  }

  /* Runtime channel moves and STA record commands use bounded timeouts so
   * timesharing and scans never stall the radio for seconds. MT7921 (0x7961)
   * needs > 500 ms when entering a new 80 MHz calibration subband (e.g. ch 36 /
   * 52 / 100 / 116 / 132), and timing out early causes overlapping switches (#72). */
  const bool is_mt7921 = (dev.chip_id() == 0x7961);
  dev.move_timeout_ms = is_mt7921 ? 1500u : 500u;
  dev.sta_cmd_timeout_ms = is_mt7921 ? 1000u : 500u;

  std::mutex cmd_mu;
  std::deque<PendingCmd> cmds;
  auto push_cmd = [&](PendingCmd m) {
    std::lock_guard<std::mutex> lk(cmd_mu);
    if (cmds.size() < 4096) cmds.push_back(std::move(m));
  };

  std::array<uint8_t, 6> bssid_filter{};
  bool filter_active = false;
  uint8_t awdl_channel = channel;

  std::thread reader([&]() {
    while (!stopping()) {
      if (!pipe.wait_for_client()) continue;
      log_line("awdld client connected");

      radio_link_hello hello{RADIO_LINK_VERSION, awdl_channel,
                             static_cast<uint8_t>(awdl_channel <= 14 ? 20 : width),
                             {pipe.shm_cap_flag(), 0}};
      pipe.send(RADIO_LINK_HELLO,
                std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)));

      while (!stopping() && pipe.connected()) {
        radio_link_header hdr{};
        if (!pipe.read_exact(&hdr, sizeof(hdr))) break;
        if (hdr.length > RADIO_LINK_MAX_PAYLOAD) break;
        std::vector<uint8_t> payload(hdr.length);
        if (hdr.length && !pipe.read_exact(payload.data(), hdr.length)) break;
        push_cmd({hdr.type, std::move(payload), now_us()});
      }
      log_line("awdld client disconnected");
      pipe.drop_client();
    }
  });

  log_line("ready on channel " + std::to_string(channel) + ", waiting for a client");

  /* Concurrent Wi-Fi station (optional --sta-config). */
  std::unique_ptr<MtkStation> sta_dev;
  std::unique_ptr<WifiStation> sta;
  WifiStation::State published_wifi = WifiStation::State::Idle;
  Clock::time_point running_since{};
  std::thread tap_thread;

  uint64_t awdl_tx_acked = 0, awdl_tx_failed = 0;
  dev.set_tx_status_handler([&](unsigned token, bool acked, unsigned) {
    if (sta_dev && sta_dev->on_tx_status(token, acked)) return;
    if (acked) awdl_tx_acked++;
    else awdl_tx_failed++;
  });

  if (!sta_config_path.empty()) {
    StaConfig cfg;
    std::string cfg_err;
    const bool loaded = cfg.load(sta_config_path, cfg_err);
    /* The file holds the Wi-Fi password: remove once read. */
    DeleteFileA(sta_config_path.c_str());
    if (!loaded) {
      log_line("station config: " + cfg_err);
      log_line("event: wifi failed");
    } else if (cfg.channel <= 14) {
      log_fmt("station: \"%s\" is on 2.4 GHz (channel %u); MT7922/MT7921 Concurrent mode joins only 5 GHz networks beside AirDrop",
              cfg.ssid.c_str(), cfg.channel);
      log_line("event: wifi failed");
    } else {
      cfg.block_ack = true;
      sta_dev = std::make_unique<MtkStation>(dev, log_line);
      sta = std::make_unique<WifiStation>(*sta_dev, log_line);
      if (!sta->start(cfg, cfg_err)) {
        log_line("station: " + cfg_err);
        log_line("event: wifi failed");
        sta.reset();
        sta_dev.reset();
      } else {
        log_fmt("station: joining \"%s\" on channel %u, AWDL on %u%s", cfg.ssid.c_str(),
                cfg.channel, awdl_channel,
                cfg.channel == awdl_channel ? " (one channel for both)"
                                            : " (the card moves between the two)");
        WifiStation *sta_ptr = sta.get();
        tap_thread = std::thread([&push_cmd, sta_ptr] {
          std::vector<uint8_t> eth;
          while (!stopping()) {
            if (!sta_ptr->tap().read(eth, 200)) {
              Sleep(20);
              continue;
            }
            if (!eth.empty()) push_cmd({kStaEthernet, std::move(eth), now_us()});
          }
        });
      }
    }
  }

  /* Service commands on stdin: "scan[-bg] [SSID hex]", "rejoin", and "quit". */
  std::thread([&push_cmd] {
    char line[128];
    while (!stopping() && std::fgets(line, sizeof(line), stdin)) {
      if (std::strncmp(line, "quit", 4) == 0) {
        g_stop = true;
        break;
      }
      if (std::strncmp(line, "rejoin", 6) == 0) {
        push_cmd({kRejoinRequest, {}, now_us()});
        continue;
      }
      if (std::strncmp(line, "scan", 4) != 0) continue;
      const bool background = std::strncmp(line + 4, "-bg", 3) == 0;
      std::vector<uint8_t> ssid;
      for (const char *h = line + (background ? 7 : 4); *h;) {
        if (*h == ' ') {
          h++;
          continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(h[0])) ||
            !std::isxdigit(static_cast<unsigned char>(h[1]))) {
          break;
        }
        const char b[3] = {h[0], h[1], 0};
        if (ssid.size() < 32) ssid.push_back(static_cast<uint8_t>(std::strtoul(b, nullptr, 16)));
        h += 2;
      }
      push_cmd({background ? kBackgroundScan : kScanRequest, std::move(ssid), now_us()});
    }
  }).detach();

  std::thread timer;
  if (run_for) {
    timer = std::thread([secs = run_for] {
      const auto end = Clock::now() + std::chrono::seconds(secs);
      while (!stopping() && Clock::now() < end) Sleep(100);
      g_stop = true;
    });
  }

  std::array<uint8_t, 6> own_mac{};
  std::memcpy(own_mac.data(), default_mac, 6);
  uint16_t mgmt_seq = 0;

  /* Peer AWDL schedule & country/channel learning (reused from iwxradio). */
  awdl_sched::PeerSchedule sched{};
  sched.assumeZero = true;
  uint8_t sched_peer[6] = {};
  bool have_sched_peer = false;
  uint64_t gate_since_us = 0;
  uint64_t gate_limit_us = 0;
  uint8_t peer_seq[16] = {};
  unsigned peer5g = 0;
  awdl::PeerCountry peer_country;

  /* Concurrent Wi-Fi timesharing & scan state. */
  AwClock aw_clock;
  awdl_plan::InfraMeter infra_meter;
  uint8_t away[16] = {};
  bool timeshare = false;
  bool ps_asked = false;
  Clock::time_point ps_at{};
  uint64_t last_awdl_data_us = 0;
  uint64_t here_us = 0, here_total_us = 0, here_last_us = 0;
  uint64_t moves = 0, move_us = 0, move_us_max = 0, tx_stale = 0;
  unsigned move_fail_logged = 0;
  bool move_failed = false;
  Clock::time_point move_retry_at{};
  std::vector<uint8_t> rewritten;
  std::map<std::string, BssSeen> bss_map;

  struct Scan {
    bool active = false, ps_done = false, background = false, home = false;
    std::vector<uint8_t> channels, ssid;
    size_t next = 0;
    unsigned visits = 0, home_visits = 0;
    Clock::time_point until{}, started{};
    uint64_t frames = 0, beacons = 0, mgmt = 0, ctrl = 0, data = 0;
    std::map<uint8_t, std::pair<uint64_t, uint64_t>> heard;
  } scan;

  auto station_on_air = [&]() -> bool { return sta && sta_dev && sta_dev->joined(); };

  auto note_bss = [&](const mtk::RxFrame &f) {
    if (f.len < 2) return;
    const uint16_t fc = static_cast<uint16_t>(f.data[0] | (f.data[1] << 8));
    const bool beacon = f.len >= 36 && ((fc & 0xfcu) == 0x80u || (fc & 0xfcu) == 0x50u);
    if (scan.active) {
      scan.frames++;
      scan.beacons += beacon;
      const uint8_t ftype = static_cast<uint8_t>((fc >> 2) & 0x3u);
      if (ftype == 0) scan.mgmt++;
      else if (ftype == 1) scan.ctrl++;
      else if (ftype == 2) scan.data++;
      auto &h = scan.heard[f.channel];
      h.first++;
      h.second += beacon;
    }
    if (!beacon) return;
    const std::string key = mac_str(f.data + 16);
    if (bss_map.size() >= 256 && !bss_map.count(key)) return;
    BssSeen &b = bss_map[key];
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
    b.security = bss_security(ies, n, static_cast<uint16_t>(f.data[34] | (f.data[35] << 8)));
    b.seen = Clock::now();
  };

  auto send_probe = [&](uint8_t ch) {
    std::vector<uint8_t> f(24, 0);
    f[0] = 0x40;
    std::memset(&f[4], 0xff, 6);
    std::memcpy(&f[10], own_mac.data(), 6);
    std::memset(&f[16], 0xff, 6);
    mgmt_seq = static_cast<uint16_t>((mgmt_seq + 1) & 0xfff);
    f[22] = static_cast<uint8_t>(mgmt_seq << 4);
    f[23] = static_cast<uint8_t>(mgmt_seq >> 4);
    f.push_back(0);
    f.push_back(static_cast<uint8_t>(scan.ssid.size()));
    f.insert(f.end(), scan.ssid.begin(), scan.ssid.end());
    if (ch <= 14) f.insert(f.end(), {1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24});
    else f.insert(f.end(), {1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c});
    (void)dev.transmit_raw(f.data(), f.size(), mtk::MtkDevice::tx_rate_ofdm(6), 0, true, true);
  };

  auto start_scan = [&](const std::vector<uint8_t> &ssid, bool background) {
    if (scan.active) return;
    scan = Scan{};
    scan.active = true;
    scan.background = background;
    scan.ssid = ssid;
    scan.started = Clock::now();
    for (uint8_t c = 36; c <= 64; c += 4) scan.channels.push_back(c);
    for (uint8_t c = 100; c <= 144; c += 4) scan.channels.push_back(c);
    for (uint8_t c = 149; c <= 165; c += 4) scan.channels.push_back(c);
    // The MT792x Concurrent station can join only 5 GHz; probing 2.4 GHz
    // here would waste off-channel time producing results the service discards.
    log_line("event: scan-started");
  };

  auto scan_step = [&]() -> bool {
    const auto now = Clock::now();
    // A 9.6 s uninterrupted scan expired the AWDL peer in under 3 s, and
    // cut active uploads in both directions (#54, #56). Check at start and
    // during the home windows, before any further off-channel dwell.
    const uint64_t t = now_us();
    if (scan.background && (scan.next == 0 || scan.home) && last_awdl_data_us &&
        t - last_awdl_data_us < 3000000u) {
      scan.active = false;
      log_line("scan: yielded to AirDrop traffic");
      log_line("event: scan-interrupted air-drop");
      return true;
    }
    if (!scan.ps_done) {
      const bool running = sta && sta->state() == WifiStation::State::Running;
      if (station_on_air() && running && dev.channel() == sta_dev->channel()) {
        if (!ps_asked) {
          sta_dev->send_null(true);
          ps_asked = true;
          ps_at = now;
          return true;
        }
        if (sta_dev->null_pending() && now - ps_at < std::chrono::milliseconds(8)) return false;
      }
      ps_asked = false;
      scan.ps_done = true;
      if (sta_dev) sta_dev->on_channel(false);
    }
    if (scan.until != Clock::time_point{} && now < scan.until) return false;
    if (scan.next >= scan.channels.size()) {
      scan.active = false;
      std::string heard;
      for (const auto &[ch, c] : scan.heard) {
        heard += " " + std::to_string(ch) + ":" + std::to_string(c.first) + "/" +
                 std::to_string(c.second);
      }
      log_fmt("scan: %llu frames (%llu mgmt, %llu ctrl, %llu data), %llu beacons or probe responses, %u AWDL home visits (channel:frames/beacons%s)",
              static_cast<unsigned long long>(scan.frames),
              static_cast<unsigned long long>(scan.mgmt),
              static_cast<unsigned long long>(scan.ctrl),
              static_cast<unsigned long long>(scan.data),
              static_cast<unsigned long long>(scan.beacons), scan.home_visits, heard.c_str());
      unsigned n = 0;
      for (const auto &[bssid, b] : bss_map) {
        if (b.seen < scan.started || b.ssid.empty()) continue;
        std::string hex;
        char h[3];
        for (unsigned char c : b.ssid) {
          std::snprintf(h, sizeof(h), "%02x", c);
          hex += h;
        }
        log_fmt("event: bss %s %u %d %s %s", bssid.c_str(), b.channel, b.dbm, b.security.c_str(),
                hex.c_str());
        n++;
      }
      log_fmt("event: scan-done %u", n);
      return true;
    }
    // Give queued AWDL TX and the peer's sync frames a chance at least every
    // three scan channels. Keep the normal active/passive dwell per channel.
    if (!scan.home && scan.visits >= 3) {
      dev.kick_tx();
      if (dev.channel() != awdl_channel || dev.width_mhz() != (awdl_channel > 14 ? width : 20u)) {
        if (!dev.set_channel(awdl_channel, awdl_channel > 14 ? width : 20u)) {
          scan.active = false;
          move_failed = true;
          move_retry_at = Clock::now() + std::chrono::milliseconds(500);
          log_fmt("scan: could not return to AWDL channel %u", awdl_channel);
          log_line("event: scan-interrupted retune");
          return true;
        }
      }
      scan.visits = 0;
      scan.home_visits++;
      scan.home = true;
      scan.until = Clock::now() + std::chrono::milliseconds(200);
      return true;
    }
    scan.home = false;
    scan.visits++;
    const uint8_t ch = scan.channels[scan.next++];
    dev.kick_tx();
    if (!dev.set_channel(ch, 20, mtk::CH_SWITCH_SCAN_BYPASS_DPD)) {
      log_fmt("scan: could not tune to channel %u; skipping dwell", ch);
      return true;
    }
    const bool radar = ch >= 52 && ch <= 144;
    if (!radar) send_probe(ch);
    scan.until = Clock::now() + std::chrono::milliseconds(radar ? 250 : 150);
    return true;
  };

  auto update_plan = [&](uint64_t t, uint8_t s) {
    const bool moving = last_awdl_data_us && t - last_awdl_data_us < awdl_plan::kDataHoldUS;
    infra_meter.note(t, sta->rx_packets() + sta->tx_packets(), sta->rx_bytes() + sta->tx_bytes());
    uint8_t next_away[16];
    if (!moving ||
        !awdl_plan::plan_entries(true, s, sta_dev->width(), awdl_channel, true, infra_meter.cls,
                                 next_away)) {
      std::memcpy(next_away, kNoPeerAway, 16);
    }
    if (std::memcmp(next_away, away, 16) != 0) {
      std::memcpy(away, next_away, 16);
      unsigned n = 0;
      for (uint8_t a : away) n += a;
      log_fmt("timeshare: %u of 16 entries on the router's channel %u (%s)", n, s,
              moving ? "AirDrop moving data" : "AirDrop idle");
    }
  };

  auto move_to = [&](uint8_t want, unsigned w) -> bool {
    const uint8_t at = dev.channel();
    const bool at_router = station_on_air() && at == sta_dev->channel();
    const uint64_t t = now_us();
    if (here_last_us && sta && sta->state() == WifiStation::State::Running) {
      here_total_us += t - here_last_us;
      if (at_router) here_us += t - here_last_us;
    }
    here_last_us = t;
    if (want == at && w == dev.width_mhz() && !move_failed) {
      if (sta_dev) sta_dev->on_channel(at_router);
      return false;
    }
    if (move_failed && Clock::now() < move_retry_at) {
      return false;
    }
    const bool running = sta && sta->state() == WifiStation::State::Running;
    if (want != at && at_router && sta_dev->associated() && running) {
      if (!ps_asked) {
        sta_dev->send_null(true);
        ps_asked = true;
        ps_at = Clock::now();
        return true;
      }
      if (sta_dev->null_pending() && Clock::now() - ps_at < std::chrono::milliseconds(8)) {
        return false;
      }
    }
    ps_asked = false;
    if (sta_dev) sta_dev->on_channel(false);
    dev.kick_tx();
    const auto t0 = Clock::now();
    const bool ok = dev.set_channel(want, w);
    const uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
    moves++;
    move_us += us;
    if (us > move_us_max) move_us_max = us;
    if (!ok && move_fail_logged++ < 5) log_fmt("channel move to %u failed", want);
    if (ok) {
      move_failed = false;
      move_retry_at = {};
    } else {
      move_failed = true;
      move_retry_at = Clock::now() + std::chrono::milliseconds(1000);
    }
    const bool to_router = station_on_air() && want == sta_dev->channel();
    if (to_router) {
      sta_dev->on_channel(true);
      if (sta_dev->associated() && running && want != awdl_channel) sta_dev->send_null(false);
    }
    return true;
  };

  auto schedule = [&]() -> bool {
    if (scan.active) {
      timeshare = false;
      return scan_step();
    }
    uint8_t want = awdl_channel;
    unsigned want_w = width;
    timeshare = false;
    if (station_on_air()) {
      const uint8_t s = sta_dev->channel();
      const WifiStation::State st = sta->state();
      const bool joining = st == WifiStation::State::Listening ||
                           st == WifiStation::State::Authenticating ||
                           st == WifiStation::State::Associating ||
                           st == WifiStation::State::Handshake;
      if (s == awdl_channel) {
        want_w = width;
      } else if (joining) {
        want = s;
        want_w = width;
      } else if (st == WifiStation::State::Running &&
                 Clock::now() - running_since < std::chrono::seconds(3)) {
        want = s;
        want_w = sta_dev->width();
      } else if (st == WifiStation::State::Running) {
        timeshare = true;
        const uint64_t t = now_us();
        update_plan(t, s);
        if (away[aw_clock.entry(t + 2000)]) {
          want = s;
          want_w = sta_dev->width();
        }
      }
    }
    return move_to(want, want > 14 ? want_w : 20u);
  };

  struct QueuedTx {
    std::vector<uint8_t> frame;
    mtk::TxParams params;
  };
  constexpr size_t kUnicastQueueMax = 256;
  std::deque<QueuedTx> unicast_q;
  std::deque<PendingCmd> awdl_tx_q;

  std::vector<uint8_t> rx_batch;
  rx_batch.reserve(128 * 1024);
  mtk::RxBaReorder ba_reorder;
  std::vector<std::array<uint8_t, 33>> addba_responses;
  uint64_t last_active_us = 0;
  constexpr uint64_t kSpinWindowUs = 5000; /* 5 ms active spin before Sleep(1) */
  auto last_report = Clock::now();
  uint64_t rx_total = 0, rx_awdl = 0, rx_to_us = 0;

  while (!stopping()) {
    size_t tx_sent = 0;
    bool did_local = false;

    /* Separate local commands (TAP Ethernet, scans, rejoins, radio settings)
     * from AWDL TX so station traffic and scans never wait behind AWDL frames. */
    std::deque<PendingCmd> local_q;
    {
      std::lock_guard<std::mutex> lk(cmd_mu);
      while (!cmds.empty() && local_q.size() + awdl_tx_q.size() < 1024) {
        PendingCmd &m = cmds.front();
        if (m.type == RADIO_LINK_TX) awdl_tx_q.push_back(std::move(m));
        else local_q.push_back(std::move(m));
        cmds.pop_front();
      }
    }
    {
      PendingCmd shm_tx;
      shm_tx.type = RADIO_LINK_TX;
      const uint64_t t_now = now_us();
      while (awdl_tx_q.size() < 1024 && pipe.pop_shm_tx(shm_tx.payload)) {
        shm_tx.at_us = t_now;
        awdl_tx_q.push_back(std::move(shm_tx));
        shm_tx.payload.clear();
      }
    }

    for (const auto &c : local_q) {
      did_local = true;
      if (c.type == kStaEthernet) {
        if (sta) sta->on_ethernet(c.payload.data(), c.payload.size());
      } else if (c.type == kScanRequest || c.type == kBackgroundScan) {
        start_scan(c.payload, c.type == kBackgroundScan);
      } else if (c.type == kRejoinRequest) {
        if (sta) {
          log_line("event: wifi-stall");
          sta->rejoin("the gateway stopped answering");
        }
      } else if (c.type == RADIO_LINK_SET_CHANNEL && c.payload.size() >= 1) {
        const uint8_t ch = c.payload[0];
        if (ch != awdl_channel) {
          log_fmt("channel %u -> %u", awdl_channel, ch);
          awdl_channel = ch;
          sched.reset();
          have_sched_peer = false;
          gate_since_us = 0;
          unicast_q.clear();
        }
      } else if (c.type == RADIO_LINK_SET_ADDRESS && c.payload.size() >= 6) {
        dev.kick_tx();
        std::memcpy(own_mac.data(), c.payload.data(), 6);
        dev.set_address(own_mac.data());
      } else if (c.type == RADIO_LINK_SET_BSSID_FILTER && c.payload.size() >= 6) {
        std::memcpy(bssid_filter.data(), c.payload.data(), 6);
        filter_active = false;
        for (uint8_t b : bssid_filter) {
          if (b != 0) filter_active = true;
        }
      }
    }

    if (schedule()) did_local = true;

    /* Send AWDL TX on its own channel, including the scan's home windows. */
    const bool on_awdl_ch = (dev.channel() == awdl_channel && (!scan.active || scan.home));
    if (on_awdl_ch) {
      const uint64_t now = now_us();
      while (!awdl_tx_q.empty()) {
        const PendingCmd &c = awdl_tx_q.front();
        const uint8_t *frame = nullptr;
        size_t flen = 0;
        mtk::TxParams tx_params{};
        if (mtk::parse_tx_radiotap(c.payload.data(), c.payload.size(), frame, flen, tx_params)) {
          const bool is_data = (frame[0] & 0x0cu) == 0x08u;
          const bool is_unicast = flen >= 10 && (frame[4] & 1u) == 0;
          if (!is_data && now > c.at_us && now - c.at_us > 40000) {
            /* Held while off channel: AWDL sync timestamps are stale. */
            tx_stale++;
            awdl_tx_q.pop_front();
            continue;
          }
          if (is_data && is_unicast) last_awdl_data_us = now;
          if (awdl::is_awdl_action(frame, flen)) {
            unsigned counter = 0, period = 0;
            if (frame_sync(frame, flen, counter, period)) aw_clock.note(c.at_us, counter, period);
            if (timeshare && station_on_air()) {
              rewritten.assign(frame, frame + flen);
              if (awdl_plan::rewrite_frame(rewritten.data(), flen, away, sta_dev->channel())) {
                frame = rewritten.data();
              }
            }
          }
          const bool to_sched_peer = !no_block_ack && is_data && is_unicast && have_sched_peer &&
                                     std::memcmp(frame + 4, sched_peer, 6) == 0;
          if (to_sched_peer && sched.known()) {
            if (unicast_q.size() >= kUnicastQueueMax) unicast_q.pop_front();
            unicast_q.push_back({std::vector<uint8_t>(frame, frame + flen), tx_params});
          } else {
            if (!dev.transmit(frame, flen, tx_params, false)) break;
            tx_sent++;
          }
        }
        awdl_tx_q.pop_front();
      }
      dev.kick_tx();
    } else {
      /* While away from AWDL's channel, bound the backlog and drop stale frames. */
      while (awdl_tx_q.size() > 256) {
        awdl_tx_q.pop_front();
        tx_stale++;
      }
    }

    addba_responses.clear();
    rx_batch.clear();
    const size_t got = dev.poll([&](const mtk::RxFrame &f) {
      rx_total++;
      note_bss(f);
      if (station_on_air() && f.len >= 20) {
        const RxFrame r{f.data, f.len, f.dbm, f.channel, 0, 0, false};
        /* Block Ack Request (control subtype 8) from the AP for reorder windows. */
        const bool bar =
            (f.data[0] & 0xfcu) == 0x84u && std::memcmp(f.data + 10, sta_dev->ap(), 6) == 0;
        if (bar || sta->owns(r)) {
          sta_dev->receive(f);
          RxFrame opened{};
          while (sta_dev->take(opened)) sta->on_frame(opened);
          return;
        }
      }

      const uint64_t heard = now_us();
      if (!no_block_ack && f.len >= 16 && f.channel == awdl_channel && on_awdl_ch) {
        const uint8_t *from = f.data + 10;
        const bool is_sched = have_sched_peer && std::memcmp(from, sched_peer, 6) == 0;
        const bool want = is_sched || !sched.fresh(heard);
        if (want && awdl::is_awdl_action(f.data, f.len)) {
          awdl_sched::PeerSyncInfo info{};
          bool bad = false;
          if (awdl_sched::peerSyncFromAction(f.data, f.len, &info, &bad)) {
            if (!is_sched) {
              sched.reset();
              std::memcpy(sched_peer, from, 6);
              have_sched_peer = true;
              gate_since_us = 0;
            }
            sched.notePeerFrame(heard);
            sched.note(info, heard, awdl_channel);
          } else if (is_sched) {
            if (bad) sched.invalidate();
            else sched.notePeerFrame(heard);
          }
        } else if (is_sched) {
          sched.notePeerFrame(heard);
        }
      }
      if (awdl::is_awdl_action(f.data, f.len)) {
        rx_awdl++;
        uint8_t seq[16];
        if (awdl::peer_sequence(f.data, f.len, seq) && std::memcmp(seq, peer_seq, 16) != 0) {
          std::memcpy(peer_seq, seq, 16);
          const unsigned ch = awdl::peer_channel(seq);
          if (ch && ch != peer5g) {
            peer5g = ch;
            log_line("event: peer-channel " + std::to_string(ch));
          }
        }
        char cc[3];
        if (awdl::peer_country(f.data, f.len, cc) && peer_country.note(cc)) {
          log_line("event: peer-country " + std::string(peer_country.confirmed));
        }
      }

      if (f.len >= 24 && (f.data[0] & 0x0cu) == 0x08u &&
          std::memcmp(f.data + 4, own_mac.data(), 6) == 0) {
        rx_to_us++;
        last_awdl_data_us = heard;
      }

      /* Auto-answer 802.11 ADDBA Request (awdld/OWL does not send ADDBA Response,
       * so without this MT7922 never activates HW RX BlockAck / A-MPDU). */
      if (!no_block_ack && on_awdl_ch && f.len >= 33 && (f.data[0] & 0xfcu) == 0xd0u &&
          std::memcmp(f.data + 4, own_mac.data(), 6) == 0 && f.data[24] == 3u &&
          f.data[25] == 0u) {
        const uint8_t token = f.data[26];
        const uint16_t req_params = static_cast<uint16_t>(f.data[27] | (f.data[28] << 8));
        const uint16_t seqctl = static_cast<uint16_t>(f.data[31] | (f.data[32] << 8));
        const bool immediate = (req_params & 2u) != 0;
        if (immediate && addba_responses.size() < 8) {
          /* Keep amsdu=0 in ADDBA Response (awdld does not parse A-MSDU directly;
           * append_rx_deamsdu still safely splits any A-MSDU a peer might send). */
          (void)he80;
          const uint16_t amsdu = 0u;
          const uint8_t tid = static_cast<uint8_t>((req_params >> 2) & 0x0fu);
          uint16_t win = static_cast<uint16_t>((req_params >> 6) & 0x03ffu);
          if (win == 0 || win > 64) win = 64;
          const uint16_t ssn = static_cast<uint16_t>((seqctl >> 4) & 0x0fffu);
          ba_reorder.on_addba(f.data + 10, tid, ssn, win, heard,
                              [&](const uint8_t *d, size_t n, uint8_t ch, int8_t rssi) {
                                if (pipe.connected()) append_rx_deamsdu({d, n, rssi, ch}, rx_batch);
                              });
          std::array<uint8_t, 33> resp{};
          resp[0] = 0xd0u;
          std::memcpy(resp.data() + 4, f.data + 10, 6);
          std::memcpy(resp.data() + 10, own_mac.data(), 6);
          std::memcpy(resp.data() + 16, f.data + 16, 6);
          const uint16_t sc = static_cast<uint16_t>((++mgmt_seq) << 4);
          resp[22] = static_cast<uint8_t>(sc & 0xffu);
          resp[23] = static_cast<uint8_t>(sc >> 8);
          resp[24] = 3u;
          resp[25] = 1u;
          resp[26] = token;
          const uint16_t rsp_params =
              static_cast<uint16_t>(amsdu | 2u | (static_cast<uint16_t>(tid) << 2) | (win << 6));
          resp[29] = static_cast<uint8_t>(rsp_params & 0xffu);
          resp[30] = static_cast<uint8_t>(rsp_params >> 8);
          addba_responses.push_back(resp);
        }
        return;
      } else if (!no_block_ack && on_awdl_ch && f.len >= 28 && (f.data[0] & 0xfcu) == 0xd0u &&
                 std::memcmp(f.data + 4, own_mac.data(), 6) == 0 && f.data[24] == 3u &&
                 f.data[25] == 2u) {
        const uint16_t params = static_cast<uint16_t>(f.data[26] | (f.data[27] << 8));
        if ((params >> 11) & 1u) {
          const uint8_t tid = static_cast<uint8_t>((params >> 12) & 0x0fu);
          ba_reorder.on_delba(f.data + 10, tid,
                              [&](const uint8_t *d, size_t n, uint8_t ch, int8_t rssi) {
                                if (pipe.connected()) append_rx_deamsdu({d, n, rssi, ch}, rx_batch);
                              });
        }
        return;
      } else if (!no_block_ack && on_awdl_ch && f.len >= 20 && f.data[0] == 0x84u &&
                 std::memcmp(f.data + 4, own_mac.data(), 6) == 0) {
        ba_reorder.on_bar(f.data, f.len, heard,
                          [&](const uint8_t *d, size_t n, uint8_t ch, int8_t rssi) {
                            if (pipe.connected()) append_rx_deamsdu({d, n, rssi, ch}, rx_batch);
                          });
        return;
      }

      if (!pipe.connected()) return;
      if (filter_active && f.len >= 22 && std::memcmp(f.data + 16, bssid_filter.data(), 6) != 0) {
        return;
      }
      if (!no_block_ack && on_awdl_ch && f.len >= 26 && (f.data[0] & 0x8cu) == 0x88u &&
          std::memcmp(f.data + 4, own_mac.data(), 6) == 0) {
        if (ba_reorder.process(f.data, f.len, f.channel, f.dbm, heard,
                               [&](const uint8_t *d, size_t n, uint8_t ch, int8_t rssi) {
                                 append_rx_deamsdu({d, n, rssi, ch}, rx_batch);
                               })) {
          return;
        }
      }
      append_rx_deamsdu(f, rx_batch);
    });

    if (pipe.connected() && !no_block_ack) {
      ba_reorder.flush_expired(now_us(),
                               [&](const uint8_t *d, size_t n, uint8_t ch, int8_t rssi) {
                                 append_rx_deamsdu({d, n, rssi, ch}, rx_batch);
                               });
    }

    if (!rx_batch.empty()) pipe.send_raw(rx_batch);
    if (on_awdl_ch && !addba_responses.empty()) {
      for (const auto &resp : addba_responses) {
        if (dev.transmit(resp.data(), resp.size(), 6u, false)) tx_sent++;
      }
      dev.kick_tx();
    }

    /* Station timers, reorder timeouts, state events, and ADDBA answers. */
    if (sta && sta_dev) {
      sta->tick(now_us());
      sta_dev->tick();
      RxFrame late{};
      while (sta_dev->take(late)) {
        sta->on_frame(late);
        did_local = true;
      }
      if (sta->state() != published_wifi) {
        published_wifi = sta->state();
        if (published_wifi == WifiStation::State::Running) running_since = Clock::now();
        log_fmt("event: wifi %s", sta->state_name());
      }
      std::vector<uint8_t> ans;
      while (sta->take_addba(ans)) {
        TxRate r; /* 6 Mb/s */
        sta_dev->sta_transmit(ans.data(), ans.size(), r, (ans[1] & 0x40) != 0);
        did_local = true;
      }
    }

    /* Drain queued unicast AWDL Data frames when on AWDL's channel. */
    if (on_awdl_ch && !unicast_q.empty()) {
      const uint64_t now = now_us();
      bool gate_closed = false;
      if (have_sched_peer && sched.known() && sched.fresh(now)) {
        const auto gate = sched.sendGate(now, false, 0);
        if (gate == awdl_sched::PeerSchedule::SendGate::Away) {
          if (!gate_since_us) {
            gate_since_us = now;
            gate_limit_us = sched.pauseLimitUS(now);
            if (gate_limit_us > 16000) gate_limit_us = 16000;
          }
          if (now - gate_since_us < gate_limit_us) gate_closed = true;
        } else {
          gate_since_us = 0;
          gate_limit_us = 0;
        }
      } else {
        gate_since_us = 0;
        gate_limit_us = 0;
      }
      if (!gate_closed) {
        while (!unicast_q.empty()) {
          const auto &q = unicast_q.front();
          if (!dev.transmit(q.frame.data(), q.frame.size(), q.params, false)) break;
          unicast_q.pop_front();
          tx_sent++;
        }
        dev.kick_tx();
      }
    }

    if (Clock::now() - last_report > std::chrono::seconds(10)) {
      log_fmt("rx: %llu frames, %llu awdl, %llu addressed to us; tx: %llu acked, %llu failed",
              static_cast<unsigned long long>(rx_total), static_cast<unsigned long long>(rx_awdl),
              static_cast<unsigned long long>(rx_to_us),
              static_cast<unsigned long long>(awdl_tx_acked),
              static_cast<unsigned long long>(awdl_tx_failed));
      if (sta && sta_dev) {
        const MtkStation::Stats &s = sta_dev->stats();
        unsigned router = 0;
        for (uint8_t a : away) router += a;
        log_fmt("wifi: %s, %llu packets in, %llu out; frames to the AP %llu sent, %llu acked, "
                "%llu failed, %llu held, %llu dropped, rate %s (%llu at it); from the AP %llu "
                "(and %llu to others), %llu opened, %llu bad MIC, %llu without key, %llu "
                "replayed, %llu repeats, %llu sent again by the AP, A-MPDU sessions %u (%llu "
                "reordered, %llu late); sleep/wake frames %llu/%llu sent, %llu/%llu acked; "
                "channel moves %llu (%llu us average, %llu us longest), %u of 16 entries on the "
                "router's channel%s, %llu%% of the time there; AWDL frames held too long %llu",
                sta->state_name(), static_cast<unsigned long long>(sta->rx_packets()),
                static_cast<unsigned long long>(sta->tx_packets()),
                static_cast<unsigned long long>(s.tx), static_cast<unsigned long long>(s.tx_acked),
                static_cast<unsigned long long>(s.tx_failed),
                static_cast<unsigned long long>(s.tx_held),
                static_cast<unsigned long long>(s.tx_dropped), sta_dev->rate_name().c_str(),
                static_cast<unsigned long long>(s.tx_fw), static_cast<unsigned long long>(s.rx),
                static_cast<unsigned long long>(s.rx_others),
                static_cast<unsigned long long>(s.rx_opened),
                static_cast<unsigned long long>(s.rx_bad_mic),
                static_cast<unsigned long long>(s.rx_no_key),
                static_cast<unsigned long long>(s.rx_replay),
                static_cast<unsigned long long>(s.rx_repeat),
                static_cast<unsigned long long>(s.rx_retry), s.rx_ba,
                static_cast<unsigned long long>(s.rx_reordered),
                static_cast<unsigned long long>(s.rx_late),
                static_cast<unsigned long long>(s.sleep_sent),
                static_cast<unsigned long long>(s.wake_sent),
                static_cast<unsigned long long>(s.sleep_acked),
                static_cast<unsigned long long>(s.wake_acked),
                static_cast<unsigned long long>(moves),
                static_cast<unsigned long long>(moves ? move_us / moves : 0ull),
                static_cast<unsigned long long>(move_us_max), timeshare ? router : 0u,
                aw_clock.valid ? "" : " (no AWDL clock yet)",
                static_cast<unsigned long long>(here_total_us ? here_us * 100 / here_total_us
                                                              : 0ull),
                static_cast<unsigned long long>(tx_stale));
      }
      last_report = Clock::now();
    }

    if (got > 0 || did_local || tx_sent > 0) {
      last_active_us = now_us();
    } else if (now_us() - last_active_us < kSpinWindowUs) {
      SwitchToThread();
    } else {
      Sleep(1);
    }
  }

  g_stop = true;
  /* Halt WFDMA and reset WFSYS immediately on shutdown (before joining worker
   * threads) so DMA is guaranteed idle well before process exit frees AWE pages (#72). */
  log_line("closing MTK radio (WFSYS reset)");
  dev.close();
  if (reader.joinable()) reader.join();
  if (tap_thread.joinable()) tap_thread.join();
  if (timer.joinable()) timer.join();
  if (sta) sta->stop();
  timeEndPeriod(1);
  return 0;
}
