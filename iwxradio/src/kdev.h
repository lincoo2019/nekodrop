// SPDX-License-Identifier: GPL-2.0-only
/*
 * KernelDevice: the listener of nekodrop-wlan.sys (the kernel Wi-Fi driver)
 * behind the radio backend contract (radio_backend.h), so the AWDL engine
 * and its upstream 0.6.0 policies run unchanged while Windows keeps its Wi-Fi
 * on the same card. Talks to \\.\NekodropAwdl (kdriver/nekoioctl.h).
 *
 * It implements only what kCapabilities declares: no Intel A-MPDU queue, no
 * firmware scan, no country notice (the driver answers it itself) and no
 * station (Windows owns it). radio_main never compiles those calls for this
 * backend, so there are no stubs for them here.
 */
#pragma once

#include "plat.h"
#include "radio_backend.h"

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class KernelDevice {
public:
  using Logger = RadioLogger;
  using RxHandler = RadioRxHandler;
  using TxStatusHandler = RadioTxStatusHandler;
  static constexpr RadioCapabilities kCapabilities = radio::kernel_capabilities();

  explicit KernelDevice(Logger log) : _log(std::move(log)) {}
  ~KernelDevice() { stop(); }

  bool open(const RadioOpenOptions &options, std::string &error);
  bool start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]);
  bool set_channel(uint8_t channel, unsigned width_mhz);
  bool set_address(const uint8_t address[6]);
  bool add_rx_ba(const uint8_t peer[6], unsigned tid, uint16_t ssn, uint16_t window, uint8_t token);
  bool remove_rx_ba(unsigned tid);
  bool rx_ba_open(unsigned tid) const { return tid < 8 && (_ba_tids >> tid) & 1; }
  bool set_promiscuous(bool on);
  bool promiscuous() const { return _promisc; }
  bool set_country(const std::string &alpha2);
  uint32_t channel_flags(uint8_t channel) const;
  bool transmit(const uint8_t *frame, size_t len, const TxRate &rate) { return transmit_slot(frame, len, rate) >= 0; }
  /* The driver rings the doorbell itself: `kick` is for the user-mode path
   * (supports_batched_kick is off, so radio_main never calls kick_data here). */
  int transmit_slot(const uint8_t *frame, size_t len, const TxRate &rate, bool kick = true);
  void set_tx_status_handler(TxStatusHandler handler) { _on_tx = std::move(handler); }
  void set_rx_handler(RxHandler handler) { _on_rx = std::move(handler); }
  int poll();
  void check_health();
  bool healthy() const { return _handle != INVALID_HANDLE_VALUE && _healthy; }
  const TxStats &tx_stats() const { return _stats; }
  uint8_t channel() const { return _channel; }
  unsigned width_mhz() const { return _width; }
  const std::array<uint8_t, 6> &hw_address() const { return _hw; }
  void stop();

private:
  bool ioctl(DWORD code, const void *in, DWORD inLength, void *out, DWORD outLength, DWORD *got = nullptr) const;
  void reader();

  Logger _log;
  RxHandler _on_rx;
  TxStatusHandler _on_tx;
  HANDLE _handle = INVALID_HANDLE_VALUE;
  std::thread _reader;
  std::atomic<bool> _stop{false};
  std::mutex _lock;
  std::deque<std::vector<uint8_t>> _batches; /* IOCTL_NEKO_READ results */
  std::array<uint8_t, 6> _hw{};
  uint8_t _channel = 0;
  unsigned _width = 20;
  bool _promisc = true, _healthy = true;
  uint8_t _ba_tids = 0;
  uint32_t _flags[4] = {};
  TxStats _stats;
};
