// SPDX-License-Identifier: GPL-2.0-only
#include "rx_reorder.h"

namespace {

constexpr uint32_t kNssnMask = 0x00000fff, kSnMask = 0x00fff000, kBaidMask = 0x7f000000, kOldSn = 0x80000000;
constexpr unsigned kSnShift = 12, kBaidShift = 24, kInvalidBaid = 0x7f;

uint16_t sn_add(uint16_t a, unsigned b) { return static_cast<uint16_t>((a + b) & 0xfff); }
/* ieee80211_sn_less */
bool sn_less(uint16_t a, uint16_t b) { return ((a - b) & 0xfff) > 0x800; }
/* iwl_mvm_is_sn_less: before `b`, but not by a whole window or more */
bool sn_less_in(uint16_t a, uint16_t b, uint16_t size) {
  return sn_less(a, b) && !sn_less(a, static_cast<uint16_t>((b - size) & 0xfff));
}

} // namespace

void RxReorder::open(unsigned baid, uint16_t ssn, uint16_t window) {
  if (baid >= kMaxBaid) return;
  if (_busy) {
    Op op{Op::Open, {}};
    op.baid = baid;
    op.nssn = ssn;
    op.window = window;
    _pending.push_back(std::move(op));
    return;
  }
  _busy = true;
  open_now(baid, ssn, window);
  _busy = false;
  drain();
}

void RxReorder::close(unsigned baid) {
  if (baid >= kMaxBaid) return;
  if (_busy) {
    Op op{Op::Close, {}};
    op.baid = baid;
    _pending.push_back(std::move(op));
    return;
  }
  _busy = true;
  close_now(baid);
  _busy = false;
  drain();
}

void RxReorder::open_now(unsigned baid, uint16_t ssn, uint16_t window) {
  close_now(baid);
  auto b = std::make_unique<Buffer>();
  b->head_sn = ssn & 0xfff;
  b->size = window ? (window > 1024 ? 1024 : window) : 64;
  b->entries.resize(b->size);
  _buf[baid] = std::move(b);
}

void RxReorder::close_now(unsigned baid) {
  if (!_buf[baid]) return;
  std::unique_ptr<Buffer> b = std::move(_buf[baid]);
  flush_all(*b);
}

bool RxReorder::frame(const RxFrame &f, uint32_t reorder_data, bool amsdu, bool last_subframe, uint64_t now_us) {
  const unsigned baid = (reorder_data & kBaidMask) >> kBaidShift;
  if (baid == kInvalidBaid || !_buf[baid]) return false;
  if (_busy) {
    Op op{Op::Frame, {std::vector<uint8_t>(f.data, f.data + f.len), f}};
    op.reorder_data = reorder_data;
    op.amsdu = amsdu;
    op.last = last_subframe;
    op.now_us = now_us;
    _pending.push_back(std::move(op));
    return true;
  }
  _busy = true;
  const bool consumed = frame_now(f, reorder_data, amsdu, last_subframe, now_us);
  _busy = false;
  if (!consumed) _deliver(f);
  drain();
  return true;
}

void RxReorder::release(unsigned baid, uint16_t nssn) {
  if (_busy) {
    Op op{Op::Release, {}};
    op.baid = baid;
    op.nssn = nssn;
    _pending.push_back(std::move(op));
    return;
  }
  _busy = true;
  release_now(baid, nssn);
  _busy = false;
  drain();
}

void RxReorder::expire(uint64_t now_us) {
  if (_busy) {
    Op op{Op::Expire, {}};
    op.now_us = now_us;
    _pending.push_back(std::move(op));
    return;
  }
  _busy = true;
  expire_now(now_us);
  _busy = false;
  drain();
}

void RxReorder::drain() {
  while (!_busy && !_pending.empty()) {
    Op op = std::move(_pending.front());
    _pending.erase(_pending.begin());
    _busy = true;
    if (op.kind == Op::Frame) {
      op.held.meta.data = op.held.bytes.data();
      if (!frame_now(op.held.meta, op.reorder_data, op.amsdu, op.last, op.now_us)) emit(op.held);
    } else if (op.kind == Op::Release) {
      release_now(op.baid, op.nssn);
    } else if (op.kind == Op::Open) {
      open_now(op.baid, op.nssn, op.window);
    } else if (op.kind == Op::Close) {
      close_now(op.baid);
    } else {
      expire_now(op.now_us);
    }
    _busy = false;
  }
}

