// SPDX-License-Identifier: GPL-2.0-only
/* User-mode driver for the MediaTek MT7902 (PCIe, connac2), a port of the
 * parts of Linux mt76/mt7921 (Lorenzo Bianconi, Sean Wang, MediaTek) that a
 * listener/injector needs: register access through the register proxy, the
 * WFDMA rings in AWE pages below 4 GiB, the MCU message channel, and the ROM
 * patch and RAM firmware download. No interrupts: the rings are polled. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dma.h"
#include "mt_regs.h"
#include "proxy.h"

class Mt7902 {
public:
  using Logger = std::function<void(const std::string &)>;

  Mt7902() = default;
  ~Mt7902();
  Mt7902(const Mt7902 &) = delete;
  Mt7902 &operator=(const Mt7902 &) = delete;

  void set_logger(Logger log) { _log = std::move(log); }

  /* Steps of open(), each usable on its own by the probe. */
  bool attach(std::string &error);          /* register proxy, ownership, chip id */
  bool reset_and_dma(std::string &error);   /* WFSYS reset, rings, DMA on */
  bool load_firmware(const std::string &firmware_dir, std::string &error);
  bool query_capabilities(std::string &error); /* MAC address from the firmware */
  bool open(const std::string &firmware_dir, std::string &error);
  void close();

  uint16_t chip_id() const { return _chip; }
  uint8_t chip_rev() const { return _rev; }
  const uint8_t *mac_address() const { return _mac; }
  const std::string &firmware_version() const { return _fw_version; }

  /* Registers by chip bus address (translated as mt7921's __mt7921_reg_addr). */
  uint32_t rr(uint32_t addr);
  void wr(uint32_t addr, uint32_t value);
  uint32_t rmw(uint32_t addr, uint32_t mask, uint32_t value);
  void set(uint32_t addr, uint32_t bits) { rmw(addr, 0, bits); }
  void clear(uint32_t addr, uint32_t bits) { rmw(addr, bits, 0); }
  bool poll(uint32_t addr, uint32_t mask, uint32_t value, unsigned timeout_ms);
  uint32_t l1_rr(uint32_t addr);
  void l1_wr(uint32_t addr, uint32_t value);

  /* An MCU command. `cid` with `ext_cid` 0 and `ce` false is a plain command.
   * With `wait`, returns the firmware's answer (its RXD header first) in
   * `answer`. Returns false on a full ring or no answer in time. */
  struct Command {
    uint8_t cid = 0;
    uint8_t ext_cid = 0;
    bool ce = false;
    bool query = false;
    bool uni = false; /* connac2 unified command: cid is the 16-bit UNI id */
  };
  static Command ext_cmd(uint8_t id) {
    Command c;
    c.cid = mt::kMcuCmdExtCid;
    c.ext_cid = id;
    return c;
  }
  static Command ce_cmd(uint8_t id) {
    Command c;
    c.cid = id;
    c.ce = true;
    return c;
  }
  static Command uni_cmd(uint8_t id) {
    Command c;
    c.cid = id;
    c.uni = true;
    return c;
  }

  /* ---- Radio (mt7921_radio.cpp): a listener and injector for AWDL ---- */
  struct RxFrame {
    const uint8_t *data; /* 802.11 header first, no FCS */
    size_t len;
    int8_t dbm;
    uint8_t channel;
  };
  using RxHandler = std::function<void(const RxFrame &)>;
  using TxStatusHandler = std::function<void(unsigned token, bool acked, unsigned tries)>;
  void set_rx_handler(RxHandler h) { _on_rx = std::move(h); }
  void set_tx_status_handler(TxStatusHandler h) { _on_tx_status = std::move(h); }

  /* After open(): EEPROM from efuse, MAC on, channel domain, interface,
   * sniffer on `channel`. `country` is two letters. */
  bool radio_start(uint8_t channel, uint8_t width_mhz, const std::string &country, std::string &error);
  bool set_channel(uint8_t channel, uint8_t width_mhz);
  /* How set_channel moves (the station's time sharing pays it twice an
   * entry): the sniffer's own channel too, and whether to wait for the
   * firmware's answers. Microseconds of the last move's steps: RF channel
   * switch, sniffer, MAC timing. */
  bool move_sniffer = true, move_wait = true;
  /* How long a move waits for the firmware's answer: now and then it takes
   * a third of a second, longer than a router lets a station be away. */
  unsigned move_timeout_ms = 3000;
  uint64_t last_move_us[3] = {};
  /* The address the card acknowledges unicast frames for, and the BSSID of
   * its (ad-hoc) interface. */
  bool set_identity(const uint8_t own[6], const uint8_t bssid[6]);
  /* A second own address, for the Wi-Fi station (own MAC 1, BSS 1): the
   * MAC acknowledges unicast frames to it too. `bssid` is the AP's. */
  bool set_station_identity(const uint8_t addr[6], const uint8_t bssid[6]);
  void clear_station_identity();
  static constexpr uint8_t kStationOmac = 1;
  /* Queues one 802.11 frame (no FCS) at `rate` (TXD rate value, see
   * tx_rate_*), from own MAC `omac`. Returns its token (named again in the
   * TX status), or -1 when every TX slot is still out or the ring is full. */
  int transmit(const uint8_t *frame, size_t len, uint16_t rate, uint8_t omac = 0);
  /* Test knobs for transmit(): -1 keeps the default. */
  int force_no_ack = -1, force_altx = -1;
  unsigned tx_slots_free() const { return static_cast<unsigned>(_tx_free_slots.size()); }
  /* Drains the RX rings and the firmware's TX-done reports. Returns frames handled. */
  unsigned poll();
  uint8_t channel() const { return _channel; }
  uint8_t width_mhz() const { return _width; }
  /* Diagnostics: RX packets by RXD type, and the data TX ring's indices. */
  uint64_t rx_types[32] = {};
  std::string tx_ring_state();
  uint32_t rx_filter_reg() { return rr(0x820e5000); /* MT_WF_RFCR(0) */ }

  /* ---- Peer stations (mt7902_sta.cpp): firmware rate control and A-MPDU ---- */
  struct PeerCaps {
    uint16_t ht_cap = 0x01ef;    /* LDPC, 40 MHz, SGI 20/40, RX STBC */
    uint8_t ampdu_params = 0x03; /* 64 KiB A-MPDU, no spacing rule */
    uint8_t rx_mcs[2] = {0xff, 0xff};
    bool vht = true;
    uint32_t vht_cap = 0x038001b2; /* MPDU 11454, RX LDPC, SGI80, TX/RX STBC, A-MPDU exp 7 */
    uint16_t vht_mcs = 0xfffa;   /* MCS 0-9 on two streams */
    uint8_t bw = 2;              /* IEEE80211_STA_RX_BW_80 */
    uint8_t nss = 2;
  };
  static constexpr uint8_t kFirstPeerWcid = 1, kLastPeerWcid = 8;
  /* The Wi-Fi station's access point, under own MAC 1 (mt_station.h). */
  static constexpr uint8_t kApWcid = 9;
  /* How long STA record commands wait for the firmware's answer. */
  unsigned sta_cmd_timeout_ms = 3000;
  /* `omac`: the own MAC (and BSS) the peer talks to: 0 for AWDL. */
  bool sta_add(uint8_t wcid, const uint8_t peer[6], const PeerCaps &caps, uint8_t omac = 0);
  bool sta_remove(uint8_t wcid, const uint8_t peer[6], uint8_t omac = 0);
  bool sta_ba(uint8_t wcid, const uint8_t peer[6], uint8_t tid, uint16_t ssn, uint16_t win, bool tx, bool enable,
              uint8_t omac = 0);
  /* A QoS data frame to a peer station: the firmware's rate, Block Ack
   * allowed, the frame's own sequence number kept. Token or -1. */
  int transmit_sta(const uint8_t *frame, size_t len, uint8_t wcid, uint8_t omac = 0);

  static uint16_t tx_rate_ofdm(unsigned mbps);
  static uint16_t tx_rate_cck(unsigned mbps_x2);
  static uint16_t tx_rate_ht(unsigned mcs);
  bool mcu_send(const Command &cmd, const void *data, size_t len, bool wait,
                std::vector<uint8_t> *answer = nullptr, unsigned timeout_ms = 3000, bool log_timeout = true);

  /* Frames and events the firmware sent that no command waited for. */
  using EventHandler = std::function<void(const uint8_t *data, size_t len)>;
  void set_event_handler(EventHandler h) { _on_event = std::move(h); }
  /* Takes what the MCU ring holds and hands it to the event handler. */
  void poll_events();

