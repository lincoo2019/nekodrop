// SPDX-License-Identifier: GPL-2.0-only
/* Offline checks of the radio backend contract (src/radio_backend.h): the
 * capability matrix of the two backends radio_main runs (iwx, kernel), the
 * policies radio_main applies per capability, and that a backend with no
 * Wi-Fi station, no firmware scan and no Intel A-MPDU queue (a stand-in for
 * the planned Realtek "devourer" backend, docs/HARDWARE.md) satisfies the
 * contract with the core listener calls alone, without stubs.
 *
 * Portable (no Windows headers), so it also builds outside iwx_selftest.
 * Returns the number of failures. */
#pragma once

#include <array>
#include <cctype>
#include <cstdio>
#include <string>

#include "../src/radio_backend.h"

namespace radio_backend_test {

/* The stand-in: what a user-mode Realtek backend would declare today. */
constexpr RadioCapabilities devourer_capabilities() {
  RadioCapabilities c{};
  c.backend = "devourer";
  c.max_tx_window = radio::kDefaultTxWindow; /* no faster sending until measured */
  return c;
}

/* The core calls only: no sta_*, umac_scan, add_rx_ba, set_promiscuous,
 * agg_*, kick_data, answer_country_notice, set_country or channel_flags. */
struct MockDevourerBackend {
  static constexpr RadioCapabilities kCapabilities = devourer_capabilities();
  bool open(const RadioOpenOptions &, std::string &) { return true; }
  bool start_monitor(uint8_t, unsigned, const uint8_t *) { return true; }
  bool set_channel(uint8_t, unsigned) { return true; }
  bool set_address(const uint8_t *) { return true; }
  bool transmit(const uint8_t *, size_t, const TxRate &) { return true; }
  int transmit_slot(const uint8_t *, size_t, const TxRate &, bool = true) { return 0; }
  void set_rx_handler(RadioRxHandler h) { _rx = h; }
  void set_tx_status_handler(RadioTxStatusHandler h) { _tx = h; }
  int poll() { return 0; }
  void check_health() {}
  bool healthy() const { return true; }
  const TxStats &tx_stats() const { return _stats; }
  uint8_t channel() const { return 44; }
  unsigned width_mhz() const { return 80; }
  const std::array<uint8_t, 6> &hw_address() const { return _hw; }
  void stop() {}

private:
  RadioRxHandler _rx;
  RadioTxStatusHandler _tx;
  TxStats _stats;
  std::array<uint8_t, 6> _hw{};
};

/* Compiling these is the check: a stub-free backend meets the contract, and
 * the detectors see the optional groups it lacks. */
static_assert(radio::check_backend_contract<MockDevourerBackend>(), "devourer stand-in meets the contract");
static_assert(radio::detail::has_core<MockDevourerBackend>::value, "core calls detected");
static_assert(!radio::detail::has_station<MockDevourerBackend>::value, "no station calls");
static_assert(!radio::detail::has_umac_scan<MockDevourerBackend>::value, "no firmware scan");
static_assert(!radio::detail::has_aggregate<MockDevourerBackend>::value, "no A-MPDU calls");
static_assert(!radio::detail::has_batched_kick<MockDevourerBackend>::value, "no kick_data");
static_assert(!radio::detail::has_channel_flags<MockDevourerBackend>::value, "no channel flags");
/* A type missing the core calls is seen as such. */
struct NotABackend {};
static_assert(!radio::detail::has_core<NotABackend>::value, "an empty type is not a backend");
static_assert(!radio::detail::has_capabilities<NotABackend>::value, "an empty type declares nothing");

inline bool contains_ci(const std::string &hay, const char *needle) {
  std::string h, n(needle);
  for (char c : hay) h += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (auto &c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return h.find(n) != std::string::npos;
}

/* Words installer/report.ps1 (its error filter is case-insensitive) or
 * Supervisor.cs (prefixes it acts on) react to in a radio line: plain
 * information lines must contain none. The first one found, or nullptr. */
inline const char *report_trigger(const std::string &s) {
  static const char *const kTriggers[] = {
      "error", "fail", "refus", "cannot", "could not", "timed out", "not supported", "exited", "restart",
      "giving the card", "taking the card", "ready on", "pnvm", "init_complete", "nvm:", "alive",
      "unsupported", "card 0x", "family", "rx: ", "tx: ", "aggregate: ", "event: ", "bring-up",
      "transmitting allowed", "moving there", "stayed passive", "starting:", "stopping:", "wi-fi: joining",
      "gateway", "lmac", "umac", "secboot", "wifi: "};
  for (const char *t : kTriggers)
    if (contains_ci(s, t)) return t;
  return nullptr;
}

inline int run() {
  int failures = 0;
  const auto check = [&](bool ok, const char *what) {
    std::printf("%s backend contract: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
  };
  constexpr RadioCapabilities iwx = radio::iwx_capabilities();
  constexpr RadioCapabilities kern = radio::kernel_capabilities();
  constexpr RadioCapabilities dev = devourer_capabilities();

  /* The matrix (docs/HARDWARE.md, "The radio's backend contract"). */
  check(radio::validate_capabilities(iwx) && radio::validate_capabilities(kern) && radio::validate_capabilities(dev),
        "iwx, kernel and devourer capability sets are consistent");
  check(std::string(iwx.backend) == "iwx" && iwx.supports_station && !iwx.windows_owns_station &&
            iwx.supports_umac_scan && iwx.supports_rx_ba && iwx.supports_promiscuous_toggle &&
            iwx.supports_aggregate && iwx.supports_batched_kick && iwx.supports_country_notice &&
            iwx.supports_set_country && iwx.enforces_passive_channel && iwx.max_tx_window == 64,
        "iwx: everything, TX window up to 64");
  check(std::string(kern.backend) == "kernel" && !kern.supports_station && kern.windows_owns_station &&
            !kern.supports_umac_scan && kern.supports_rx_ba && kern.supports_promiscuous_toggle &&
            !kern.supports_aggregate && !kern.supports_batched_kick && !kern.supports_country_notice &&
            kern.supports_set_country && kern.enforces_passive_channel && kern.max_tx_window == 64,
        "kernel: Windows owns the station; Block Ack, promiscuous, country, passive rule; no A-MPDU");
  check(!dev.supports_station && !dev.windows_owns_station && !dev.supports_umac_scan && !dev.supports_rx_ba &&
            !dev.supports_aggregate && !dev.supports_batched_kick && !dev.enforces_passive_channel,
        "devourer stand-in: listener and injector only");

  /* Inconsistent sets are refused. */
  {
    RadioCapabilities c = iwx;
    c.windows_owns_station = true;
    check(!radio::validate_capabilities(c), "refused: station on the backend and on Windows");
    c = iwx;
    c.supports_rx_ba = false;
    check(!radio::validate_capabilities(c), "refused: aggregate without receive Block Ack");
    c = iwx;
    c.supports_promiscuous_toggle = false;
    check(!radio::validate_capabilities(c), "refused: aggregate without promiscuous control");
    c = iwx;
    c.max_tx_window = 65;
    check(!radio::validate_capabilities(c), "refused: TX window above 64");
    c = iwx;
    c.max_tx_window = 4;
    check(!radio::validate_capabilities(c), "refused: TX window below the default 8");
    c = kern;
    c.enforces_passive_channel = false;
    c.supports_country_notice = true;
    check(!radio::validate_capabilities(c), "refused: country notice without channel flags");
    c = dev;
    c.backend = "";
    check(!radio::validate_capabilities(c), "refused: no backend name");
  }

  /* TX window: what radio_main did before the contract, for iwx and kernel. */
  {
    const radio::TxConfig i8 = radio::resolve_tx_config(8, iwx), i48 = radio::resolve_tx_config(48, iwx);
    check(i8.window == 8 && !i8.batch_kick && i8.initial_rate_step == 0 && !i8.window_clamped,
          "iwx default: window 8, no batch doorbell, ladder from step 0");
    check(i48.window == 48 && i48.batch_kick && i48.initial_rate_step == 1 && !i48.window_clamped,
          "iwx faster sending (48): window 48, batch doorbell, ladder from step 1");
    const radio::TxConfig k8 = radio::resolve_tx_config(8, kern), k48 = radio::resolve_tx_config(48, kern);
    check(k8.window == 8 && !k8.batch_kick && k8.initial_rate_step == 0, "kernel default: window 8, step 0");
    check(k48.window == 48 && !k48.batch_kick && k48.initial_rate_step == 1 && !k48.window_clamped,
          "kernel --tx-window 48: window 48 and step 1 as before, the driver rings its own doorbell");
    const radio::TxConfig d48 = radio::resolve_tx_config(48, dev);
    check(d48.window == 8 && d48.window_clamped && !d48.batch_kick && d48.initial_rate_step == 0,
          "devourer --tx-window 48: clamped to its maximum 8");
  }

  /* Passive channels (#11). */
  {
    const uint32_t passive = 0, active = radio::kChannelActiveFlag;
    check(radio::channel_allows_tx(6, passive, false, iwx), "2.4 GHz always allows sending");
    check(!radio::channel_allows_tx(44, passive, false, iwx) && !radio::channel_allows_tx(44, passive, false, kern),
          "passive 44 holds frames on iwx and kernel");
    check(radio::channel_allows_tx(44, passive, true, iwx), "a beacon or AWDL peer heard unlocks 44");
    check(radio::channel_allows_tx(44, active, false, kern), "active 44 allows sending");
    check(radio::channel_allows_tx(44, passive, false, dev), "no channel flags: no passive rule");
  }

  /* Discovery scan and the Wi-Fi list scan. */
  {
    check(radio::should_start_passive_discovery_scan(false, false, false, iwx),
          "iwx on a passive channel: one discovery scan");
    check(!radio::should_start_passive_discovery_scan(true, false, false, iwx) &&
              !radio::should_start_passive_discovery_scan(false, true, false, iwx) &&
              !radio::should_start_passive_discovery_scan(false, false, true, iwx),
          "no discovery scan when sending is allowed or a scan is on its way");
    check(!radio::should_start_passive_discovery_scan(false, false, false, kern) &&
              !radio::should_start_passive_discovery_scan(false, false, false, dev),
          "no discovery scan without a firmware scan and country notice");
    check(!radio::is_scan_complete(false, false, 99999, iwx), "no scan, nothing completes");
    check(!radio::is_scan_complete(true, false, 300, iwx) && radio::is_scan_complete(true, false, 301, iwx),
          "firmware scan: done once the firmware says so, after 300 ms");
    check(!radio::is_scan_complete(true, true, 15000, iwx) && radio::is_scan_complete(true, true, 15001, iwx),
          "firmware scan still running: ends after 15 s");
    check(!radio::is_scan_complete(true, false, 2000, kern) && radio::is_scan_complete(true, false, 2001, kern),
          "listening scan (kernel): 2 s");
    check(radio::is_scan_complete(true, false, 2001, dev), "listening scan (devourer): 2 s");
  }

  /* Station policy wording. */
  check(radio::station_policy_message(iwx).empty(), "iwx runs the station");
  check(radio::station_policy_message(kern) ==
            "station config ignored: Windows runs the station on the kernel driver",
        "kernel: the line the radio always printed");
  check(radio::station_policy_message(dev) == "station config ignored: backend devourer has no Wi-Fi station",
        "devourer: names the backend");

  /* The start-up summary must not look like a stage, an error or an event
   * to installer/report.ps1 (its error filter is case-insensitive) or to
   * Supervisor.cs (prefixes it acts on). */
  {
    const RadioCapabilities all[] = {iwx, kern, dev};
    for (const auto &c : all) {
      const std::string s = radio::format_capabilities_summary(c);
      const char *hit = report_trigger(s);
      std::printf("     %s\n", s.c_str());
      check(!hit && s.rfind(std::string("backend ") + c.backend + ": ", 0) == 0,
            hit ? hit : "summary line is plain information");
    }
    check(radio::format_capabilities_summary(iwx).find("tx-window up to 64") != std::string::npos &&
              radio::format_capabilities_summary(kern).find("station windows") != std::string::npos,
          "summary names the window and who owns the station");
  }

  /* Experimental fast passive-channel unlock (#11): off keeps the one scan
   * at 10 s; on scans sooner, up to three times, over non-DFS channels. */
  {
    check(radio::passive_discovery_scan_delay_ms(false, 0, false) == 10000 &&
              radio::passive_discovery_scan_delay_ms(false, 0, true) == 10000 &&
              radio::passive_discovery_scan_delay_ms(false, 1, false) == 0,
          "fast unlock off: one discovery scan at 10 s, never another (unchanged)");
    check(radio::passive_discovery_scan_delay_ms(true, 0, false) == 1500 &&
              radio::passive_discovery_scan_delay_ms(true, 1, false) == 12000 &&
              radio::passive_discovery_scan_delay_ms(true, 2, false) == 30000 &&
              radio::passive_discovery_scan_delay_ms(true, radio::kMaxPassiveDiscoveryScans, false) == 0 &&
              radio::kMaxPassiveDiscoveryScans == 3,
          "fast unlock on: discovery scans at 1.5, 12 and 30 s, three at most");
    check(radio::passive_discovery_scan_delay_ms(true, 0, true) == 4000 &&
              radio::passive_discovery_scan_delay_ms(true, 1, true) == 12000,
          "fast unlock on: the first scan waits up to 4 s while the station joins");
    const auto no_dfs = [](const uint8_t *c, size_t n) {
      for (size_t i = 0; i < n; i++)
        if (c[i] >= 52 && c[i] <= 144) return false;
      return true;
    };
    const auto same_set = [](const uint8_t *a, const uint8_t *b, size_t n) {
      for (size_t i = 0; i < n; i++) {
        bool found = false;
        for (size_t j = 0; j < n; j++) found |= a[i] == b[j];
        if (!found) return false;
      }
      return true;
    };
    uint8_t c149[radio::kFastDiscoveryChannelCount] = {}, c44[radio::kFastDiscoveryChannelCount] = {},
            c153[radio::kFastDiscoveryChannelCount] = {};
    const size_t n149 = radio::fast_discovery_channels(149, c149), n44 = radio::fast_discovery_channels(44, c44),
                 n153 = radio::fast_discovery_channels(153, c153);
    check(n149 == 11 && c149[0] == 149 && c149[1] == 153 && c149[2] == 157 && c149[3] == 161 && no_dfs(c149, n149),
          "fast discovery scan for 149: 11 non-DFS channels, 149-161 first");
    check(n44 == 11 && c44[0] == 44 && c44[1] == 36 && c44[2] == 40 && c44[3] == 48 && no_dfs(c44, n44) &&
              same_set(c44, c149, 11),
          "fast discovery scan for 44: the same 11 channels, 44 then 36/40/48 first");
    check(n153 == 11 && c153[0] == 153 && c153[1] == 149 && same_set(c153, c149, 11),
          "fast discovery scan for 153: its own channel first, then its block");
    check(radio::held_country("TW") == "TW" && radio::held_country("00").empty() &&
              radio::held_country("ZZ").empty() && radio::held_country("").empty() &&
              radio::held_country("tw").empty() && radio::held_country("TWN").empty(),
          "held country: two letters; the world domain and anything else are none");
    const std::string m1 = radio::format_discovery_milestone("AWDL action frame", 3249, " (channel 149, -61 dBm)"),
                      m2 = radio::format_discovery_milestone("AWDL MIF", 0, ""),
                      m3 = radio::format_discovery_milestone("frame addressed to us", 99950, "");
    check(m1 == "discovery: first AWDL action frame after 3.2 s (channel 149, -61 dBm)" &&
              m2 == "discovery: first AWDL MIF after 0.0 s" &&
              m3 == "discovery: first frame addressed to us after 100.0 s",
          "discovery milestone lines: tenths of a second, rounded");
    /* The new informational lines radio_main writes, as it words them: none
     * may look like a stage, an error or an event to report.ps1/Supervisor. */
    const std::string lines[] = {m1, m2, m3, radio::kFastUnlockOnLine,
                                 "channel 149 is passive: fast discovery scan 1 of 3 so the firmware can learn the country",
                                 "fast discovery scan started (11 channels)", "fast discovery scan did not start",
                                 "fast discovery scan done; channel 149 still passive",
                                 "fast discovery scan done; channel 149 now allows sending"};
    for (const auto &s : lines) {
      const char *hit = report_trigger(s);
      std::printf("     %s\n", s.c_str());
      check(!hit, hit ? hit : "new discovery line is plain information");
    }
  }
  return failures;
}

} // namespace radio_backend_test
