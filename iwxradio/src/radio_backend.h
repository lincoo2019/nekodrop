// SPDX-License-Identifier: GPL-2.0-only
/*
 * The contract between the radio daemon (radio_main.cpp) and a radio backend:
 * the user-mode Intel driver (IwxDevice, iwx.h), the kernel driver's listener
 * (KernelDevice, kdev.h), and later other cards (docs/HARDWARE.md: a Realtek
 * backend would drive the card from user mode but have no Wi-Fi station, no
 * firmware scan and no Intel A-MPDU queue).
 *
 * radio_main used to ask "is this IwxDevice?" and made KernelDevice carry
 * empty stubs for every Intel-only call. Instead each backend now declares
 * what it can do (RadioCapabilities, a static constexpr kCapabilities), and
 * radio_main branches on those bits with `if constexpr`: a call a backend
 * does not declare is never compiled for it, so a new backend implements the
 * core listener calls and nothing else. check_backend_contract<Dev>() makes
 * a declared capability without its calls (or the reverse mistake in
 * radio_main) a compile error naming the capability.
 *
 * Portable C++17 on purpose: iwx.h includes it, and the kernel driver builds
 * iwx.h with MSVC /std:c++17 /W4 /WX /EHs-c- /GR- (scripts/build-kdriver.ps1).
 * No Windows headers, no exceptions, no RTTI, no designated initializers,
 * no concepts; iwx_selftest checks it on any compiler.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

/* ---- Values every backend shares (moved here from iwx.h unchanged) ---- */

struct RxFrame {
  const uint8_t *data; /* 802.11 frame, header first, no FCS */
  size_t len;
  int8_t dbm;          /* -127 when unknown */
  uint8_t channel;
  uint32_t rate_n_flags;
  uint16_t status; /* IWX_RX_MPDU_RES_STATUS_*: decryption result */
  bool mic_stripped = false; /* hardware removed the CCMP MIC (RADA) */
};

struct TxRate {
  enum Kind { Legacy, Ht, Vht, Auto } kind = Legacy; /* Auto: firmware rate control (TLC) */
  unsigned mbps_x2 = 12; /* legacy rate in 500 kb/s units: 12 = 6 Mb/s */
  unsigned mcs = 0;      /* HT MCS 0..15, VHT MCS 0..9 */
  unsigned nss = 1;      /* VHT spatial streams, 1 or 2 */
  bool wide80 = false;   /* VHT at 80 MHz */
};

/* The network the station joins, read from Windows before the card is taken
 * (moved here from iwx.h unchanged, for sta_device.h). */
struct StaParams {
  uint8_t bssid[6];
  uint8_t addr[6];      /* our address: the TAP adapter's */
  uint8_t channel;
  unsigned width = 20;  /* PHY width while associated */
  uint16_t beacon_int = 100;
  bool ht = false, vht = false, mimo = false;
  /* The AP's own capabilities from its beacon, for backends whose firmware
   * picks the rate (HT capability info, A-MPDU parameters, VHT capability
   * info and RX MCS map). */
  uint16_t ap_ht_cap = 0;
  uint8_t ap_ampdu = 0;
  uint32_t ap_vht_cap = 0;
  uint16_t ap_vht_mcs = 0xffff;
};

struct TxStats {
  uint64_t submitted = 0, completed = 0, acked = 0, failed = 0, retries = 0,
           dropped_full = 0;
};

/* A-MPDU sending counters, for backends with supports_aggregate. */
struct RadioAggStats {
  uint64_t frames = 0, notifs = 0, txed = 0, acked = 0, singles = 0, agg_resps = 0;
  uint64_t singles_ok = 0, singles_fail = 0;
  uint32_t last_rate = 0, peak_frames = 0, last_single_rate = 0;
  uint16_t last_fail_status = 0;
};

enum class RadioMacType { Listener, Ibss, Sta };

