// SPDX-License-Identifier: GPL-2.0-only
/*
 * 64-slot 802.11 RX Block Ack reordering buffer for MediaTek MT7922 / MT7921.
 *
 * Connac2 delivers A-MPDU MPDUs in hardware reception order (with retransmitted
 * MPDUs arriving after later sequence numbers). Without host-side reordering,
 * TCP sees spurious out-of-order segments and collapses its congestion window.
 *
 * Adapted from the 802.11-2020 scoreboard algorithm (and YiHoooong/owly's
 * ba_reorder): in-order fast path with zero copies, duplicate suppression,
 * ADDBA/DELBA/BAR tracking, and 40 ms stall-flush timeout.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mtk {

class RxBaReorder {
public:
  static constexpr uint16_t kMaxWin = 64;
  static constexpr uint64_t kStallTimeoutUs = 40000; /* 40 ms */
  static constexpr size_t kMaxSessions = 8;

  struct BufferedFrame {
    bool present = false;
    uint16_t seq = 0;
    uint8_t channel = 0;
    int8_t dbm = -127;
    std::vector<uint8_t> data;
  };

  struct Session {
    bool active = false;
    std::array<uint8_t, 6> peer{};
    uint8_t tid = 0;
    uint16_t head_seq = 0;
    uint16_t win_size = kMaxWin;
    uint32_t buffered = 0;
    uint64_t oldest_us = 0;
    uint64_t last_used_us = 0;
    std::array<BufferedFrame, kMaxWin> slots{};
  };

  template <typename EmitFn>
  void on_addba(const uint8_t peer[6], uint8_t tid, uint16_t ssn, uint16_t win,
                uint64_t now_us, EmitFn &&emit) {
    if (tid >= 8) return;
    Session *s = find_or_alloc(peer, tid, now_us, emit);
    if (s->active) flush_all(*s, emit);
    s->active = true;
    std::memcpy(s->peer.data(), peer, 6);
    s->tid = tid;
    s->head_seq = static_cast<uint16_t>(ssn & 0x0fffu);
    s->win_size = (win == 0 || win > kMaxWin) ? kMaxWin : win;
    s->buffered = 0;
    s->oldest_us = 0;
    s->last_used_us = now_us;
  }

  template <typename EmitFn>
  void on_delba(const uint8_t peer[6], uint8_t tid, EmitFn &&emit) {
    Session *s = find(peer, tid);
    if (!s) return;
    flush_all(*s, emit);
    s->active = false;
  }

  /* 802.11 BlockAckReq (type=Control 0x04, subtype=0x80 -> fc0 == 0x84). */
  template <typename EmitFn>
  void on_bar(const uint8_t *frame, size_t len, uint64_t now_us, EmitFn &&emit) {
    if (len < 20 || frame[0] != 0x84u) return;
    const uint8_t *src = frame + 10;
    const uint16_t bar_ctrl = static_cast<uint16_t>(frame[16] | (frame[17] << 8));
    const uint8_t tid = static_cast<uint8_t>((bar_ctrl >> 12) & 0x0fu);
    const uint16_t bar_info = static_cast<uint16_t>(frame[18] | (frame[19] << 8));
    const uint16_t ssn = static_cast<uint16_t>((bar_info >> 4) & 0x0fffu);
    Session *s = find(src, tid);
    if (!s) return;
    s->last_used_us = now_us;
    const uint16_t diff = seq_diff(ssn, s->head_seq);
    if (diff > 0 && diff < 2048u) {
      advance_to(*s, ssn, emit);
      drain_in_order(*s, now_us, emit);
    }
  }

  /* Returns true if the frame was consumed/emitted by an active BA session;
   * false if caller should deliver it normally (non-QoS or no active session). */
  template <typename EmitFn>
  bool process(const uint8_t *frame, size_t len, uint8_t channel, int8_t dbm,
               uint64_t now_us, EmitFn &&emit) {
    /* Only QoS Data frames (type=2, subtype bit 3 set => (fc0 & 0x8c) == 0x88). */
    if (len < 26 || (frame[0] & 0x8cu) != 0x88u) return false;
    const uint8_t *src = frame + 10;
    const uint16_t seqctl = static_cast<uint16_t>(frame[22] | (frame[23] << 8));
    const uint16_t seq = static_cast<uint16_t>((seqctl >> 4) & 0x0fffu);
    const uint8_t tid = static_cast<uint8_t>(frame[24] & 0x0fu);
    Session *s = find(src, tid);
    if (!s) return false;

    s->last_used_us = now_us;
    const uint16_t diff = seq_diff(seq, s->head_seq);

    /* Fast path: exact next sequence number. */
    if (diff == 0) {
      emit(frame, len, channel, dbm);
      s->head_seq = static_cast<uint16_t>((s->head_seq + 1u) & 0x0fffu);
      drain_in_order(*s, now_us, emit);
      return true;
    }

    /* Old / duplicate frame behind window start: drop. */
    if (diff >= 2048u) {
      _duplicates++;
      return true;
    }

    /* Beyond right edge of window: shift window forward. */
    if (diff >= s->win_size) {
      const uint16_t new_head =
          static_cast<uint16_t>((seq - s->win_size + 1u) & 0x0fffu);
      advance_to(*s, new_head, emit);
    }

    /* Check again in case window shift moved head_seq onto seq. */
    if (seq == s->head_seq) {
      emit(frame, len, channel, dbm);
      s->head_seq = static_cast<uint16_t>((s->head_seq + 1u) & 0x0fffu);
      drain_in_order(*s, now_us, emit);
      return true;
    }

    /* Buffer out-of-order frame in slot. */
    BufferedFrame &slot = s->slots[seq & (kMaxWin - 1u)];
    if (slot.present && slot.seq == seq) {
      _duplicates++;
      return true;
    }
    if (slot.present) {
      /* Stale occupant from an earlier wrap (shouldn't happen, emit safely). */
      emit(slot.data.data(), slot.data.size(), slot.channel, slot.dbm);
      s->buffered--;
    }
    slot.present = true;
    slot.seq = seq;
    slot.channel = channel;
    slot.dbm = dbm;
    slot.data.assign(frame, frame + len);
    if (s->buffered == 0) s->oldest_us = now_us;
    s->buffered++;
    _reordered++;
    return true;
  }

  template <typename EmitFn>
  void flush_expired(uint64_t now_us, EmitFn &&emit) {
    for (auto &s : _sessions) {
      if (!s.active || s.buffered == 0) continue;
      if (now_us < s.oldest_us || (now_us - s.oldest_us) < kStallTimeoutUs) continue;
      /* Find the nearest occupied slot ahead of head_seq and jump to it. */
      uint16_t best_diff = kMaxWin;
      for (uint16_t d = 1; d < s.win_size; d++) {
        const uint16_t cand = static_cast<uint16_t>((s.head_seq + d) & 0x0fffu);
        const auto &slot = s.slots[cand & (kMaxWin - 1u)];
        if (slot.present && slot.seq == cand) {
          best_diff = d;
          break;
        }
      }
      if (best_diff < s.win_size) {
        s.head_seq = static_cast<uint16_t>((s.head_seq + best_diff) & 0x0fffu);
        _timeouts++;
        drain_in_order(s, now_us, emit);
      } else {
        s.buffered = 0;
        s.oldest_us = 0;
      }
    }
  }

  void clear() {
    for (auto &s : _sessions) {
      s.active = false;
      s.buffered = 0;
      for (auto &sl : s.slots) {
        sl.present = false;
        sl.data.clear();
      }
    }
  }

  uint64_t reordered() const { return _reordered; }
  uint64_t duplicates() const { return _duplicates; }
  uint64_t timeouts() const { return _timeouts; }

