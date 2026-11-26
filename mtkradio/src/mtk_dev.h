// SPDX-License-Identifier: GPL-2.0-only
/*
 * User-mode PCIe driver for MediaTek MT7922 / MT7921 (Connac2) on Windows,
 * built on nekodrop's UMDF register proxy (RegProxy) and 32-bit AWE DMA pool.
 *
 * Interrupt-free: polls WFDMA descriptor DDONE bits directly from user mode.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "mtk_dma.h"
#include "mtk_regs.h"
#include "../../iwxradio/src/proxy.h"

namespace mtk {

struct RxFrame {
  const uint8_t *data = nullptr; /* raw 802.11 frame, no FCS */
  size_t len = 0;
  int8_t dbm = -127;
  uint8_t channel = 0;
};

struct TxParams {
  bool is_ht = false;
  bool is_vht = false;
  uint8_t rate_mbps = 6; /* Legacy OFDM Mbps (6..54) when !is_ht && !is_vht */
  uint8_t mcs = 0;       /* HT MCS (0..15) or VHT MCS (0..9) */
  uint8_t nss = 1;       /* Spatial streams (1..2) when is_vht */
  bool sgi = false;      /* Short GI when is_ht || is_vht */
  uint8_t bw = 0;        /* 0 = 20 MHz, 1 = 40 MHz, 2 = 80 MHz */
  bool no_ack = false;   /* IEEE80211_RADIOTAP_F_TX_NOACK (0x0008) */
};

/* Strip radiotap header and extract TX rate (Legacy OFDM / HT MCS+SGI+BW) and NO_ACK flag. */
bool parse_tx_radiotap(const uint8_t *buf, size_t len, const uint8_t *&frame, size_t &frame_len,
                       TxParams &params);

struct Ring {
  uint32_t reg_base = 0; /* BAR0 offset of desc_base */
  uint32_t count = 0;
  uint32_t head = 0;     /* next descriptor to submit (TX) or refill (RX) */
  uint32_t tail = 0;     /* next descriptor to reclaim (TX) or consume (RX) */
  DmaBuf desc_buf;       /* array of `count` MtkDesc */
  std::vector<DmaBuf> bufs; /* per-slot 4 KiB payload buffer */
  std::vector<DmaBuf> txwi; /* per-slot 64-byte TXD+TXP buffer (TX data only) */
  std::vector<uint8_t> rx_reasm; /* multi-descriptor RX A-MSDU reassembly (up to 12 KiB) */
  bool rx_reasm_drop = false;    /* discard trailing descriptors of an oversized/corrupt frame */

  MtkDesc *descs() const { return reinterpret_cast<MtkDesc *>(desc_buf.va); }
};

class MtkDevice {
public:
  struct Options {
    std::string firmware_dir;
    std::string country = "US";
    bool verbose = false;
  };

  struct PeerCaps {
    uint16_t ht_cap = 0x01ef;    /* LDPC, 40 MHz, SGI 20/40, RX STBC */
    uint8_t ampdu_params = 0x03; /* 64 KiB A-MPDU, no spacing rule */
    uint8_t rx_mcs[2] = {0xff, 0xff};
    bool vht = true;
    uint32_t vht_cap = 0x038001b2; /* MPDU 11454, RX LDPC, SGI80, TX/RX STBC, A-MPDU exp 7 */
    uint16_t vht_mcs = 0xfffa;     /* MCS 0-9 on two streams */
    uint8_t bw = 2;                /* 80 MHz */
    uint8_t nss = 2;
  };

  /* Own MAC 1 / BSS 1 and reserved WTBL slot 18 for the Wi-Fi station's AP
   * (AWDL peer slots occupy 1..17, below MT792X_WTBL_RESERVED = 19). */
  static constexpr uint8_t kStationOmac = 1;
  static constexpr uint8_t kApWcid = 18;

  using Logger = std::function<void(const std::string &)>;
  using RxHandler = std::function<void(const RxFrame &)>;
  using TxStatusHandler = std::function<void(unsigned token, bool acked, unsigned tries)>;
  using StopCheck = std::function<bool()>;

  explicit MtkDevice(Logger log) : _log(std::move(log)) {}
  ~MtkDevice();

  void set_tx_status_handler(TxStatusHandler h) { _on_tx_status = std::move(h); }
  void set_stop_check(StopCheck fn) { _stopping = std::move(fn); }

  /* Safe hardware probe: opens RegProxy, reads MT_HW_CHIPID/REV, tests driver-own. */
  bool probe(std::string &error);

  /* Hardware RGU whole-subsystem reset + release ownership (helps OEM mtkwlex rebind). */
  bool hard_reset(std::string &error);

  /* Full bring-up: probe + 32-bit DMA pool + WFDMA rings + firmware download + MAC init. */
  bool open(const Options &options, std::string &error);