private:
  struct TxRing {
    unsigned hw = 0;
    unsigned n = 0;
    unsigned head = 0;
    DmaBuf desc;
    std::vector<DmaBuf> bufs; /* slot s uses bufs[s % bufs.size()] */
  };
  struct RxRing {
    unsigned hw = 0;
    unsigned n = 0;
    unsigned tail = 0; /* next descriptor the card fills */
    uint32_t buf_len = 0;
    DmaBuf desc;
    std::vector<DmaBuf> bufs; /* one per descriptor */
  };

  uint32_t reg_addr(uint32_t addr);
  uint32_t map_l1(uint32_t addr);
  void log(const std::string &line) const;

  bool alloc_tx(TxRing &r, unsigned hw, unsigned n, unsigned nbufs, std::string &error);
  bool alloc_rx(RxRing &r, unsigned hw, unsigned n, uint32_t buf_len, std::string &error);
  void reset_tx(TxRing &r);
  void reset_rx(RxRing &r);
  bool dma_disable(bool force);
  void dma_enable();
  /* Queues one buffer on a TX ring and rings the doorbell. */
  bool tx_raw(TxRing &r, const void *data, size_t len, unsigned timeout_ms = 1000);
  /* One complete frame from an RX ring (segments joined), or false if none. */
  bool rx_take(RxRing &r, std::vector<uint8_t> &out);

  bool send_patch(const std::vector<uint8_t> &fw, std::string &error);
  bool send_ram(const std::vector<uint8_t> &fw, std::string &error);
  bool init_download(uint32_t addr, uint32_t len, uint32_t mode);
  bool send_scatter(const uint8_t *data, size_t len);
  int patch_sem(bool get);

  /* The pool outlives the proxy handle: closing the handle runs the close ops
   * that stop the card's DMA, and only then are its pages given back. */
  DmaPool _pool;
  RegProxy _proxy;
  Logger _log;
  EventHandler _on_event;
  uint32_t _l1_base = 0xffffffff; /* last L1 window programmed */
  uint16_t _chip = 0;
  uint8_t _rev = 0;
  uint8_t _seq = 0;
  uint8_t _mac[6] = {};
  std::string _fw_version;
  bool _dma_on = false;

  TxRing _tx_data, _tx_mcu, _tx_fwdl;
  RxRing _rx_mcu, _rx_data;

  /* Radio state */
  void handle_rx(const std::vector<uint8_t> &buf);
  void handle_tx_free(const uint8_t *data, size_t len);
  bool add_interface(bool enable);
  bool add_omac(uint8_t omac, const uint8_t addr[6], const uint8_t bssid[6], bool enable);
  bool sniffer(bool enable);
  bool sniffer_config(bool wait = true);
  bool channel_switch(uint8_t ext_cmd_id, bool wait = true);
  void set_timing(); /* mt792x_mac_set_timeing */
  /* Copies the frame into a free TX slot and queues it with this TXD. */
  int queue_frame(const uint8_t *frame, size_t len, uint32_t txd[8]);
  struct TxSlot {
    DmaBuf page; /* TXD + TXP at 0, the frame at kTxFrameOffset */
  };
  std::vector<TxSlot> _tx_slots;
  std::vector<uint16_t> _tx_free_slots;
  RxHandler _on_rx;
  TxStatusHandler _on_tx_status;
  uint8_t _channel = 0, _width = 20;
  uint8_t _own[6] = {}, _bssid[6] = {};
  bool _iface_up = false;
  uint8_t _sta_addr[6] = {}, _sta_bssid[6] = {};
  bool _sta_iface_up = false;
  std::vector<uint8_t> _rx_scratch;
};
