// SPDX-License-Identifier: GPL-2.0-only
#include "kdev.h"

#include <winioctl.h>

#include <cstring>

#include "../../kdriver/nekoioctl.h"

/* The handle is overlapped: a synchronous handle would serialize every
 * request behind the reader's pending IOCTL_NEKO_READ. */
bool KernelDevice::ioctl(DWORD code, const void *in, DWORD inLength, void *out, DWORD outLength, DWORD *got) const {
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return false;
  DWORD n = 0;
  BOOL ok = DeviceIoControl(_handle, code, const_cast<void *>(in), inLength, out, outLength, nullptr, &ov);
  if (ok || GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(_handle, &ov, &n, TRUE);
  CloseHandle(ov.hEvent);
  if (got) *got = n;
  return ok != FALSE;
}

bool KernelDevice::open(const RadioOpenOptions &options, std::string &error) {
  _handle = CreateFileA(NEKO_AWDL_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, nullptr);
  if (_handle == INVALID_HANDLE_VALUE) {
    error = "cannot open the nekodrop kernel driver (\\\\.\\NekodropAwdl, error " + std::to_string(GetLastError()) +
            "); is nekodrop-wlan.sys bound to the card?";
    return false;
  }
  if (options.country.size() == 2 && options.country != "ZZ") set_country(options.country);
  check_health();
  _log("kernel driver: AWDL through nekodrop-wlan.sys, Windows keeps the station");
  return true;
}

bool KernelDevice::start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]) {
  NEKO_ATTACH a{};
  a.channel = channel;
  a.width = static_cast<unsigned char>(width_mhz);
  std::memcpy(a.address, address, 6);
  NEKO_INFO info{};
  if (!ioctl(IOCTL_NEKO_ATTACH, &a, sizeof(a), &info, sizeof(info))) return false;
  std::memcpy(_hw.data(), info.hw_address, 6);
  _channel = info.channel;
  _width = info.width;
  _promisc = true;
  _stop = false;
  _reader = std::thread([this] { reader(); });
  _log("listening on channel " + std::to_string(_channel) + " at " + std::to_string(_width) + " MHz");
  return true;
}

/* A second handle-less loop would race the driver; one thread keeps one read
 * pending at a time and queues each batch for poll(). */