/* iwl_mvm_reorder(), one RX queue */
bool RxReorder::frame_now(const RxFrame &f, uint32_t reorder_data, bool amsdu, bool last_subframe,
                          uint64_t now_us) {
  const unsigned baid = (reorder_data & kBaidMask) >> kBaidShift;
  if (baid == kInvalidBaid || !_buf[baid] || f.len < 24) return false;
  Buffer &b = *_buf[baid];
  const uint8_t fc0 = f.data[0];
  const bool bar = fc0 == 0x84;
  const bool qos_data = (fc0 & 0x0c) == 0x08 && (fc0 & 0x80) && !(fc0 & 0x40);
  if (!bar && (!qos_data || (f.data[4] & 1))) return false;
  const uint16_t nssn = reorder_data & kNssnMask;
  const uint16_t sn = static_cast<uint16_t>((reorder_data & kSnMask) >> kSnShift);
  if (bar) {
    _stats.notifications++;
    release_to(b, nssn);
    return true;
  }
  if (!b.valid) {
    if (reorder_data & kOldSn) return false;
    b.valid = true;
  }
  if (reorder_data & kOldSn) {
    _stats.dropped_old++;
    return true;
  }
  const bool sets_head = !amsdu || last_subframe;
  /* Due now: before the NSSN with nothing waiting, or exactly the head. */
  if (!b.stored && sn_less(sn, nssn)) {
    if (sets_head) b.head_sn = nssn;
    _stats.in_order++;
    return false;
  }
  if (!b.stored && sn == b.head_sn) {
    if (sets_head) b.head_sn = sn_add(b.head_sn, 1);
    _stats.in_order++;
    return false;
  }
  hold(b, sn, f, now_us);
  /* The NSSN of an A-MSDU's earlier subframes cannot be trusted. */
  if (sets_head) release_to(b, nssn);
  return true;
}

void RxReorder::release_now(unsigned baid, uint16_t nssn) {
  if (baid >= kMaxBaid || !_buf[baid]) return;
  _stats.notifications++;
  release_to(*_buf[baid], nssn & 0xfff);
}

/* iwl_mvm_reorder_timer_expired(): up to and including the last expired frame */
void RxReorder::expire_now(uint64_t now_us) {
  for (auto &p : _buf) {
    if (!p || !p->stored) continue;
    Buffer &b = *p;
    bool expired = false;
    uint16_t to = b.head_sn;
    for (unsigned i = 0; i < b.size; i++) {
      const Entry &e = b.entries[sn_add(b.head_sn, i) % b.size];
      if (e.frames.empty()) continue;
      if (now_us - e.since <= kTimeoutUs) break;
      expired = true;
      to = sn_add(b.head_sn, i + 1);
    }
    if (expired) {
      const uint64_t before = _stats.released;
      release_to(b, to);
      _stats.timed_out += _stats.released - before;
    }
  }
}

void RxReorder::hold(Buffer &b, uint16_t sn, const RxFrame &f, uint64_t now_us) {
  Entry &e = b.entries[sn % b.size];
  if (e.frames.empty()) e.since = now_us;
  e.frames.push_back({std::vector<uint8_t>(f.data, f.data + f.len), f});
  b.stored++;
  _stats.held++;
}

/* iwl_mvm_release_frames() */
void RxReorder::release_to(Buffer &b, uint16_t nssn) {
  if (sn_less_in(nssn, b.head_sn, b.size)) return; /* behind the head: a timeout went first */
  if (b.stored && !sn_less_in(b.head_sn, nssn, b.size) && nssn != b.head_sn) {
    /* A whole window or more ahead: nothing held can still be completed. */
    flush_all(b);
    b.head_sn = nssn;
    return;
  }
  while (sn_less_in(b.head_sn, nssn, b.size)) {
    Entry &e = b.entries[b.head_sn % b.size];
    b.head_sn = sn_add(b.head_sn, 1);
    if (e.frames.empty()) continue;
    std::vector<Held> out = std::move(e.frames);
    e.frames.clear();
    for (const Held &h : out) {
      b.stored--;
      _stats.released++;
      emit(h);
    }
  }
  b.head_sn = nssn;
}

void RxReorder::flush_all(Buffer &b) {
  for (unsigned i = 0; i < b.size && b.stored; i++) {
    Entry &e = b.entries[sn_add(b.head_sn, i) % b.size];
    std::vector<Held> out = std::move(e.frames);
    e.frames.clear();
    for (const Held &h : out) {
      b.stored--;
      _stats.released++;
      emit(h);
    }
  }
  b.stored = 0;
}

void RxReorder::emit(const Held &h) {
  RxFrame f = h.meta;
  f.data = h.bytes.data();
  f.len = h.bytes.size();
  _deliver(f);
}