private:
  static uint16_t seq_diff(uint16_t a, uint16_t b) {
    return static_cast<uint16_t>((a - b) & 0x0fffu);
  }

  Session *find(const uint8_t peer[6], uint8_t tid) {
    for (auto &s : _sessions) {
      if (s.active && s.tid == tid && std::memcmp(s.peer.data(), peer, 6) == 0)
        return &s;
    }
    return nullptr;
  }

  template <typename EmitFn>
  Session *find_or_alloc(const uint8_t peer[6], uint8_t tid, uint64_t /*now_us*/, EmitFn &&emit) {
    if (auto *s = find(peer, tid)) return s;
    Session *lru = &_sessions[0];
    for (auto &s : _sessions) {
      if (!s.active) return &s;
      if (s.last_used_us < lru->last_used_us) lru = &s;
    }
    flush_all(*lru, emit);
    lru->active = false;
    return lru;
  }

  template <typename EmitFn>
  void advance_to(Session &s, uint16_t new_head, EmitFn &&emit) {
    while (s.head_seq != new_head) {
      auto &slot = s.slots[s.head_seq & (kMaxWin - 1u)];
      if (slot.present && slot.seq == s.head_seq) {
        emit(slot.data.data(), slot.data.size(), slot.channel, slot.dbm);
        slot.present = false;
        if (s.buffered > 0) s.buffered--;
      }
      s.head_seq = static_cast<uint16_t>((s.head_seq + 1u) & 0x0fffu);
    }
  }

  template <typename EmitFn>
  void drain_in_order(Session &s, uint64_t now_us, EmitFn &&emit) {
    while (s.buffered > 0) {
      auto &slot = s.slots[s.head_seq & (kMaxWin - 1u)];
      if (!slot.present || slot.seq != s.head_seq) break;
      emit(slot.data.data(), slot.data.size(), slot.channel, slot.dbm);
      slot.present = false;
      s.buffered--;
      s.head_seq = static_cast<uint16_t>((s.head_seq + 1u) & 0x0fffu);
    }
    s.oldest_us = s.buffered > 0 ? now_us : 0;
  }

  template <typename EmitFn>
  void flush_all(Session &s, EmitFn &&emit) {
    for (uint16_t d = 0; d < s.win_size && s.buffered > 0; d++) {
      const uint16_t cand = static_cast<uint16_t>((s.head_seq + d) & 0x0fffu);
      auto &slot = s.slots[cand & (kMaxWin - 1u)];
      if (slot.present && slot.seq == cand) {
        emit(slot.data.data(), slot.data.size(), slot.channel, slot.dbm);
        slot.present = false;
        s.buffered--;
      }
    }
    for (auto &slot : s.slots) slot.present = false;
    s.buffered = 0;
    s.oldest_us = 0;
  }

  std::array<Session, kMaxSessions> _sessions{};
  uint64_t _reordered = 0;
  uint64_t _duplicates = 0;
  uint64_t _timeouts = 0;
};

} // namespace mtk
