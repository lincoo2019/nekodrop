// SPDX-License-Identifier: GPL-2.0-only
/*
 * AWDL radio policy, ported from ForbiddenItlwm 0.6.0 (retropineaple,
 * GPL-2.0): include/AWDLBlockAck.hpp, include/AWDLProbePolicy.hpp,
 * include/AWDLChannels.hpp and the awdlCoex* helpers in ItlIwx.cpp. The logic
 * and constants are upstream's; the names are shortened and the macOS state
 * around them is replaced by plain structs.
 */
#pragma once
#include <vector>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace awdl {

/* ---------------------------------------------------------- channels */

/* Channels a fixed AWDL schedule may name (no radar duty). */
inline bool fixed_schedule_channel(unsigned ch) {
  if (ch >= 1 && ch <= 13) return true;
  if (ch >= 36 && ch <= 48) return ch % 4 == 0;
  if (ch >= 149 && ch <= 165) return (ch - 149) % 4 == 0;
  return false;
}

/* First channel of the 80 MHz block (149-161 or 36-48), 0 otherwise. */
inline unsigned block80_first(unsigned ch) {
  if (ch >= 149 && ch <= 161 && (ch - 149) % 4 == 0) return 149;
  if (ch >= 36 && ch <= 48 && ch % 4 == 0) return 36;
  return 0;
}

/* The peer's 16-slot channel sequence (TLV 18) from an AWDL action frame
 * without FCS: one channel number per slot. */
inline bool peer_sequence(const uint8_t *frame, size_t length, uint8_t out[16]) {
  if (!frame || length < 40) return false;
  for (size_t i = 40; i + 3 <= length;) {
    const size_t value = size_t(frame[i + 1]) | size_t(frame[i + 2]) << 8;
    if (i + 3 + value > length) return false;
    if (frame[i] == 18) {
      if (value < 6 + 32) return false;
      for (unsigned n = 0; n < 16; ++n) out[n] = frame[i + 3 + 6 + 2 * n];
      return true;
    }
    i += 3 + value;
  }
  return false;
}

/* Whether the sequence names `ch` as an AWDL channel. Slots 6, 7, 14, 15 are
 * the phone's own Wi-Fi; a channel there counts only with wifi_too and when
 * it also appears in another slot (upstream PR #21's min rule). */
inline bool peer_names(const uint8_t seq[16], unsigned ch, bool wifi_too = false) {
  if (!wifi_too && (seq[6] == ch || seq[7] == ch || seq[14] == ch || seq[15] == ch)) return false;
  for (unsigned n = 0; n < 16; ++n)
    if ((n & 7) < 6 && seq[n] == ch) return true;
  return false;
}

/* The phone's 5 GHz AWDL channel: 44 or 149, 0 if neither. 44 wins. */
inline unsigned peer_channel(const uint8_t seq[16]) {
  const bool names149 = peer_names(seq, 149);
  if (peer_names(seq, 44, !names149)) return 44;
  return names149 ? 149 : 0;
}

/* The country a peer states in its AWDL frames: TLV 12, value bytes 2-3. */
inline bool peer_country(const uint8_t *frame, size_t length, char out[3]) {
  if (!frame || length < 40) return false;
  for (size_t i = 40; i + 3 <= length;) {
    const size_t value = size_t(frame[i + 1]) | size_t(frame[i + 2]) << 8;
    if (i + 3 + value > length) return false;
    if (frame[i] == 12) {
      if (value < 4) return false;
      const char a = char(frame[i + 5]), b = char(frame[i + 6]);
      if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z' || (a == 'Z' && b == 'Z')) return false;
      out[0] = a;
      out[1] = b;
      out[2] = 0;
      return true;
    }
    i += 3 + value;
  }
  return false;
}

/* The same country this many times in a row before it is believed. */
constexpr unsigned kPeerCountryFrames = 5;
struct PeerCountry {
  char candidate[3] = {}, confirmed[3] = {};
  unsigned run = 0;
  /* True when `confirmed` just changed. */
  bool note(const char c[3]) {
    if (std::strcmp(c, candidate) != 0) {
      std::memcpy(candidate, c, 3);
      run = 0;
    }
    if (++run >= kPeerCountryFrames && std::strcmp(candidate, confirmed) != 0) {
      std::memcpy(confirmed, candidate, 3);
      return true;
    }
    return false;
  }
};

