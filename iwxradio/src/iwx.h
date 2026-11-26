// SPDX-License-Identifier: GPL-2.0-only
/* A user-mode driver for Intel Wi-Fi 6/6E cards of the 22000 family (AX200,
 * AX201, 9560 on Qu) and the AX210 family (AX210, AX211, AX411), reduced to
 * what an AWDL radio needs: boot the firmware,
 * hold one listener MAC context on one channel, hand up every frame heard and
 * inject frames.
 *
 * It is a port of the bring-up and data paths of itlwm's hal_iwx (OpenBSD
 * if_iwx, as extended by ForbiddenItlwm for AWDL) from the macOS kernel to a
 * Windows process:
 *   - registers go through the UMDF register proxy (proxy.h),
 *   - DMA memory is AWE-locked physical memory (dma.h),
 *   - interrupts are not used; the receive ring's status word, which the card
 *     writes by DMA, is polled.
 *
 * Single-threaded: every method must be called from the same thread, which
 * also runs poll(). Received frames are delivered from inside poll() and from
 * inside synchronous firmware commands. */
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cards.h"
#include "dma.h"
#include "fw.h"
#include "proxy.h"
/* RxFrame, TxRate, TxStats, StaParams, the open options and the backend
 * contract: shared with KernelDevice and future backends, which need none of
 * the headers above. */
#include "radio_backend.h"
/* Host-side Block Ack receive reordering (rx_reorder.h; RxFrame comes from
 * radio_backend.h above). */
#include "rx_reorder.h"

class IwxDevice {
public:
  /* The names this class always had, now the shared types (radio_backend.h),
   * so iwx.cpp, wifi_sta and kdriver/khal.cpp read the same. */
  using MacType = RadioMacType;
  using Options = RadioOpenOptions;
  using Logger = RadioLogger;
  using RxHandler = RadioRxHandler;
  /* Everything radio_main may call on this backend (radio_backend.h). */
  static constexpr RadioCapabilities kCapabilities = radio::iwx_capabilities();

  explicit IwxDevice(Logger log)
      : _log(std::move(log)), _reorder([this](const RxFrame &f) { emit_rx(f); }) {}
  ~IwxDevice();