/* What radio_main asks a backend to open with (formerly IwxDevice::Options). */
struct RadioOpenOptions {
  std::string firmware_dir;   /* holds iwlwifi-*.ucode */
  std::string country = "ZZ"; /* MCC to ask for; ZZ keeps what the card has */
  RadioMacType mac_type = RadioMacType::Listener;
  bool verbose = false;
  /* Experimental A-MPDU sending to the AWDL peer: one more TX ring. */
  bool aggregate = false;
  /* Experimental Wi-Fi 6 HE80 TX TLC (MCS 4..11 2SS) + 8 KiB RX A-MSDU buffers. */
  bool he80 = false;
  /* --family (the user's manual choice in Settings, for a card whose PCI
   * device ID nekodrop does not know): the device ID whose transport
   * parameters to use (cards::family_device), 0 for the card's own. */
  uint16_t family_device = 0;
  /* --bt-coex: the firmware's Bluetooth coexistence mode on a combined
   * Wi-Fi/Bluetooth card (1 network arbitration, 2 Bluetooth first, 3 Wi-Fi
   * first, as ForbiddenItlwm's itlawdlbtcoex), 0 for the backend's default.
   * Windows' AirDrop beacon goes out on that card's Bluetooth half. */
  uint32_t bt_coex = 0;
};

using RadioLogger = std::function<void(const std::string &)>;
using RadioRxHandler = std::function<void(const RxFrame &)>;
using RadioTxStatusHandler = std::function<void(unsigned slot, bool acked)>;

/* ---- What a backend can do ----
 * Each bit names a group of calls radio_main makes only when the bit is set
 * (check_backend_contract lists them). Defaults are "cannot": a new backend
 * starts as a bare listener/injector and opts in. */
struct RadioCapabilities {
  const char *backend = "";               /* short name for the log: "iwx", "kernel" */
  /* A Wi-Fi station beside AWDL on this backend (WifiStation, wifi_sta.h,
   * through StaDevice, sta_device.h; sta_transmit, sta_tx_acked,
   * sta_tx_failed). */
  bool supports_station = false;
  /* Windows keeps its own station on this card (the kernel driver): a
   * station config handed to the radio is ignored with that reason. */
  bool windows_owns_station = false;
  /* Firmware channel scan for the Wi-Fi list (umac_scan, scan_running).
   * Without it a scan request lists the beacons heard on our channel. */
  bool supports_umac_scan = false;
  /* Receive Block Ack sessions with AWDL peers (add_rx_ba, remove_rx_ba, rx_ba_open). */
  bool supports_rx_ba = false;
  /* Promiscuous receive on/off (promiscuous, set_promiscuous). */
  bool supports_promiscuous_toggle = false;
  /* Experimental A-MPDU sending (agg_*, transmit_agg, ba_epoch, agg_stats). */
  bool supports_aggregate = false;
  /* transmit_slot(kick=false) leaves the frame for one kick_data() doorbell
   * per batch. Without it the backend rings for itself; a TX window above
   * the default still works, just without batching. */
  bool supports_batched_kick = false;
  /* The firmware's own country notice (answer_country_notice), and the
   * domain it last reported (current_country). */
  bool supports_country_notice = false;
  /* Asking the card for a regulatory domain (set_country). */
  bool supports_set_country = false;
  /* The card reports which channels are passive (channel_flags), and the
   * radio holds frames there until a beacon or an AWDL peer is heard. A
   * backend without regulatory information sends on every channel. */
  bool enforces_passive_channel = false;
  /* Largest --tx-window this backend takes; larger requests are clamped. */
  unsigned max_tx_window = 8;
};