  /* Starts raw 802.11 monitor/AWDL reception and hardware ACK for `address`. */
  bool start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]);
  bool set_channel(uint8_t channel, unsigned width_mhz, uint8_t switch_reason = CH_SWITCH_NORMAL);
  bool set_address(const uint8_t address[6]);

  /* Second own address (own MAC 1, BSS 1) for the concurrent Wi-Fi station. */
  bool set_station_identity(const uint8_t addr[6], const uint8_t bssid[6]);
  void clear_station_identity();

  /* Firmware STA record and Block Ack for the Wi-Fi station's AP (or another peer). */
  bool sta_add(uint8_t wcid, const uint8_t peer[6], const PeerCaps &caps, uint8_t omac = 0);
  bool sta_remove(uint8_t wcid, const uint8_t peer[6], uint8_t omac = 0);
  bool sta_ba(uint8_t wcid, const uint8_t peer[6], uint8_t tid, uint16_t ssn, uint16_t win,
              bool tx, bool enable, uint8_t omac = 0);

  /* MCU timeouts for runtime channel moves and STA/BA commands (ms). */
  unsigned move_timeout_ms = 3000;
  unsigned sta_cmd_timeout_ms = 3000;

  /* Connac2 TXD6 TX rate values (matching Mt7902::tx_rate_*). */
  static uint16_t tx_rate_ofdm(unsigned mbps);
  static uint16_t tx_rate_cck(unsigned mbps_x2);
  static uint16_t tx_rate_ht(unsigned mcs);

  /* Injects a raw 802.11 frame (no FCS) with explicit TX rate / HT MCS / NO_ACK parameters.
   * Pass kick=false when enqueueing a batch, then call kick_tx() once to ring the WFDMA doorbell. */
  bool transmit(const uint8_t *frame, size_t len, const TxParams &params, bool kick = true);
  /* Convenience overload for fixed legacy OFDM rate (e.g. 6 or 24 Mbps). */
  bool transmit(const uint8_t *frame, size_t len, unsigned rate_mbps = 6, bool kick = true);
  /* Fixed-rate TX for the Wi-Fi station / scans (no AWDL BA snooping). Returns token or -1. */
  int transmit_raw(const uint8_t *frame, size_t len, uint16_t rate_val, uint8_t omac = 0,
                   bool no_ack = false, bool force_altx = false);
  /* QoS data frame via firmware rate control for `wcid` under `omac`. Returns token or -1. */
  int transmit_sta(const uint8_t *frame, size_t len, uint8_t wcid, uint8_t omac = 0);
  /* Flushes deferred TX ring CIDX doorbell write (one BAR0 MMIO write per batch). */
  void kick_tx();

  /* Polls RX data + MCU rings and invokes `on_rx` for each received 802.11 frame. */
  size_t poll(const RxHandler &on_rx);

  void close();

  uint32_t chip_id() const { return _chip_id; }
  uint32_t hw_rev() const { return _hw_rev; }
  uint8_t channel() const { return _channel; }
  unsigned width_mhz() const { return _width_mhz; }

  /* Low-level register helpers (exposed for diagnostics). */
  uint32_t rr(uint32_t addr);
  void wr(uint32_t addr, uint32_t val);
  void rmw(uint32_t addr, uint32_t mask, uint32_t val);
  void set(uint32_t addr, uint32_t bits) { rmw(addr, 0, bits); }
  void clear(uint32_t addr, uint32_t bits) { rmw(addr, bits, 0); }
  uint32_t l1_rr(uint32_t addr);
  void l1_wr(uint32_t addr, uint32_t val);
  bool poll_reg(uint32_t addr, uint32_t mask, uint32_t want, unsigned timeout_ms);