  /* Opens the card, boots its firmware and runs the init sequence. */
  bool open(const Options &options, std::string &error);
  /* Adds the PHY/MAC/binding/station/queue for monitoring on `channel`
   * (1-14 or a 5 GHz number) at `width_mhz` (20, 40 or 80). `address` is the
   * MAC the listener answers to; it can change later. */
  bool start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]);
  bool set_channel(uint8_t channel, unsigned width_mhz);
  bool set_address(const uint8_t address[6]);
  bool uses_mld_listener() const { return use_mld(); }
  /* A Block Ack receive session with `peer` (ForbiddenItlwm's awdlBaHandle):
   * a firmware station for the peer, bound to the listener, and an immediate
   * BA agreement on `tid`. The firmware then answers aggregates by itself. */
  bool add_rx_ba(const uint8_t peer[6], unsigned tid, uint16_t ssn, uint16_t window, uint8_t token);
  /* `peer` null: the TID's sessions with every peer. */
  bool remove_rx_ba(unsigned tid, const uint8_t *peer = nullptr);
  /* On Bz the listener is never promiscuous (the firmware then stops matching
   * ACKs to its own unicast), so set_promiscuous is a no-op there. */
  bool bz_card() const { return _card.bz; }
  /* Bz with --mac-type sta: keeps a session protection running so the
   * unassociated BSS_STA listener link has the radio. Call often; no-op
   * otherwise. */
  bool keep_on_channel();
  /* Host-side Block Ack reordering counters (rx_reorder.h). */
  const RxReorder::Stats &reorder_stats() const { return _reorder.stats(); }
  /* Experimental A-MPDU sending to one AWDL peer (ForbiddenItlwm's
   * -itlawdlcoexbatx): the peer's firmware station (the one receive Block Ack
   * uses), an aggregation queue on TID 0 and firmware rate control for it. */
  bool agg_available() const { return _aggq.count != 0; }
  bool agg_ready() const { return _agg_queue_enabled; }
  bool agg_setup(const uint8_t peer[6], bool vht80, bool mimo);
  /* Falls back from experimental HE80 TX TLC to VHT80 if the peer/firmware gives no Block Acks. */
  bool agg_using_he80() const { return _agg_he80_active; }
  void agg_fallback_vht() { _he80_fallback = true; _agg_he80_active = false; }
  /* Lets the firmware aggregate TID 0 (after the peer's ADDBA answer), or stops it. */
  bool agg_enable(bool on);
  /* Holds the peer's frames in the firmware while it is away from our channel. */
  bool agg_pause(bool on);
  /* Flushes TID 0 of the peer station (after DELBA so SSN restarts cleanly). */
  bool agg_flush();
  int transmit_agg(const uint8_t *frame, size_t len, bool kick = true);
  void kick_agg();
  unsigned agg_queued() const { return _aggq.queued; }
  /* Up to 128 in flight (two BA windows), leaving 16 slots of headroom on AX210's 128-entry ring. */
  unsigned agg_queue_cap() const { return _aggq.count > 16u ? ((_aggq.count - 16u) < 128u ? (_aggq.count - 16u) : 128u) : 0u; }
  uint16_t agg_seq() const { return _agg_seq; }
  /* Goes up whenever the peer station is replaced: everything set up for the old one is gone. */
  uint32_t ba_epoch() const { return _ba_epoch; }
  const uint8_t *ba_peer() const { return _ba_station ? _ba_peer : nullptr; }
  using AggStats = RadioAggStats;
  const AggStats &agg_stats() const { return _agg_stats; }
  bool rx_ba_open(unsigned tid) const { return tid < 8 && (_ba_tids >> tid) & 1; }
  /* Promiscuous receive on the listener on/off (off while a BA session is busy). */
  bool set_promiscuous(bool on);
  bool promiscuous() const { return _promisc; }
  /* Asks the firmware for a regulatory domain ("CN", "US"...). True if taken. */
  bool set_country(const std::string &alpha2);
  /* The firmware's own country notice (MCC_CHUB_UPDATE, e.g. learned from
   * access points' Country elements), answered with the same country and
   * source, as Linux and ForbiddenItlwm do: the firmware takes back what it
   * reported where it refuses a country from any other source (#11). Called
   * from the main loop; true when a notice was answered, with what the
   * firmware then says in *got. */
  bool answer_country_notice(std::string *got);
  /* The domain the firmware last answered an MCC update with ("00" for the
   * world domain), "" before the first. At start this tells whether
   * --country was really taken, which set_country's caller cannot see once
   * open() returns (#11: refused with status 4, the firmware stayed "00"). */
  const std::string &current_country() const { return _current_mcc; }
  /* IWX_NVM_CHANNEL_* flags the firmware last reported for a channel (6, 36, 44, 149). */
  uint32_t channel_flags(uint8_t channel) const { return _chan_flags[channel]; }
  /* Queues one frame (802.11 header first, no FCS). False if the ring is full
   * or the frame is malformed. */
  bool transmit(const uint8_t *frame, size_t len, const TxRate &rate) {
    return transmit_slot(frame, len, rate) >= 0;
  }
  /* As transmit, returning the ring slot the frame took (-1 on failure); the
   * TX-status handler is later called with that slot. With kick false the
   * frame waits in the ring until kick_data() rings the doorbell once for a
   * whole batch (each doorbell is a call through the register proxy). */
  int transmit_slot(const uint8_t *frame, size_t len, const TxRate &rate, bool kick = true);
  void kick_data();

  /* ---- Station beside the listener (ForbiddenItlwm's coex mode) ----
   * MAC 1 is a BSS_STA context for the access point, on PHY 1 and binding 1,
   * or in the listener's binding when both are on one channel; the firmware
   * time-shares the radio between the two bindings. */
  bool sta_join(const StaParams &p);
  bool sta_associated(uint16_t aid, uint8_t dtim_period);
  /* What association agreed on (HT/VHT/width), before sta_associated. */
  void sta_join_update(const StaParams &p) {
    _sta.ht = p.ht;
    _sta.vht = p.vht;
    _sta.mimo = p.mimo;
    _sta.width = p.width;
    _sta.beacon_int = p.beacon_int;
  }
  bool sta_set_key(bool group, unsigned key_index, const uint8_t key[16]);
  /* 802.11w: management frame protection for the AP station, and the AP's
   * integrity group key (BIP-CMAC-128, key ID 4 or 5). */
  bool sta_set_mfp(bool on);
  bool sta_set_igtk(unsigned key_id, const uint8_t key[16], uint64_t rsc);
  /* Firmware rate control for the AP (itlwm's iwx_rs_init); afterwards
   * frames sent with TxRate::Auto use it. */
  bool sta_rate_control();
  bool sta_tlc() const { return _sta_tlc; }
  unsigned sta_tx_acked() const { return _sta_tx_acked; }
  unsigned sta_tx_failed() const { return _sta_tx_failed; }
  /* Frames queued to the station's ring; tx_stats().submitted counts the
   * AWDL data ring only, the ring its done/failed counts come from. */
  unsigned sta_tx_submitted() const { return _sta_tx_submitted; }
  /* `encrypt`: the station's keys are installed and the frame carries the
   * protected bit; the firmware adds the CCMP header and MIC. */
  int sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt);
  bool sta_add_rx_ba(unsigned tid, uint16_t ssn, uint16_t window);
  bool sta_remove_rx_ba(unsigned tid);
  void sta_leave();
  bool sta_active() const { return _sta_mac_added; }
  const StaParams &sta_params() const { return _sta; }
  using TxStatusHandler = RadioTxStatusHandler;
  void set_tx_status_handler(TxStatusHandler handler) { _on_tx = std::move(handler); }
  /* Firmware scan (iwx_scan.cpp): the firmware visits `channels` between the
   * station's and listener's slots; beacons and probe responses arrive through
   * the receive handler; scan_running() turns false at IWX_SCAN_COMPLETE_UMAC.
   * `ssid` (may be empty) makes it an active, directed scan. */
  bool umac_scan(const uint8_t *channels, size_t count, const uint8_t *ssid, size_t ssid_len,
                 const uint8_t address[6]);
  bool scan_running() const { return _scan_running; }
  bool scan_succeeded() const { return !_scan_running && _scan_succeeded; }
  void scan_abort();
  /* Handles whatever the card has delivered since the last call. Returns the
   * number of receive buffers processed. */
  int poll();
  /* Looks for a firmware or DMA error (about one register read); on one,
   * logs the firmware's error logs and marks the device unhealthy. */
  void check_health();
  /* Stops DMA and resets the card. Safe to call twice. */
  void stop();

  void set_rx_handler(RxHandler handler) { _on_rx = std::move(handler); }
  const TxStats &tx_stats() const { return _tx_stats; }
  uint8_t channel() const { return _channel; }
  unsigned width_mhz() const { return _width; }
  bool healthy() const { return _open && !_fw_error && !_proxy.failed(); }
  const std::array<uint8_t, 6> &hw_address() const { return _hw_addr; }
  const CardInfo &card() const { return _card; }