/* The AWDL action frame header: management action, category 127, Apple OUI,
 * type 8. */
inline bool is_awdl_action(const uint8_t *f, size_t len) {
  return len >= 30 && f[0] == 0xd0 && f[24] == 0x7f && f[25] == 0x00 && f[26] == 0x17 &&
         f[27] == 0xf2 && f[28] == 0x08;
}

/* ------------------------------------------------------------- FCS */

/* IEEE 802.3 CRC-32, as the 802.11 FCS uses. */
inline uint32_t crc32_ieee(const uint8_t *p, size_t n) {
  struct Table {
    uint32_t t[256];
    Table() {
      for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
      }
    }
  };
  static const Table table;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = table.t[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* The length without the FCS when the last four bytes are this frame's valid
 * FCS, else `len`. The monitor context asks for IWX_MAC_FILTER_IN_CRC32
 * ("append the FCS"), and whether the firmware does so differs between
 * cards: OWL rejects an AWDL action frame with any bytes after its last
 * TLV, before it marks a MIF, so a peer whose frames keep their FCS never
 * turns valid ("not valid yet", AX211/AX411, #24/#25). A frame without an
 * FCS matches by chance with odds of 2^-32. */
inline size_t strip_valid_fcs(const uint8_t *f, size_t len) {
  if (len < 14) return len;
  const uint8_t *t = f + len - 4;
  const uint32_t fcs = uint32_t(t[0]) | uint32_t(t[1]) << 8 | uint32_t(t[2]) << 16 | uint32_t(t[3]) << 24;
  return crc32_ieee(f, len - 4) == fcs ? len - 4 : len;
}

/* How an AWDL action frame's TLVs (after the 24-byte header and 16 fixed
 * bytes) fit the frame: the bytes left after the last whole TLV, whether
 * all of them are zero (padding awdld trims), and whether a version TLV
 * (21), which awdld needs before it takes the peer as valid, is there. */
struct AwdlTlvFit {
  size_t left = 0;
  bool left_zero = true;
  bool has_version = false;
};
inline AwdlTlvFit awdl_tlv_fit(const uint8_t *f, size_t len) {
  AwdlTlvFit r;
  size_t off = 40;
  if (len < off) return r;
  while (len - off >= 3) {
    const size_t n = size_t(f[off + 1]) | size_t(f[off + 2]) << 8;
    if (n > len - off - 3) break;
    if (f[off] == 21 && n >= 2) r.has_version = true;
    off += 3 + n;
  }
  r.left = len - off;
  for (size_t i = off; i < len; i++)
    if (f[i]) r.left_zero = false;
  return r;
}

/* --------------------------------------------------------- Block Ack */

constexpr uint16_t kBaWindowMax = 64;
constexpr size_t kAddbaResponseLength = 33;

struct AddbaRequest {
  uint8_t peer[6], bssid[6];
  uint8_t token, tid;
  bool immediate;
  uint16_t window, ssn;
};

/* A management action frame to `own`: category 3 (Block Ack), action 0. */
inline bool parse_addba_request(const uint8_t *f, size_t length, const uint8_t *own, AddbaRequest *out) {
  if (!f || !own || !out || length < 33 || f[0] != 0xd0 || std::memcmp(f + 4, own, 6) || f[24] != 3 ||
      f[25] != 0)
    return false;
  const uint16_t params = uint16_t(f[27] | f[28] << 8);
  const uint16_t seqctl = uint16_t(f[31] | f[32] << 8);
  const unsigned tid = (params >> 2) & 15U;
  if (tid >= 8) return false;
  std::memcpy(out->peer, f + 10, 6);
  std::memcpy(out->bssid, f + 16, 6);
  out->token = f[26];
  out->tid = uint8_t(tid);
  out->immediate = params & 2U;
  const uint16_t asked = params >> 6;
  out->window = !asked || asked > kBaWindowMax ? kBaWindowMax : asked;
  out->ssn = seqctl >> 4;
  return true;
}

/* DELBA from the peer (category 3, action 2) to `own`. */
inline bool parse_delba(const uint8_t *f, size_t length, const uint8_t *own, bool *peer_was_sender,
                        uint8_t *tid) {
  if (!f || !own || length < 28 || f[0] != 0xd0 || std::memcmp(f + 4, own, 6) || f[24] != 3 || f[25] != 2)
    return false;
  const uint16_t params = uint16_t(f[26] | f[27] << 8);
  *peer_was_sender = (params >> 11) & 1U; /* initiator bit */
  *tid = uint8_t((params >> 12) & 15U);
  return true;
}

/* DELBA (category 3, action 2) to `peer`: `initiator` when we are the
 * session's sender. Reason 39 (QSTA_TIMEOUT), as Linux sends for an idle
 * session. */
constexpr size_t kDelbaLength = 30;
inline size_t build_delba(uint8_t *out, const uint8_t *peer, const uint8_t *own, const uint8_t *bssid, uint8_t tid,
                          bool initiator, uint16_t seq) {
  std::memset(out, 0, kDelbaLength);
  out[0] = 0xd0;
  std::memcpy(out + 4, peer, 6);
  std::memcpy(out + 10, own, 6);
  std::memcpy(out + 16, bssid, 6);
  const uint16_t sc = uint16_t(seq << 4);
  out[22] = uint8_t(sc);
  out[23] = uint8_t(sc >> 8);
  out[24] = 3;
  out[25] = 2;
  const uint16_t params = uint16_t((initiator ? 1U : 0U) << 11 | unsigned(tid & 15U) << 12);
  out[26] = uint8_t(params);
  out[27] = uint8_t(params >> 8);
  out[28] = 39;
  return kDelbaLength;
}

/* A TX Block Ack session is ended once a bulk send is over: nothing waits or
 * is queued for the peer and no aggregate frame went out for this long.
 * While agreed the listener takes group frames only, so the peer's own
 * DELBA (sent when its side times the session out) never arrives, and the
 * next send's frames went out as A-MPDUs the peer no longer accepted:
 * connection refused, then the peer gone until the radio restarted (#4). */
constexpr uint64_t kBaTxIdleEndUS = 1500000;
inline bool ba_tx_session_idle(bool bulk, size_t waiting, unsigned queued, uint64_t last_agg_us, uint64_t agreed_us,
                               uint64_t now_us) {
  if (bulk || waiting || queued) return false;
  const uint64_t since = last_agg_us > agreed_us ? last_agg_us : agreed_us;
  return since && now_us - since >= kBaTxIdleEndUS;
}

inline size_t build_addba_response(uint8_t *out, const AddbaRequest &r, const uint8_t *own, uint16_t seq) {
  std::memset(out, 0, kAddbaResponseLength);
  out[0] = 0xd0;
  std::memcpy(out + 4, r.peer, 6);
  std::memcpy(out + 10, own, 6);
  std::memcpy(out + 16, r.bssid, 6);
  const uint16_t sc = uint16_t(seq << 4);
  out[22] = uint8_t(sc);
  out[23] = uint8_t(sc >> 8);
  out[24] = 3;
  out[25] = 1;
  out[26] = r.token;
  /* 27-28: status 0, success. */
  const uint16_t params = uint16_t(2U | unsigned(r.tid) << 2 | unsigned(r.window) << 6);
  out[29] = uint8_t(params);
  out[30] = uint8_t(params >> 8);
  /* 31-32: timeout 0. */
  return kAddbaResponseLength;
}

/* Promiscuous receive off while a lot of data arrives: with a session open
 * and the filter on, the firmware does not acknowledge the peer's frames. */
constexpr uint64_t kBaFilterCheckUS = 1000000;
constexpr uint32_t kBaFilterBusyFrames = 16, kBaFilterQuietFrames = 8, kBaFilterQuietChecks = 3;
struct BaFilter {
  uint64_t last_check_us = 0;
  uint32_t last_frames = 0, quiet_checks = 0;
  bool off = false;
  /* Returns true when the filter should change; `off` holds the new state. */
  bool note(uint64_t now_us, uint32_t frames) {
    if (!last_check_us) {
      last_check_us = now_us;
      last_frames = frames;
      return false;
    }
    if (now_us - last_check_us < kBaFilterCheckUS) return false;
    const uint32_t delta = frames - last_frames;
    last_check_us = now_us;
    last_frames = frames;
    if (!off) {
      quiet_checks = 0;
      if (delta < kBaFilterBusyFrames) return false;
      off = true;
      return true;
    }
    if (delta >= kBaFilterQuietFrames) {
      quiet_checks = 0;
      return false;
    }
    if (++quiet_checks < kBaFilterQuietChecks) return false;
    quiet_checks = 0;
    off = false;
    return true;
  }
};

/* ------------------------------------------------------- send policy */

/* A failed frame to one station is sent again for 400 ms (the peer may be on
 * another channel for a few slots), with a growing gap. */
constexpr uint64_t kResendWindowUS = 400000;
constexpr unsigned kResendMaxAttempts = 100;
constexpr uint64_t resend_backoff_us(unsigned attempts) {
  return attempts <= 1U ? 0U : attempts == 2U ? 2000U : 5000U;
}

/* Unicast data rate ladder: legacy 24, HT MCS 4, 5, 7 (20 MHz, 1 stream),
 * then VHT 80 MHz 2 streams MCS 4, 7 and 8 (with LDPC + SGI) while on 80 MHz.
 * Down after a window of 32 with over 20% failures, up after one with under 5%. */
constexpr uint8_t kRateMcs[7] = {0, 4, 5, 7, 4, 7, 8};
struct RateLadder {
  unsigned step = 0;
  uint32_t ok = 0, failed = 0, ups = 0, downs = 0;
  void note(bool acked, bool wide80) {
    ++(acked ? ok : failed);
    const uint32_t window = ok + failed;
    if (window < 32) return;
    const unsigned top = wide80 ? 7u : 4u;
    if (failed * 5 > window && step > 0) {
      --step;
      ++downs;
    } else if (failed * 20 < window && step + 1 < top) {
      ++step;
      ++ups;
    }
    if (step >= top) step = top - 1;
    ok = failed = 0;
  }
};


/* ---- Sending with Block Ack (experimental --aggregate), from ForbiddenItlwm's
 * include/AWDLBlockAck.hpp. */

/* Asks `peer` for a session to send to it: immediate, no A-MSDU, no timeout. */
inline size_t build_addba_request(uint8_t *out, const uint8_t *peer, const uint8_t *own, const uint8_t *bssid,
                                  uint8_t token, uint8_t tid, uint16_t window, uint16_t start_seq, uint16_t seq) {
  std::memset(out, 0, kAddbaResponseLength);
  out[0] = 0xd0;
  std::memcpy(out + 4, peer, 6);
  std::memcpy(out + 10, own, 6);
  std::memcpy(out + 16, bssid, 6);
  const uint16_t sc = static_cast<uint16_t>(seq << 4);
  out[22] = static_cast<uint8_t>(sc);
  out[23] = static_cast<uint8_t>(sc >> 8);
  out[24] = 3; /* Block Ack */
  out[25] = 0; /* ADDBA request */
  out[26] = token;
  const uint16_t params = static_cast<uint16_t>(2u | unsigned(tid & 7) << 2 | unsigned(window) << 6);
  out[27] = static_cast<uint8_t>(params);
  out[28] = static_cast<uint8_t>(params >> 8);
  const uint16_t start = static_cast<uint16_t>(start_seq << 4);
  out[31] = static_cast<uint8_t>(start);
  out[32] = static_cast<uint8_t>(start >> 8);
  return kAddbaResponseLength;
}

struct AddbaAnswer {
  uint8_t token, tid;
  uint16_t status, window;
  bool immediate;
};
/* The peer's answer to our request: category 3, action 1, to `own`. */
inline bool parse_addba_answer(const uint8_t *f, size_t length, const uint8_t *own, AddbaAnswer *out) {
  if (length < 33 || f[0] != 0xd0 || std::memcmp(f + 4, own, 6) != 0 || f[24] != 3 || f[25] != 1) return false;
  out->token = f[26];
  out->status = static_cast<uint16_t>(f[27] | f[28] << 8);
  const uint16_t params = static_cast<uint16_t>(f[29] | f[30] << 8);
  out->tid = static_cast<uint8_t>((params >> 2) & 15u);
  out->window = params >> 6;
  out->immediate = params & 2u;
  return true;
}

/* A plain data frame as QoS data on `tid` with our sequence number: the new
 * length, or 0 if `in` is not plain data. */
inline size_t build_qos_data(const uint8_t *in, size_t length, uint8_t tid, uint16_t seq, std::vector<uint8_t> &out) {
  if (length < 24 || in[0] != 0x08) return 0;
  out.assign(in, in + 24);
  out[0] = 0x88;
  const uint16_t sc = static_cast<uint16_t>(seq << 4);
  out[22] = static_cast<uint8_t>(sc);
  out[23] = static_cast<uint8_t>(sc >> 8);
  out.push_back(tid & 7u);
  out.push_back(0);
  out.insert(out.end(), in + 24, in + length);
  return out.size();
}

/* The A-MPDU path only while a bulk send runs: short exchanges such as a
 * connection's setup stay on the monitor queue, where the host resends a
 * frame lost while the peer is away (the firmware's aggregate queue does
 * not, and those stalled through it upstream). Trigger aggressively (24
 * payload frames in 250 ms, ~35 KB) so even medium files enter A-MPDU fast. */
constexpr uint64_t kBaTxCheckUS = 250000;
constexpr uint32_t kBaTxBulkFrames = 24, kBaTxQuietFrames = 8, kBaTxQuietChecks = 4;
struct BaTxBulk {
  uint64_t last_us = 0;
  uint32_t last_frames = 0, quiet_checks = 0;
  bool bulk = false;
  /* `frames` counts payload-sized unicast data frames to the peer. True on a change. */
  bool note(uint64_t now_us, uint32_t frames, bool fast_entry) {
    const bool short_sample = fast_entry && !bulk;
    const uint64_t sample = short_sample ? 100000ULL : kBaTxCheckUS;
    const uint32_t entry = short_sample ? 8u : kBaTxBulkFrames;
    if (!last_us) {
      last_us = now_us;
      last_frames = frames;
      return false;
    }
    if (now_us - last_us < sample) return false;
    const uint32_t delta = frames - last_frames;
    last_us = now_us;
    last_frames = frames;
    if (!bulk) {
      quiet_checks = 0;
      if (delta < entry) return false;
      bulk = true;
      return true;
    }
    if (delta >= kBaTxQuietFrames) {
      quiet_checks = 0;
      return false;
    }
    if (++quiet_checks < kBaTxQuietChecks) return false;
    quiet_checks = 0;
    bulk = false;
    return true;
  }
};

/* Experimental A-MPDU negotiation bounds (#4): ask every 100 ms up to 20 asks
 * (or 5 s wall clock), then fall back cleanly to the single-frame queue and
 * retry after 60 s instead of looping in `asking` indefinitely. */
constexpr uint64_t kBaTxAskIntervalUS = 100000;
constexpr unsigned kBaTxAskMax = 20;
constexpr uint64_t kBaTxAskTimeoutUS = 5000000;
constexpr uint64_t kBaTxFailBackoffUS = 60000000;

/* Experimental A-MPDU send fallback (#4): if the peer agreed and we have fed at
 * least two full 64-frame Block Ack windows with zero Block Ack notifications
 * and >= 64 single-frame completions, the firmware is not aggregating on this
 * queue; fall back to the single-frame queue instead of crawling at ~40 KB/s. */
constexpr uint64_t kBaTxNoBaFallbackFrames = 128;
constexpr uint64_t kBaTxNoBaFallbackSingles = 64;
inline bool ba_tx_stalled_as_singles(uint64_t frames, uint64_t notifs, uint64_t singles) {
  return notifs == 0 && frames >= kBaTxNoBaFallbackFrames && singles >= kBaTxNoBaFallbackSingles;
}

} // namespace awdl
