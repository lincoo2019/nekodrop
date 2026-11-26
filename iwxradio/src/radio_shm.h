// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lock-free single-producer / single-consumer shared-memory data plane between
 * the radio daemon (iwxradio / mtkradio) and awdld.
 *
 * Control messages (HELLO, SET_CHANNEL, SET_BSSID_FILTER, SET_ADDRESS) and
 * disconnect detection remain on the existing Named Pipe; high-rate 802.11
 * RX/TX frames flow through two 4 MiB user-space circular rings with Dekker
 * wake flags, avoiding per-frame ReadFile/WriteFile kernel transitions.
 */
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "radio_link.h"

#define RADIO_SHM_MAGIC 0x4E4B5348u /* "NKSH" */
#define RADIO_SHM_VERSION 1u
#define RADIO_SHM_RING_SIZE (4u * 1024u * 1024u) /* 4 MiB power-of-2 */
#define RADIO_SHM_RING_MASK (RADIO_SHM_RING_SIZE - 1u)

#define RADIO_LINK_CAP_SHM_GLOBAL 1u /* radio_link_hello.reserved[0] bit 0 */
#define RADIO_LINK_CAP_SHM_LOCAL 2u  /* radio_link_hello.reserved[0] bit 1 */

#define RADIO_SHM_MAP_GLOBAL "Global\\nekodrop-radio-shm-v1"
#define RADIO_SHM_RX_GLOBAL "Global\\nekodrop-radio-shm-rx-v1"
#define RADIO_SHM_TX_GLOBAL "Global\\nekodrop-radio-shm-tx-v1"

#define RADIO_SHM_MAP_LOCAL "Local\\nekodrop-radio-shm-v1"
#define RADIO_SHM_RX_LOCAL "Local\\nekodrop-radio-shm-rx-v1"
#define RADIO_SHM_TX_LOCAL "Local\\nekodrop-radio-shm-tx-v1"

struct RadioShmRing {
  volatile LONG write_pos;
  volatile LONG pad0[15];
  volatile LONG read_pos;
  volatile LONG waiting;
  volatile LONG pad1[14];
  uint8_t data[RADIO_SHM_RING_SIZE];
};

struct RadioShmLayout {
  uint32_t magic;
  uint32_t version;
  volatile LONG epoch;
  volatile LONG client_ready; /* set to 1 by awdld once mapping + events are open */
  uint32_t reserved[12];
  RadioShmRing rx; /* radio -> awdld */
  RadioShmRing tx; /* awdld -> radio */
};
static_assert(sizeof(RadioShmRing) == 128u + RADIO_SHM_RING_SIZE, "RadioShmRing ABI");
static_assert(sizeof(RadioShmLayout) == 64u + 2u * sizeof(RadioShmRing), "RadioShmLayout ABI");

