// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host-side Block Ack receive reordering, after Linux iwlwifi's
 * iwl_mvm_reorder()/iwl_mvm_release_frames(): the firmware acknowledges an
 * aggregate and hands over its MPDUs as they arrive, each with its BAID,
 * sequence number and the "NSSN" up to which everything may go up; frames
 * past a hole wait here until a later NSSN, a frame-release notification or
 * a 100 ms timeout lets them out, in sequence order.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "radio_backend.h"

class RxReorder {
public:
  /* Called for each frame let out, in order; `f.data` points at `f.len` bytes. */
  using Deliver = std::function<void(const RxFrame &f)>;

  static constexpr unsigned kMaxBaid = 128;
  static constexpr uint64_t kTimeoutUs = 100000; /* RX_REORDER_BUF_TIMEOUT_MQ */

  explicit RxReorder(Deliver deliver) : _deliver(std::move(deliver)) {}

  /* A session the firmware gave `baid`: frames from `ssn` on, `window` wide. */
  void open(unsigned baid, uint16_t ssn, uint16_t window);
  /* Lets out whatever is held, in order, and forgets the session. */
  void close(unsigned baid);
  /* Drops every session and held frame without delivering (card reset). */
  void clear() {
    for (auto &b : _buf) b.reset();
    _pending.clear();
  }
  bool is_open(unsigned baid) const { return baid < kMaxBaid && _buf[baid] != nullptr; }

  /* One received MPDU with its descriptor's reorder data. False: no session
   * for its BAID, the caller delivers it. True: delivered (now or after the
   * frames before it), held, or dropped as old. */
  bool frame(const RxFrame &f, uint32_t reorder_data, bool amsdu, bool last_subframe, uint64_t now_us);
  /* FRAME_RELEASE / BAR_FRAME_RELEASE: everything before `nssn` goes up. */
  void release(unsigned baid, uint16_t nssn);
  /* Frames held past the timeout go up (with everything before them). */
  void expire(uint64_t now_us);

  struct Stats {
    uint64_t held = 0, in_order = 0, released = 0, timed_out = 0, dropped_old = 0, notifications = 0;
  };
  const Stats &stats() const { return _stats; }

private:
  struct Held {
    std::vector<uint8_t> bytes;
    RxFrame meta;
  };
  struct Entry {
    std::vector<Held> frames;
    uint64_t since = 0;
  };
  struct Buffer {
    uint16_t head_sn = 0, size = 64;
    bool valid = false;
    unsigned stored = 0;
    std::vector<Entry> entries;
  };

  /* The deliver callback may send firmware commands, which poll the card
   * and come back here (a frame, a release notification): those calls are
   * queued and run once the outer one is done, so frames stay in order. */
  struct Op {
    enum Kind { Frame, Release, Expire, Open, Close } kind;
    Held held;
    uint32_t reorder_data = 0;
    bool amsdu = false, last = false;
    uint64_t now_us = 0;
    unsigned baid = 0;
    uint16_t nssn = 0; /* Open: the start sequence */
    uint16_t window = 0;
  };
  void open_now(unsigned baid, uint16_t ssn, uint16_t window);
  void close_now(unsigned baid);

  bool frame_now(const RxFrame &f, uint32_t reorder_data, bool amsdu, bool last_subframe, uint64_t now_us);
  void release_now(unsigned baid, uint16_t nssn);
  void expire_now(uint64_t now_us);
  void drain();
  void release_to(Buffer &b, uint16_t nssn);
  void flush_all(Buffer &b);
  void hold(Buffer &b, uint16_t sn, const RxFrame &f, uint64_t now_us);
  void emit(const Held &h);

  Deliver _deliver;
  std::unique_ptr<Buffer> _buf[kMaxBaid];
  Stats _stats;
  bool _busy = false;
  std::vector<Op> _pending;
};