namespace radio {

/* --tx-window: frames in flight by default and at most (radio_main's parser). */
constexpr unsigned kDefaultTxWindow = 8;
constexpr unsigned kMaxTxWindow = 64;
/* IWX_NVM_CHANNEL_ACTIVE; nekodrop-wlan.sys reports the same bit. */
constexpr uint32_t kChannelActiveFlag = 1u << 3;

/* The two backends this daemon has. Kept here, beside the contract, so the
 * matrix is one table iwx_selftest can check without Windows headers; each
 * backend points its kCapabilities at its entry. */
constexpr RadioCapabilities iwx_capabilities() {
  RadioCapabilities c{};
  c.backend = "iwx";
  c.supports_station = true;
  c.supports_umac_scan = true;
  c.supports_rx_ba = true;
  c.supports_promiscuous_toggle = true;
  c.supports_aggregate = true;
  c.supports_batched_kick = true;
  c.supports_country_notice = true;
  c.supports_set_country = true;
  c.enforces_passive_channel = true;
  c.max_tx_window = kMaxTxWindow;
  return c;
}

constexpr RadioCapabilities kernel_capabilities() {
  RadioCapabilities c{};
  c.backend = "kernel";
  c.windows_owns_station = true;
  c.supports_rx_ba = true;
  c.supports_promiscuous_toggle = true;
  c.supports_set_country = true; /* the driver answers the firmware's notice itself */
  c.enforces_passive_channel = true;
  /* The window is host-side (frames radio_main keeps in flight), so a
   * --kernel --tx-window N run keeps working exactly as before; only the
   * batched doorbell is a user-mode feature. */
  c.max_tx_window = kMaxTxWindow;
  return c;
}

/* Why a capability set makes no sense, or nullptr. Checked at compile time
 * for every backend radio_main runs (check_backend_contract) and by iwx_selftest. */
constexpr const char *capabilities_problem(const RadioCapabilities &c) {
  if (!c.backend || !c.backend[0]) return "no backend name";
  if (c.supports_station && c.windows_owns_station) return "station both on this backend and on Windows";
  if (c.max_tx_window < kDefaultTxWindow || c.max_tx_window > kMaxTxWindow) return "max TX window outside 8..64";
  /* A-MPDU sending uses the receive Block Ack peer station and turns
   * promiscuous receive off while it runs. */
  if (c.supports_aggregate && !(c.supports_rx_ba && c.supports_promiscuous_toggle))
    return "aggregate without receive Block Ack and promiscuous control";
  /* A country notice matters only through the channel flags it changes. */
  if (c.supports_country_notice && !c.enforces_passive_channel) return "country notice without channel flags";
  return nullptr;
}

constexpr bool validate_capabilities(const RadioCapabilities &c) { return capabilities_problem(c) == nullptr; }

/* ---- Policies radio_main applies, per capability (offline-tested) ---- */

struct TxConfig {
  unsigned window = kDefaultTxWindow; /* unicast frames kept in flight */
  bool window_clamped = false;        /* the request was above the backend's maximum */
  bool batch_kick = false;            /* transmit with kick=false, then one kick_data() */
  unsigned initial_rate_step = 0;     /* RateLadder start: 1 (HT MCS 4) for faster sending (#4) */
};

/* `requested`: --tx-window, 1..64 (the parser refuses others). As before the
 * split: above 8 the ladder starts one step up and, where the backend can,
 * the doorbell is rung once per batch. */
constexpr TxConfig resolve_tx_config(unsigned requested, const RadioCapabilities &c) {
  TxConfig t{};
  t.window = requested;
  if (t.window > c.max_tx_window) {
    t.window = c.max_tx_window;
    t.window_clamped = true;
  }
  t.batch_kick = c.supports_batched_kick && t.window > kDefaultTxWindow;
  t.initial_rate_step = t.window > kDefaultTxWindow ? 1u : 0u;
  return t;
}

constexpr bool channel_active(unsigned channel, uint32_t flags) {
  return channel <= 14 || (flags & kChannelActiveFlag) != 0;
}

/* No initiating on a passive 5 GHz channel until a beacon or AWDL peer is
 * heard there (#11), on backends that know which channels are passive. */
constexpr bool channel_allows_tx(unsigned channel, uint32_t flags, bool beacon_or_peer_heard,
                                 const RadioCapabilities &c) {
  return !c.enforces_passive_channel || channel_active(channel, flags) || beacon_or_peer_heard;
}

/* ForbiddenItlwm's discovery scan, 10 s after start: access points' Country
 * elements let the firmware learn the country and send its notice. Needs a
 * firmware scan and a notice to answer. */
constexpr bool should_start_passive_discovery_scan(bool may_send, bool scan_started, bool scan_wanted,
                                                   const RadioCapabilities &c) {
  return c.supports_umac_scan && c.supports_country_notice && !may_send && !scan_started && !scan_wanted;
}

/* ---- Experimental fast passive-channel unlock (--fast-unlock, #11) ----
 * On a card that refuses every country the host asks for (#11, AX210, TW:
 * MCC status 4 from the BIOS, Wi-Fi, ME and MCC-API sources, world domain
 * "00", all of 5 GHz passive) the firmware's own rules leave one way to an
 * active AWDL channel: its country notice, which it sends after a scan heard
 * access points' Country elements (answer_country_notice). By default the
 * radio scans once, 10 s after start, all 38 channels (DFS ones included,
 * 149-165 last), and never again: #11 sat ~60 s on a passive 149 with every
 * AWDL frame held. With the experimental switch the discovery scan comes at
 * 1.5 s, again at 12 s and 30 s while the channel stays passive, and visits
 * only the eleven non-DFS channels with the AWDL channel's 80 MHz block
 * first. Nothing here transmits on a passive channel: may_send() still waits
 * for the firmware, a beacon or an AWDL peer. */
constexpr unsigned kMaxPassiveDiscoveryScans = 3;
constexpr size_t kFastDiscoveryChannelCount = 11;
/* Logged at start when the switch is on (report.ps1's timing table looks for it). */
constexpr const char kFastUnlockOnLine[] =
    "experimental fast passive-channel unlock: on (discovery scans at 1.5, 12 and 30 s while the channel is passive)";

/* Milliseconds after the radio is ready that discovery scan `attempt`
 * (0-based) is due, 0 when there is none. Off: the one scan at 10 s, as
 * always. On: the first waits up to 4 s while the Wi-Fi station is joining
 * (a scan takes the card off the access point's channel mid-handshake); it
 * is asked again every tick, so it runs as soon as the join ends after 1.5 s. */
constexpr unsigned passive_discovery_scan_delay_ms(bool fast_unlock, unsigned attempt, bool sta_joining) {
  if (!fast_unlock) return attempt == 0 ? 10000u : 0u;
  switch (attempt) {
  case 0: return sta_joining ? 4000u : 1500u;
  case 1: return 12000u;
  case 2: return 30000u;
  default: return 0u;
  }
}

/* The channels of a fast discovery scan, into out[kFastDiscoveryChannelCount]:
 * the AWDL channel, the rest of its 80 MHz block (a beacon there unlocks it
 * at once), the other non-DFS 5 GHz block, then 1, 6 and 11 for more Country
 * elements. Returns the count (always 11). */
constexpr size_t fast_discovery_channels(unsigned awdl_channel, uint8_t (&out)[kFastDiscoveryChannelCount]) {
  const uint8_t lower[4] = {36, 40, 44, 48}, upper[4] = {149, 153, 157, 161};
  const bool high = awdl_channel >= 149;
  const uint8_t *first = high ? upper : lower, *second = high ? lower : upper;
  size_t n = 0;
  /* Casts: the kernel build's /W4 /WX warns on int (a promoted uint8_t) against unsigned. */
  for (size_t i = 0; i < 4; i++)
    if (static_cast<unsigned>(first[i]) == awdl_channel) out[n++] = first[i];
  for (size_t i = 0; i < 4; i++)
    if (static_cast<unsigned>(first[i]) != awdl_channel) out[n++] = first[i];
  for (size_t i = 0; i < 4; i++) out[n++] = second[i];
  out[n++] = 1;
  out[n++] = 6;
  out[n++] = 11;
  return n;
}

/* The country the firmware actually holds, as radio_main tracks it: two
 * letters, "" for the world domain ("00", "ZZ") or anything else. With
 * --fast-unlock radio_main starts from this instead of --country, so a
 * country the firmware refused at start is asked again when a peer names it,
 * and the firmware's own notice of it is reported as an event (#11). */
inline std::string held_country(const std::string &firmware_says) {
  if (firmware_says.size() != 2 || firmware_says == "ZZ") return std::string();
  for (char c : firmware_says)
    if (c < 'A' || c > 'Z') return std::string();
  return firmware_says;
}

/* One line, once per radio start, for each first step of discovery (#4,
 * #11): "discovery: first <what> after 12.3 s<detail>". Plain information
 * for installer/report.ps1's timing table; worded to match no pattern
 * report.ps1 or Supervisor.cs reacts to (iwx_selftest checks). */
inline std::string format_discovery_milestone(const char *what, uint64_t elapsed_ms, const std::string &detail) {
  /* No std::to_string: the kernel build has no CRT formatting. */
  const uint64_t tenths = (elapsed_ms + 50) / 100;
  char n[24] = {};
  size_t i = sizeof(n) - 1;
  n[--i] = static_cast<char>('0' + tenths % 10);
  n[--i] = '.';
  uint64_t whole = tenths / 10;
  do {
    n[--i] = static_cast<char>('0' + whole % 10);
    whole /= 10;
  } while (whole && i);
  return std::string("discovery: first ") + what + " after " + (n + i) + " s" + detail;
}

/* A scan for the Wi-Fi list ends when the firmware says so (and at least
 * 300 ms passed), after 2 s of listening on backends without a firmware scan,
 * and after 15 s whatever happens. */
constexpr bool is_scan_complete(bool scan_started, bool backend_scan_running, uint64_t elapsed_ms,
                                const RadioCapabilities &c) {
  if (!scan_started) return false;
  if (elapsed_ms > 15000) return true;
  return c.supports_umac_scan ? !backend_scan_running && elapsed_ms > 300 : elapsed_ms > 2000;
}

/* What the radio logs when the service hands it a station config the
 * backend cannot use; empty when it can. The kernel driver's wording is the
 * one the radio always printed. */
inline std::string station_policy_message(const RadioCapabilities &c) {
  if (c.supports_station) return std::string();
  if (c.windows_owns_station) return "station config ignored: Windows runs the station on the kernel driver";
  return std::string("station config ignored: backend ") + c.backend + " has no Wi-Fi station";
}

/* One line at start, after the backend opened, for reports: what this
 * backend does. Worded to match none of the patterns installer/report.ps1
 * and Supervisor.cs look for in radio lines (iwx_selftest checks). */
inline std::string format_capabilities_summary(const RadioCapabilities &c) {
  const auto yn = [](bool b) { return b ? "yes" : "no"; };
  std::string s = std::string("backend ") + c.backend + ": station " +
                  (c.supports_station ? "yes" : c.windows_owns_station ? "windows" : "no");
  s += std::string(", firmware-scan ") + yn(c.supports_umac_scan);
  s += std::string(", rx-ba ") + yn(c.supports_rx_ba);
  s += std::string(", promiscuous-toggle ") + yn(c.supports_promiscuous_toggle);
  s += std::string(", aggregate ") + yn(c.supports_aggregate);
  s += std::string(", batched-kick ") + yn(c.supports_batched_kick);
  s += std::string(", country-notice ") + yn(c.supports_country_notice);
  s += std::string(", set-country ") + yn(c.supports_set_country);
  s += std::string(", passive-channel-rule ") + yn(c.enforces_passive_channel);
  /* No std::to_string: the kernel build has no CRT formatting. */
  char n[4] = {};
  unsigned w = c.max_tx_window, i = 3;
  do {
    n[--i] = static_cast<char>('0' + w % 10);
    w /= 10;
  } while (w && i);
  s += std::string(", tx-window up to ") + (n + i);
  return s;
}

/* ---- The compile-time contract ---- */
namespace detail {
#define RADIO_DETECT(name, ...)                                                    \
  template <class D, class = void> struct name : std::false_type {};               \
  template <class D> struct name<D, std::void_t<decltype(__VA_ARGS__)>> : std::true_type {}
#define RADIO_DEV std::declval<D &>()
#define RADIO_CDEV std::declval<const D &>()
#define RADIO_P6 std::declval<const uint8_t *>()

/* Every backend: open, listen on a channel, hand up frames, inject frames. */
RADIO_DETECT(has_core,
             (void)RADIO_DEV.open(std::declval<const RadioOpenOptions &>(), std::declval<std::string &>()),
             (void)RADIO_DEV.start_monitor(uint8_t{}, 0u, RADIO_P6),
             (void)RADIO_DEV.set_channel(uint8_t{}, 0u), (void)RADIO_DEV.set_address(RADIO_P6),
             (void)RADIO_DEV.transmit(RADIO_P6, size_t{}, std::declval<const TxRate &>()),
             (void)RADIO_DEV.transmit_slot(RADIO_P6, size_t{}, std::declval<const TxRate &>(), false),
             (void)RADIO_DEV.set_rx_handler(RadioRxHandler{}),
             (void)RADIO_DEV.set_tx_status_handler(RadioTxStatusHandler{}), (void)RADIO_DEV.poll(),
             (void)RADIO_DEV.check_health(), (void)RADIO_CDEV.healthy(), (void)RADIO_CDEV.tx_stats(),
             (void)RADIO_CDEV.channel(), (void)RADIO_CDEV.width_mhz(), (void)RADIO_CDEV.hw_address(),
             (void)RADIO_DEV.stop());
RADIO_DETECT(has_station,
             (void)RADIO_DEV.sta_transmit(RADIO_P6, size_t{}, std::declval<const TxRate &>(), false),
             (void)RADIO_CDEV.sta_tx_acked(), (void)RADIO_CDEV.sta_tx_failed());
RADIO_DETECT(has_umac_scan,
             (void)RADIO_DEV.umac_scan(RADIO_P6, size_t{}, RADIO_P6, size_t{}, RADIO_P6),
             (void)RADIO_CDEV.scan_running());
RADIO_DETECT(has_rx_ba,
             (void)RADIO_DEV.add_rx_ba(RADIO_P6, 0u, uint16_t{}, uint16_t{}, uint8_t{}),
             (void)RADIO_DEV.remove_rx_ba(0u), (void)RADIO_CDEV.rx_ba_open(0u));
RADIO_DETECT(has_promiscuous_toggle, (void)RADIO_CDEV.promiscuous(), (void)RADIO_DEV.set_promiscuous(false));
RADIO_DETECT(has_aggregate,
             (void)RADIO_CDEV.agg_available(), (void)RADIO_CDEV.agg_ready(),
             (void)RADIO_DEV.agg_setup(RADIO_P6, false, false), (void)RADIO_DEV.agg_enable(false),
             (void)RADIO_DEV.agg_pause(false), (void)RADIO_DEV.agg_flush(),
             (void)RADIO_DEV.transmit_agg(RADIO_P6, size_t{}, false), (void)RADIO_DEV.kick_agg(),
             (void)RADIO_CDEV.agg_queued(), (void)RADIO_CDEV.agg_queue_cap(),
             (void)RADIO_CDEV.agg_seq(), (void)RADIO_CDEV.ba_epoch(),
             (void)RADIO_CDEV.agg_stats().frames);
RADIO_DETECT(has_batched_kick, (void)RADIO_DEV.kick_data());
/* current_country(): the domain the firmware last reported, for --fast-unlock. */
RADIO_DETECT(has_country_notice, (void)RADIO_DEV.answer_country_notice(std::declval<std::string *>()),
             (void)RADIO_CDEV.current_country());
RADIO_DETECT(has_set_country, (void)RADIO_DEV.set_country(std::declval<const std::string &>()));
RADIO_DETECT(has_channel_flags, (void)RADIO_CDEV.channel_flags(uint8_t{}));
RADIO_DETECT(has_capabilities, (void)D::kCapabilities.backend);

#undef RADIO_P6
#undef RADIO_CDEV
#undef RADIO_DEV
#undef RADIO_DETECT
} // namespace detail

/* static_assert(radio::check_backend_contract<Dev>()) where a backend is used. */
template <class D> constexpr bool check_backend_contract() {
  static_assert(detail::has_capabilities<D>::value, "backend: declare static constexpr RadioCapabilities kCapabilities");
  static_assert(detail::has_core<D>::value, "backend: the core listener/injector calls are missing");
  static_assert(validate_capabilities(D::kCapabilities), "backend: kCapabilities is inconsistent (capabilities_problem)");
  static_assert(!D::kCapabilities.supports_station || detail::has_station<D>::value,
                "backend: supports_station without sta_transmit/sta_tx_acked/sta_tx_failed");
  static_assert(!D::kCapabilities.supports_umac_scan || detail::has_umac_scan<D>::value,
                "backend: supports_umac_scan without umac_scan/scan_running");
  static_assert(!D::kCapabilities.supports_rx_ba || detail::has_rx_ba<D>::value,
                "backend: supports_rx_ba without add_rx_ba/remove_rx_ba/rx_ba_open");
  static_assert(!D::kCapabilities.supports_promiscuous_toggle || detail::has_promiscuous_toggle<D>::value,
                "backend: supports_promiscuous_toggle without promiscuous/set_promiscuous");
  static_assert(!D::kCapabilities.supports_aggregate || detail::has_aggregate<D>::value,
                "backend: supports_aggregate without the agg_* calls");
  static_assert(!D::kCapabilities.supports_batched_kick || detail::has_batched_kick<D>::value,
                "backend: supports_batched_kick without kick_data");
  static_assert(!D::kCapabilities.supports_country_notice || detail::has_country_notice<D>::value,
                "backend: supports_country_notice without answer_country_notice/current_country");
  static_assert(!D::kCapabilities.supports_set_country || detail::has_set_country<D>::value,
                "backend: supports_set_country without set_country");
  static_assert(!D::kCapabilities.enforces_passive_channel || detail::has_channel_flags<D>::value,
                "backend: enforces_passive_channel without channel_flags");
  return true;
}

} // namespace radio