class RadioShmServer {
public:
  RadioShmServer() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) {
      sa.lpSecurityDescriptor = sd;
    }
    SECURITY_ATTRIBUTES *psa = sd ? &sa : nullptr;

    /* Try Global\ first with SYSTEM/Admin DACL (used by NekoDropService);
     * fall back to Local\ with default DACL when running unprivileged in tests. */
    if (!try_init(RADIO_SHM_MAP_GLOBAL, RADIO_SHM_RX_GLOBAL, RADIO_SHM_TX_GLOBAL, psa,
                  RADIO_LINK_CAP_SHM_GLOBAL)) {
      try_init(RADIO_SHM_MAP_LOCAL, RADIO_SHM_RX_LOCAL, RADIO_SHM_TX_LOCAL, nullptr,
               RADIO_LINK_CAP_SHM_LOCAL);
    }
    if (sd) LocalFree(sd);
  }

  ~RadioShmServer() { cleanup(); }

  RadioShmServer(const RadioShmServer &) = delete;
  RadioShmServer &operator=(const RadioShmServer &) = delete;

  bool active() const { return _layout != nullptr; }
  bool client_ready() const {
    return _layout != nullptr &&
           InterlockedCompareExchange(&_layout->client_ready, 0, 0) != 0;
  }
  uint8_t cap_flag() const { return _cap_flag; }
  HANDLE tx_event() const { return _tx_event; }

  void reset() {
    if (!_layout) return;
    InterlockedExchange(&_layout->client_ready, 0);
    InterlockedExchange(&_layout->rx.write_pos, 0);
    InterlockedExchange(&_layout->rx.read_pos, 0);
    InterlockedExchange(&_layout->rx.waiting, 0);
    InterlockedExchange(&_layout->tx.write_pos, 0);
    InterlockedExchange(&_layout->tx.read_pos, 0);
    InterlockedExchange(&_layout->tx.waiting, 0);
    InterlockedIncrement(&_layout->epoch);
    MemoryBarrier();
    if (_rx_event) ResetEvent(_rx_event);
    if (_tx_event) ResetEvent(_tx_event);
  }

  /* Producer (radio -> awdld): writes one RX payload into the lock-free RX ring. */
  bool push_rx(const uint8_t *payload, uint32_t len, bool signal_now = false) {
    if (!_layout || len == 0 || len > RADIO_LINK_MAX_PAYLOAD) return false;
    RadioShmRing &ring = _layout->rx;
    const uint32_t total = (4u + len + 3u) & ~3u;
    const uint32_t w = static_cast<uint32_t>(ring.write_pos);
    const uint32_t r = static_cast<uint32_t>(InterlockedCompareExchange(&ring.read_pos, 0, 0));
    const uint32_t used = w - r;
    if (RADIO_SHM_RING_SIZE - used < total) return false;

    write_bytes(ring.data, w, reinterpret_cast<const uint8_t *>(&len), 4u);
    write_bytes(ring.data, w + 4u, payload, len);
    MemoryBarrier();
    InterlockedExchange(&ring.write_pos, static_cast<LONG>(w + total));
    if (signal_now || used == 0) wake_rx();
    return true;
  }

  void wake_rx() {
    if (!_layout || !_rx_event) return;
    MemoryBarrier();
    if (InterlockedCompareExchange(&_layout->rx.waiting, 0, 0) != 0) {
      InterlockedExchange(&_layout->rx.waiting, 0);
      SetEvent(_rx_event);
    }
  }

  /* Consumer (awdld -> radio): pops one TX payload from the lock-free TX ring. */
  bool pop_tx(std::vector<uint8_t> &out) {
    if (!_layout) return false;
    RadioShmRing &ring = _layout->tx;
    const uint32_t r = static_cast<uint32_t>(ring.read_pos);
    const uint32_t w = static_cast<uint32_t>(InterlockedCompareExchange(&ring.write_pos, 0, 0));
    if (w == r) return false;
    MemoryBarrier();

    uint32_t len = 0;
    read_bytes(ring.data, r, reinterpret_cast<uint8_t *>(&len), 4u);
    const uint32_t total = (4u + len + 3u) & ~3u;
    if (len == 0 || len > RADIO_LINK_MAX_PAYLOAD || (w - r) < total) {
      /* Corrupt/truncated record: resync safely to write_pos. */
      InterlockedExchange(&ring.read_pos, static_cast<LONG>(w));
      return false;
    }
    out.resize(len);
    read_bytes(ring.data, r + 4u, out.data(), len);
    MemoryBarrier();
    InterlockedExchange(&ring.read_pos, static_cast<LONG>(r + total));
    return true;
  }

private:
  bool try_init(const char *map_name, const char *rx_name, const char *tx_name,
                SECURITY_ATTRIBUTES *psa, uint8_t flag) {
    const DWORD sz = static_cast<DWORD>(sizeof(RadioShmLayout));
    _mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE, 0, sz, map_name);
    if (!_mapping) return false;
    _layout = static_cast<RadioShmLayout *>(MapViewOfFile(_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sz));
    if (!_layout) {
      cleanup();
      return false;
    }
    /* Manual-reset RX event: stays signaled while consumer drains a full burst
     * (zero per-frame syscalls) and is only reset by the consumer when w == r. */
    _rx_event = CreateEventA(psa, TRUE, FALSE, rx_name);
    _tx_event = CreateEventA(psa, FALSE, FALSE, tx_name);
    if (!_rx_event || !_tx_event) {
      cleanup();
      return false;
    }
    std::memset(_layout, 0, sizeof(RadioShmLayout));
    _layout->magic = RADIO_SHM_MAGIC;
    _layout->version = RADIO_SHM_VERSION;
    MemoryBarrier();
    _cap_flag = flag;
    return true;
  }

  void cleanup() {
    if (_layout) {
      UnmapViewOfFile(_layout);
      _layout = nullptr;
    }
    if (_mapping) {
      CloseHandle(_mapping);
      _mapping = nullptr;
    }
    if (_rx_event) {
      CloseHandle(_rx_event);
      _rx_event = nullptr;
    }
    if (_tx_event) {
      CloseHandle(_tx_event);
      _tx_event = nullptr;
    }
    _cap_flag = 0;
  }

  static void write_bytes(uint8_t *ring, uint32_t pos, const uint8_t *src, uint32_t n) {
    const uint32_t idx = pos & RADIO_SHM_RING_MASK;
    const uint32_t first = (std::min)(n, RADIO_SHM_RING_SIZE - idx);
    std::memcpy(ring + idx, src, first);
    if (n > first) std::memcpy(ring, src + first, n - first);
  }

  static void read_bytes(const uint8_t *ring, uint32_t pos, uint8_t *dst, uint32_t n) {
    const uint32_t idx = pos & RADIO_SHM_RING_MASK;
    const uint32_t first = (std::min)(n, RADIO_SHM_RING_SIZE - idx);
    std::memcpy(dst, ring + idx, first);
    if (n > first) std::memcpy(dst + first, ring, n - first);
  }

  HANDLE _mapping = nullptr;
  HANDLE _rx_event = nullptr;
  HANDLE _tx_event = nullptr;
  RadioShmLayout *_layout = nullptr;
  uint8_t _cap_flag = 0;
};