void KernelDevice::reader() {
  std::vector<uint8_t> buf(256 * 1024);
  while (!_stop) {
    DWORD got = 0;
    if (!ioctl(IOCTL_NEKO_READ, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
      if (_stop) break;
      _healthy = false;
      Sleep(100);
      continue;
    }
    if (!got) continue;
    std::lock_guard<std::mutex> l(_lock);
    if (_batches.size() < 256) _batches.emplace_back(buf.begin(), buf.begin() + got);
  }
}

int KernelDevice::poll() {
  std::deque<std::vector<uint8_t>> batches;
  {
    std::lock_guard<std::mutex> l(_lock);
    batches.swap(_batches);
  }
  int handled = 0;
  for (const auto &b : batches) {
    size_t off = 0;
    while (off + sizeof(NEKO_EVENT) <= b.size()) {
      NEKO_EVENT e;
      std::memcpy(&e, b.data() + off, sizeof(e));
      const uint8_t *data = b.data() + off + sizeof(e);
      if (off + sizeof(e) + e.length > b.size()) break;
      if (e.type == NEKO_EVENT_RX && _on_rx) {
        RxFrame f{data, e.length, e.dbm, e.channel, e.rate, e.status, false};
        _on_rx(f);
      } else if (e.type == NEKO_EVENT_TX_STATUS && _on_tx) {
        _on_tx(e.slot, e.status != 0);
      }
      off += (sizeof(e) + e.length + 3) & ~size_t(3);
      handled++;
    }
  }
  return handled;
}

bool KernelDevice::set_channel(uint8_t channel, unsigned width_mhz) {
  const uint8_t in[2] = {channel, static_cast<uint8_t>(width_mhz)};
  if (!ioctl(IOCTL_NEKO_SET_CHANNEL, in, 2, nullptr, 0)) return false;
  _channel = channel;
  _width = width_mhz;
  return true;
}

bool KernelDevice::set_address(const uint8_t address[6]) { return ioctl(IOCTL_NEKO_SET_ADDRESS, address, 6, nullptr, 0); }

bool KernelDevice::add_rx_ba(const uint8_t peer[6], unsigned tid, uint16_t ssn, uint16_t window, uint8_t token) {
  NEKO_BA ba{};
  std::memcpy(ba.peer, peer, 6);
  ba.tid = static_cast<unsigned char>(tid);
  ba.token = token;
  ba.ssn = ssn;
  ba.window = window;
  uint8_t ok = 0;
  if (!ioctl(IOCTL_NEKO_ADD_RX_BA, &ba, sizeof(ba), &ok, 1) || !ok) return false;
  _ba_tids |= static_cast<uint8_t>(1u << tid);
  return true;
}

bool KernelDevice::remove_rx_ba(unsigned tid) {
  const uint8_t t = static_cast<uint8_t>(tid);
  _ba_tids &= static_cast<uint8_t>(~(1u << tid));
  return ioctl(IOCTL_NEKO_REMOVE_RX_BA, &t, 1, nullptr, 0);
}

bool KernelDevice::set_promiscuous(bool on) {
  const uint8_t v = on;
  if (!ioctl(IOCTL_NEKO_SET_PROMISC, &v, 1, nullptr, 0)) return false;
  _promisc = on;
  return true;
}

bool KernelDevice::set_country(const std::string &alpha2) {
  if (alpha2.size() != 2) return false;
  uint8_t taken = 0;
  const bool ok = ioctl(IOCTL_NEKO_SET_COUNTRY, alpha2.data(), 2, &taken, 1) && taken;
  check_health(); /* refresh the channel flags */
  return ok;
}

uint32_t KernelDevice::channel_flags(uint8_t channel) const {
  switch (channel) {
  case 6: return _flags[0];
  case 36: return _flags[1];
  case 44: return _flags[2];
  case 149: return _flags[3];
  default: return 0;
  }
}

int KernelDevice::transmit_slot(const uint8_t *frame, size_t len, const TxRate &rate, bool /*kick*/) {
  if (len > 4000) return -1;
  std::vector<uint8_t> in(sizeof(NEKO_TX) + len);
  NEKO_TX t{};
  t.kind = static_cast<unsigned char>(rate.kind == TxRate::Ht ? 1 : rate.kind == TxRate::Vht ? 2
                                      : rate.kind == TxRate::Auto ? 3 : 0);
  t.mcs = static_cast<unsigned char>(rate.mcs);
  t.nss = static_cast<unsigned char>(rate.nss);
  t.wide80 = rate.wide80;
  t.mbps_x2 = static_cast<unsigned short>(rate.mbps_x2);
  t.length = static_cast<unsigned short>(len);
  std::memcpy(in.data(), &t, sizeof(t));
  std::memcpy(in.data() + sizeof(t), frame, len);
  LONG slot = -1;
  if (!ioctl(IOCTL_NEKO_TX, in.data(), static_cast<DWORD>(in.size()), &slot, sizeof(slot))) return -1;
  if (slot >= 0) _stats.submitted++;
  else _stats.dropped_full++;
  return slot;
}

void KernelDevice::check_health() {
  NEKO_STATUS s{};
  if (!ioctl(IOCTL_NEKO_QUERY, nullptr, 0, &s, sizeof(s))) {
    _healthy = false;
    return;
  }
  _healthy = s.healthy != 0;
  _ba_tids = s.ba_tids;
  for (int i = 0; i < 4; i++) _flags[i] = s.channel_flags[i];
  _stats.completed = s.completed;
  _stats.acked = s.acked;
  _stats.failed = s.failed;
  _stats.retries = s.retries;
}

void KernelDevice::stop() {
  if (_handle == INVALID_HANDLE_VALUE) return;
  _stop = true;
  ioctl(IOCTL_NEKO_DETACH, nullptr, 0, nullptr, 0);
  CancelIoEx(_handle, nullptr); /* the reader's pending read */
  if (_reader.joinable()) _reader.join();
  CloseHandle(_handle);
  _handle = INVALID_HANDLE_VALUE;
}