private:
  uint32_t translate_addr(uint32_t addr);
  uint32_t remap_l1(uint32_t addr);

  bool driver_own(std::string &error);
  bool wfsys_reset(std::string &error);
  bool dma_disable(bool force, std::string &error);
  bool dma_init(std::string &error);
  bool dma_enable(std::string &error);
  bool alloc_tx_ring(Ring &r, uint32_t reg_base, uint32_t count, bool with_txwi, std::string &error);
  bool alloc_rx_ring(Ring &r, uint32_t reg_base, uint32_t count, std::string &error);
  void program_ring(const Ring &r, bool is_rx);

  bool tx_ring_push(Ring &r, const void *data, size_t len, bool wait_done = false);
  void tx_ring_reclaim(Ring &r);
  bool rx_ring_pop(Ring &r, std::vector<uint8_t> &out, bool commit = true);
  void rx_ring_commit(Ring &r);

  /* MCU commands */
  bool mcu_send(uint8_t cid, uint8_t ext_cid, uint8_t set_query,
                const void *payload, size_t len, bool wait_rsp,
                std::vector<uint8_t> *rsp = nullptr, unsigned timeout_ms = 3000);
  bool mcu_send_uni(uint16_t cid, const void *payload, size_t len,
                    bool wait_rsp, unsigned timeout_ms = 3000);
  bool mcu_wait_response(uint8_t seq, std::vector<uint8_t> &out, unsigned timeout_ms);
  bool mcu_send_scatter(const uint8_t *data, size_t len, std::string &error);

  /* Firmware loading */
  bool load_firmware(const std::string &fw_dir, std::string &error);
  bool load_patch(const std::string &path, std::string &error);
  bool load_ram(const std::string &path, std::string &error);
  int mcu_patch_sem_ctrl(bool get);
  bool mcu_init_download(uint32_t addr, uint32_t len, uint32_t mode);
  bool mcu_start_patch();
  bool mcu_start_firmware(uint32_t override_addr, uint32_t option);

  /* MAC / Channel / RX parsing / Hardware RX Block Ack */
  bool mac_init(std::string &error);
  bool mcu_set_eeprom();
  bool mcu_set_chan_cmd(uint8_t ext_cid, uint8_t ch, unsigned width_mhz,
                        uint8_t switch_reason = CH_SWITCH_NORMAL);
  bool add_omac(uint8_t omac, const uint8_t addr[6], const uint8_t bssid[6], bool enable);
  void set_timing();
  void apply_rx_filter_mcu();
  void restore_rx_filter();
  void handle_tx_free(const uint8_t *data, size_t len);
  void handle_rx_meta(const uint8_t *buf, size_t len);
  int queue_tx_frame(const uint8_t *frame, size_t len, const uint32_t txd[8], bool kick);
  bool parse_rx_packet(const uint8_t *buf, size_t len, RxFrame &out);

  int find_peer_wcid(const uint8_t mac[6]);
  int get_or_add_peer_wcid(const uint8_t mac[6]);
  bool mcu_register_peer(uint8_t wcid, const uint8_t mac[6]);
  bool mcu_set_rx_ba(uint8_t wcid, const uint8_t mac[6], uint8_t tid, uint16_t ssn,
                     uint16_t winsize, bool amsdu, bool enable);
  bool mcu_set_tx_ba(uint8_t wcid, const uint8_t mac[6], uint8_t tid, uint16_t ssn,
                     uint16_t winsize, bool amsdu, bool enable);
  void snoop_rx_ba(const uint8_t *frame, size_t len);
  void snoop_tx_ba(const uint8_t *frame, size_t len);

  void log(const std::string &msg);

  struct PeerEntry {
    bool in_use = false;
    std::array<uint8_t, 6> mac{};
    std::array<bool, 8> tx_ba_active{};
    uint64_t last_used_ms = 0;
  };
  struct PendingAddba {
    bool valid = false;
    std::array<uint8_t, 6> peer_mac{};
    uint8_t tid = 0;
    uint8_t token = 0;
    uint16_t ssn = 0;
    bool amsdu = true;
    uint64_t ts_ms = 0;
  };
  struct DeferredRx {
    std::vector<uint8_t> frame;
    int8_t dbm;
    uint8_t channel;
  };

  Logger _log;
  TxStatusHandler _on_tx_status;
  StopCheck _stopping;
  RegProxy _proxy;
  DmaPool32 _dma;

  uint32_t _chip_id = 0;
  uint32_t _hw_rev = 0;
  uint32_t _l1_base = 0xffffffffu;
  uint8_t _mcu_seq = 0;
  uint16_t _tx_token = 0;

  uint8_t _channel = 149;
  unsigned _width_mhz = 80;
  std::string _country = "US";
  std::array<uint8_t, 6> _mac{};
  std::array<uint8_t, 6> _sta_addr{};
  std::array<uint8_t, 6> _sta_bssid{};
  bool _sta_iface_up = false;
  bool _verbose = false;
  bool _dma_running = false;
  bool _tx_pending_kick = false;
  bool _fw_loaded = false;
  bool _rx_path_set = false;

  Ring _tx_data;
  Ring _tx_fwdl;
  Ring _tx_mcu;
  Ring _rx_mcu;
  Ring _rx_mcu_wa;
  Ring _rx_data;

  std::array<PeerEntry, MT792X_WTBL_RESERVED> _peers{}; /* slots 1..18 */
  std::array<PendingAddba, 32> _pending_addba{};
  std::array<PendingAddba, 32> _pending_tx_addba{};
  std::vector<uint8_t> _rx_pkt_buf;
  std::vector<uint8_t> _rx_unpad_buf;
  std::deque<DeferredRx> _deferred_rx;
};

} // namespace mtk
