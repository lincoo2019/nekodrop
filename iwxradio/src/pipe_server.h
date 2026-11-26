// SPDX-License-Identifier: GPL-2.0-only
/* Server end of the radio link: one named pipe instance, one client at a time.
 * Overlapped, because one thread reads while another writes. */
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "radio_link.h"
#include "radio_shm.h"

class PipeServer {
public:
  /* `stopping` is polled while an operation waits, so a blocked read or
   * connect ends soon after the daemon is asked to stop. */
  PipeServer(const std::string &name, std::function<bool()> stopping,
             DWORD buffer_bytes = kBufferBytes)
      : _stopping(std::move(stopping)) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) {
      sa.lpSecurityDescriptor = sd;
      _handle = CreateNamedPipeA(name.c_str(),
                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                     FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                     PIPE_REJECT_REMOTE_CLIENTS,
                                 1, buffer_bytes, buffer_bytes, 0, &sa);
      LocalFree(sd);
    }
    _read_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    _write_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  }
  ~PipeServer() {
    if (_handle != INVALID_HANDLE_VALUE) CloseHandle(_handle);
    CloseHandle(_read_event);
    CloseHandle(_write_event);
  }
  PipeServer(const PipeServer &) = delete;
  PipeServer &operator=(const PipeServer &) = delete;

  bool valid() const { return _handle != INVALID_HANDLE_VALUE; }
  bool connected() const { return _connected; }
  bool shm_active() const { return _shm.active(); }
  uint8_t shm_cap_flag() const { return _shm.cap_flag(); }
  bool pop_shm_tx(std::vector<uint8_t> &out) { return _connected && _shm.pop_tx(out); }

  /* A client that connected and already left still counts as connected here:
   * its departure shows up as a failed first read. */
  bool wait_for_client() {
    OVERLAPPED io{};
    io.hEvent = _read_event;
    if (!ConnectNamedPipe(_handle, &io)) {
      const DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        DWORD ignored = 0;
        if (!finish(io, ignored)) return false;
      } else if (error != ERROR_PIPE_CONNECTED && error != ERROR_NO_DATA) {
        return false;
      }
    }
    _shm.reset();
    _connected = true;
    return true;
  }

  /* Frees the single pipe instance for the next client. */
  void drop_client() {
    _connected = false;
    {
      std::lock_guard<std::mutex> lock(_write_mutex);
      _rx_batch.clear();
    }
    _taken = _filled = 0;
    DisconnectNamedPipe(_handle);
  }

  /* Whether read_exact would hand over `length` bytes without waiting for
   * the client: they were read along with what came before them. */
  bool buffered(DWORD length) const { return _filled - _taken >= length; }

  bool read_exact(void *data, DWORD length) {
    auto *bytes = static_cast<uint8_t *>(data);
    while (length > 0) {
      if (_taken == _filled && !refill()) return false;
      const DWORD step = (std::min)(length, _filled - _taken);
      std::memcpy(bytes, _in.data() + _taken, step);
      _taken += step;
      bytes += step;
      length -= step;
    }
    return true;
  }

  bool send(radio_link_type type, std::span<const uint8_t> payload) {
    if (!_connected) return false;
    std::lock_guard<std::mutex> lock(_write_mutex);
    if (!_rx_batch.empty()) {
      const bool ok = write_all(_rx_batch.data(), static_cast<DWORD>(_rx_batch.size()));
      _rx_batch.clear();
      if (!ok) return false;
    }
    const radio_link_header header{static_cast<uint16_t>(type), 0,
                                   static_cast<uint32_t>(payload.size())};
    /* One write for the two: a frame is then one system call, not two. */
    _out.resize(sizeof(header) + payload.size());
    std::memcpy(_out.data(), &header, sizeof(header));
    if (!payload.empty())
      std::memcpy(_out.data() + sizeof(header), payload.data(), payload.size());
    return write_all(_out.data(), static_cast<DWORD>(_out.size()));
  }

  /* Appends a framed packet into the RX data plane: zero-copy shared-memory
   * ring when the client enabled SHM, otherwise batched over the Named Pipe. */
  bool queue_send(radio_link_type type, std::span<const uint8_t> payload) {
    if (!_connected) return false;
    if (type == RADIO_LINK_RX && _shm.client_ready()) {
      (void)_shm.push_rx(payload.data(), static_cast<uint32_t>(payload.size()), false);
      return true;
    }
    std::lock_guard<std::mutex> lock(_write_mutex);
    const radio_link_header header{static_cast<uint16_t>(type), 0,
                                   static_cast<uint32_t>(payload.size())};
    const size_t at = _rx_batch.size();
    _rx_batch.resize(at + sizeof(header) + payload.size());
    std::memcpy(_rx_batch.data() + at, &header, sizeof(header));
    if (!payload.empty())
      std::memcpy(_rx_batch.data() + at + sizeof(header), payload.data(), payload.size());
    if (_rx_batch.size() >= kBatchFlushBytes) {
      const bool ok = write_all(_rx_batch.data(), static_cast<DWORD>(_rx_batch.size()));
      _rx_batch.clear();
      return ok;
    }
    return true;
  }

  bool flush_send() {
    if (!_connected) return false;
    if (_shm.client_ready()) {
      _shm.wake_rx();
      return true;
    }
    std::lock_guard<std::mutex> lock(_write_mutex);
    if (_rx_batch.empty()) return true;
    const bool ok = write_all(_rx_batch.data(), static_cast<DWORD>(_rx_batch.size()));
    _rx_batch.clear();
    return ok;
  }

  /* Writes a pre-framed batch of radio_link messages; routes RADIO_LINK_RX
   * through shared memory when active so callers get zero-pipe RX for free. */
  bool send_raw(std::span<const uint8_t> bytes) {
    if (!_connected) return false;
    if (bytes.empty()) return true;
    if (_shm.client_ready()) {
      size_t off = 0;
      bool pushed = false;
      while (off + sizeof(radio_link_header) <= bytes.size()) {
        radio_link_header hdr{};
        std::memcpy(&hdr, bytes.data() + off, sizeof(hdr));
        off += sizeof(hdr);
        if (off + hdr.length > bytes.size()) break;
        if (hdr.type == RADIO_LINK_RX) {
          (void)_shm.push_rx(bytes.data() + off, hdr.length, false);
          pushed = true;
        }
        off += hdr.length;
      }
      if (pushed) _shm.wake_rx();
      return true;
    }
    std::lock_guard<std::mutex> lock(_write_mutex);
    if (!_rx_batch.empty()) {
      const bool ok = write_all(_rx_batch.data(), static_cast<DWORD>(_rx_batch.size()));
      _rx_batch.clear();
      if (!ok) return false;
    }
    return write_all(bytes.data(), static_cast<DWORD>(bytes.size()));
  }