private:
  struct TxRing {
    DmaBuf desc, bc_tbl, cmd;
    std::vector<DmaBuf> payload; /* one 4 KiB page per slot */
    std::vector<DmaBuf> cmd_slot; /* data ring: one page per slot for the TX command */
    unsigned count = 0, qid = 0, cur = 0, tail = 0, queued = 0;
  };
  struct CmdSlot {
    bool pending = false, want_resp = false, narrow = false;
    std::vector<uint8_t> resp;
  };

  /* Registers */
  uint32_t rd(uint32_t off) { return _proxy.read32(off); }
  void wr(uint32_t off, uint32_t v) { _proxy.write32(off, v); }
  void setb(uint32_t off, uint32_t bits) { _proxy.set_bits(off, bits); }
  void clrb(uint32_t off, uint32_t bits) { _proxy.clear_bits(off, bits); }
  bool poll_bit(uint32_t reg, uint32_t bits, uint32_t mask, unsigned timeout_us);
  bool nic_lock();
  void nic_unlock();
  uint32_t read_prph(uint32_t addr);
  void write_prph(uint32_t addr, uint32_t value);
  /* UMAC peripherals: 0x300000 higher on the AX210 family (iwx_*_umac_prph). */
  uint32_t read_umac_prph(uint32_t addr) { return read_prph(addr + (gen3() ? 0x300000u : 0u)); }
  void write_umac_prph(uint32_t addr, uint32_t v) { write_prph(addr + (gen3() ? 0x300000u : 0u), v); }
  bool gen3() const { return _card.ax210; }
  bool bz() const { return _card.bz; }
  bool use_mld() const { return _use_mld; }
  void set_bits_prph(uint32_t addr, uint32_t bits);
  void clear_bits_prph(uint32_t addr, uint32_t bits);
  void sw_reset();
  void ring_rx_doorbell(unsigned idx);

  /* Bring-up */
  bool identify(std::string &error);
  bool alloc_rings(std::string &error);
  bool set_hw_ready();
  bool prepare_card_hw();
  void clear_persistence_bit();
  void force_power_gating();
  bool apm_init();
  void apm_stop();
  bool start_hw();
  void mask_interrupts();
  void nic_config();
  bool load_firmware(std::string &error);
  bool load_pnvm();
  bool run_init_sequence(std::string &error);
  void set_ltr();

  /* Firmware commands */
  int send_cmd(uint32_t id, const void *data, size_t len, bool want_resp,
               std::vector<uint8_t> *resp, unsigned timeout_ms = 2000);
  int send_cmd_status(uint32_t id, const void *data, size_t len, uint32_t *status);
  bool wait_for(const std::function<bool()> &done, unsigned timeout_ms);
  uint8_t cmd_version(uint8_t group, uint8_t cmd) const {
    return _fw.cmd_version(group, cmd);
  }
  uint8_t valid_tx_ant() const;
  uint8_t valid_rx_ant() const;
  int nvm_get();
  int update_mcc(const std::string &alpha2, int source, std::string *got);
  int phy_ctxt_cmd(uint32_t action);
  int mac_ctxt_cmd(uint32_t action);
  int binding_cmd(uint32_t action);
  int add_monitor_sta();
  int enable_monitor_queue();
  int monitor_smart_fifo(uint32_t state);
  /* MLD API (Bz / BE200) */
  int mld_mac_cmd(uint32_t action, const uint8_t *address = nullptr);
  int mld_link_add();
  int mld_link_modify(bool active, uint32_t modify_mask, const uint8_t *address = nullptr);
  int mld_add_monitor_sta();
  int mld_sta_mac_cmd(uint32_t action, bool assoc);
  int mld_sta_link_add();
  int mld_sta_link_modify(bool active, uint32_t modify_mask);
  int mld_sta_link_remove();
  int mld_sta_cfg_ap(bool assoc);
  int mld_sec_key_remove(uint32_t key_id, uint32_t key_flags);
  int disable_queue(uint8_t sta_id, uint8_t tid);
  /* Bz receive Block Ack: one firmware station per AWDL peer (stations
   * IWX_BZ_PEER_STA_ID..), and the firmware's BAID allocation/release. */
  int bz_peer_station(const uint8_t peer[6]);
  void bz_peer_drop(unsigned slot);
  void bz_recount_ba_tids();
  int bz_baid_add(unsigned sta_id, unsigned tid, uint16_t ssn, uint16_t window, uint32_t &baid);
  int bz_baid_remove(uint32_t baid, unsigned tid, unsigned sta_id);
  int bz_session_protect(unsigned link_id, unsigned mac_id, uint32_t duration_tu);
  std::string bz_boot_regs(); /* FSEQ/IML/ROM status; caller holds nic_lock */
  /* 0: healthy, 1: firmware/DMA error, 2: the device dropped off the bus.
   * `what` describes the registers when not healthy. On Bz the old SW_ERR
   * position (CSR_INT bit 25) rises by itself and is only noted and cleared. */
  int fw_error_state(std::string &what);
  bool _bz_bit25_logged = false;

  /* Receive */
  void post_rx_buf(unsigned index); /* iwx_update_rx_desc */
  void handle_rb(unsigned index);
  void handle_packet(const uint8_t *pkt, size_t len);
  void handle_mpdu(const uint8_t *payload, size_t len, bool rada);
  /* A received MPDU into Block Ack reordering, then up (emit_rx). */
  void rx_in(const RxFrame &f, uint32_t reorder_data, bool amsdu, bool last_subframe);
  void emit_rx(const RxFrame &f);
  void sta_reorder_close_all();
  void handle_tx_resp(const uint8_t *payload, size_t len, unsigned slot);
  int transmit_on(TxRing &ring, const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt,
                  bool kick = true);
  int phy_cmd(unsigned id, uint8_t channel, unsigned width, uint32_t action);
  int binding_for(unsigned id, unsigned phy, const uint32_t *macs, unsigned nmacs, uint8_t channel,
                  uint32_t action);
  int sta_mac_cmd(uint32_t action, bool assoc);
  int enable_queue(TxRing &ring, uint8_t sta_id, uint8_t tid);
  void dump_error_tables();
  bool read_mem(uint32_t addr, uint32_t *out, unsigned dwords);

  Logger _log;
  RxHandler _on_rx;
  bool _scan_configured = false, _scan_running = false, _scan_succeeded = false;
  bool scan_config();
  std::vector<uint8_t> _unpadded; /* handle_mpdu's scratch */
  TxStatusHandler _on_tx;
  /* Frames that arrive while a command waits for its answer: the handler may
   * send commands itself, so they are delivered once no command is waiting
   * (otherwise poll() re-enters and the same receive buffer is handled again). */
  struct DeferredRx {
    std::vector<uint8_t> frame;
    RxFrame meta;
    bool raw = false; /* straight from the card: reordering still to do */
    uint32_t reorder_data = 0;
    bool amsdu = false, last_subframe = false;
  };
  std::deque<DeferredRx> _deferred_rx;
  unsigned _cmd_depth = 0;
  void deliver_deferred();
  RxReorder _reorder;
  Options _opt;
  RegProxy _proxy;
  DmaPool _dma;
  Firmware _fw;

  uint32_t _hw_rev = 0, _hw_rev_raw = 0, _hw_rf_id = 0;
  uint32_t _hw_rev_step = 0; /* Bz: MAC step from CNVI_AUX_MISC_CHIP (start_hw) */
  CardInfo _card;
  bool _integrated = true;
  std::string _fw_name;
  /* AX210 / Bz families */
  DmaBuf _prph_scratch, _prph_info, _iml_buf, _pnvm_buf, _pnvm_desc;
  std::vector<DmaBuf> _pnvm_chunks;
  std::vector<uint8_t> _pnvm_raw;
  uint32_t _sku_id[3] = {};
  bool _pnvm_done = false;
  bool _use_mld = false;
  /* TFD write pointers run modulo 256 (22000) or 65536 (AX210); the slot is
   * the pointer modulo the ring size. */
  unsigned wrap() const { return gen3() ? 65536u : 256u; }
  int _nic_locks = 0;
  bool _open = false, _fw_error = false;

  DmaBuf _ctxt_info, _rx_free, _rx_used, _rx_stat;
  std::vector<DmaBuf> _rx_bufs;
  size_t _rx_buf_bytes = 4096;
  std::vector<DmaBuf> _fw_sections; /* kept until the card is reset */
  unsigned _rx_cur = 0;
  std::vector<unsigned> _rx_inflight; /* buffers being handled, oldest first (re-entrant poll) */
  TxRing _cmdq, _dataq, _staq, _aggq;
  bool _agg_queue_enabled = false;
  bool _agg_active = false;
  bool _agg_he80_active = false;
  bool _he80_fallback = false;
  uint16_t _agg_seq = 0;
  uint32_t _ba_epoch = 0;
  AggStats _agg_stats;
  bool ensure_ba_station(const uint8_t peer[6]);
  void handle_ba_notif(const uint8_t *payload, size_t len);
  std::vector<CmdSlot> _cmd_slots;
  std::vector<DmaBuf> _cmd_big; /* per slot, for commands over 316 bytes */

  /* Notifications the bring-up waits for */
  bool _alive_seen = false, _alive_ok = false, _init_complete = false;
  int _trace_packets = 0; /* packets still to log during the init handshake */
  /* #39/#43 diagnostics: notification IDs nothing handled (each logged once,
   * at most 32), and Block Ack notifications naming no queue of ours. */
  std::vector<uint32_t> _unhandled_notifs;
  unsigned _ba_foreign_logged = 0;
  static constexpr uint32_t kPrphScratchImrDebugEn = 1u << 1; /* IWX_PRPH_SCRATCH_IMR_DEBUG_EN */
  uint32_t _lmac_error_table = 0, _lmac2_error_table = 0, _umac_error_table = 0;

  /* NVM */
  uint8_t _nvm_tx_ant = 0, _nvm_rx_ant = 0;
  bool _lar_enabled = false;
  std::array<uint8_t, 6> _hw_addr{};

  /* Monitor state */
  bool _phy_added = false, _mac_added = false, _binding_added = false,
       _link_added = false, _link_active = false, _sta_added = false, _queue_enabled = false;
  uint8_t _channel = 0;
  unsigned _width = 20;
  std::array<uint8_t, 6> _address{};
  TxStats _tx_stats;
  StaParams _sta{};
  bool _sta_phy_added = false, _sta_mac_added = false, _sta_binding_added = false,
       _sta_link_added = false, _sta_link_active = false,
       _sta_sta_added = false, _sta_queue_enabled = false, _sta_shared_binding = false,
       _sta_assoc = false, _sta_tlc = false, _sta_session_prot = false, _sta_mfp = false;
  uint16_t _sta_aid = 0;
  uint8_t _sta_dtim = 1, _sta_ba_tids = 0, _sta_gtk_mask = 0, _sta_igtk_id = 0;
  uint32_t _sta_ptk_flags = 0;
  uint8_t _sta_baid[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  unsigned _sta_tx_acked = 0, _sta_tx_failed = 0, _sta_tx_fail_logged = 0, _sta_tx_submitted = 0;
  bool _promisc = true;
  bool _ba_station = false;
  uint8_t _ba_peer[6] = {}, _ba_tids = 0, _ba_token[8] = {};
  /* Bz: one firmware station per AWDL Block Ack peer (IWX_BZ_PEER_STA_ID..),
   * so peers taking turns keep their sessions; the least recently used goes
   * when another arrives. */
  struct BzPeer {
    bool used = false;
    uint8_t addr[6] = {};
    uint8_t valid = 0; /* TIDs with a BAID */
    uint8_t token[8] = {};
    uint32_t baid[8] = {};
    uint64_t last_us = 0;
  };
  static constexpr unsigned kBzPeers = 4;
  BzPeer _bz_peers[kBzPeers];
  unsigned _ba_logged = 0, _ba_rm_logged = 0;
  /* The listener link's session protection on Bz. */
  bool _session_on = false;
  uint64_t _session_sent_us = 0;
  unsigned _session_logged = 0;
  /* The station link beside the listener (--mac-type sta): its protection. */
  bool _sta_session_on = false;
  uint64_t _sta_join_us = 0, _sta_session_sent_us = 0;
  uint32_t _chan_flags[256] = {};
  bool _notice_pending = false;
  uint16_t _notice_mcc = 0, _answered_mcc = 0;
  uint8_t _notice_source = 0, _answered_source = 0;
  std::string _current_mcc;
  struct TxHist { uint16_t len; uint8_t hdrlen, fc0, fc1, a1; uint32_t rnf; uint8_t slot; };
  TxHist _tx_hist[8] = {};
  unsigned _tx_hist_at = 0;
  unsigned _tx_fail_logged = 0;
};