private:
  static constexpr DWORD kBufferBytes = 1 << 18; /* 256 KiB pipe buffer */
  static constexpr size_t kBatchFlushBytes = 32768;
  static constexpr DWORD kStopPollMs = 100;

  bool disconnect() {
    _connected = false;
    return false;
  }

  /* Reads all the client has written so far, waiting for the first byte. */
  bool refill() {
    OVERLAPPED io{};
    io.hEvent = _read_event;
    DWORD got = 0;
    if (!ReadFile(_handle, _in.data(), kBufferBytes, &got, &io) &&
        (GetLastError() != ERROR_IO_PENDING || !finish(io, got)))
      return disconnect();
    if (got == 0) return disconnect();
    _taken = 0;
    _filled = got;
    return true;
  }

  bool finish(OVERLAPPED &io, DWORD &transferred) {
    while (WaitForSingleObject(io.hEvent, kStopPollMs) == WAIT_TIMEOUT) {
      if (_stopping()) {
        CancelIoEx(_handle, &io);
        GetOverlappedResult(_handle, &io, &transferred, TRUE);
        return false;
      }
    }
    return GetOverlappedResult(_handle, &io, &transferred, FALSE) != 0;
  }

  bool write_all(const void *data, DWORD length) {
    auto *bytes = static_cast<const uint8_t *>(data);
    while (length > 0) {
      OVERLAPPED io{};
      io.hEvent = _write_event;
      DWORD put = 0;
      if (!WriteFile(_handle, bytes, length, &put, &io) &&
          (GetLastError() != ERROR_IO_PENDING || !finish(io, put)))
        return disconnect();
      bytes += put;
      length -= put;
    }
    return true;
  }

  std::function<bool()> _stopping;
  HANDLE _handle = INVALID_HANDLE_VALUE;
  HANDLE _read_event = nullptr;
  HANDLE _write_event = nullptr;
  std::atomic<bool> _connected{false};
  std::mutex _write_mutex;
  std::vector<uint8_t> _out;      /* under _write_mutex */
  std::vector<uint8_t> _rx_batch; /* under _write_mutex */
  std::vector<uint8_t> _in = std::vector<uint8_t>(kBufferBytes);
  DWORD _taken = 0, _filled = 0;
  RadioShmServer _shm;
};
