// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental Apple-style coexistence plan (--coex-plan), ported from
 * ForbiddenItlwm PR #30 (zheznanohana, "AWDL coexistence: Apple-style router
 * entries in the advertised schedule"): include/AWDLChannels.hpp's
 * routerEntryChannel, ItlIwx.cpp's awdlAppleTick tables and traffic class,
 * and the silent listener of awdlCoexTxSend.
 *
 * With the station on another 5 GHz channel than AWDL, the firmware shares
 * the air between the two on its own clock, unaligned with the peer's AWDL
 * entries. The plan names the station's channel in some of our 16 entries
 * (as macOS does beside an infra link), keeps the listener silent and holds
 * aggregate sending in them, so the time away falls in the entries the peer
 * already expects us to be away.
 *
 * Differences from upstream: awdld (OWL) builds the sync frames, so the radio
 * rewrites the channel sequences (TLV 4 and TLV 18) of the frames awdld hands
 * it, and only while awdld's AW counter is in step with the peer's (awdld
 * follows the peer as master); otherwise the plan stays inactive.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "awdl_schedule.h"

namespace awdl_plan {

/* The fixed-schedule channels (awdl::fixed_schedule_channel). */
inline bool fixed_channel(unsigned ch) {
  if (ch >= 1 && ch <= 13) return true;
  if (ch >= 36 && ch <= 48) return ch % 4 == 0;
  if (ch >= 149 && ch <= 165) return (ch - 149) % 4 == 0;
  return false;
}

/* A channel allowed in our schedule only as a router entry: the fixed ones
 * plus radar channels a station may use (52-64, 100-144). */
inline bool router_entry_channel(unsigned ch) {
  if (fixed_channel(ch)) return true;
  if (ch >= 52 && ch <= 64) return ch % 4 == 0;
  if (ch >= 100 && ch <= 144) return ch % 4 == 0;
  return false;
}

/* 20 MHz global operating class (802.11 Annex E, table E-4). */
inline uint8_t operating_class20(unsigned ch) {
  if (ch >= 1 && ch <= 13) return 81;
  if (ch >= 36 && ch <= 48) return 115;
  if (ch >= 52 && ch <= 64) return 118;
  if (ch >= 100 && ch <= 144) return 121;
  if (ch >= 149 && ch <= 161) return 124;
  if (ch == 165) return 125;
  return 0;
}

/* Apple's _channelSeqSlotTypes rows (IO80211Family, Tahoe 26.7.3), as PR #30:
 * 1 = the entry goes to the router. */
constexpr uint8_t kDataInfraIdle[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
constexpr uint8_t kDataInfraData[16] = {0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
constexpr uint8_t kDataInfraStream[16] = {0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
constexpr uint8_t kIdle[16] = {0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1};
/* AWDL counts as moving data this long after its last data frame. */
constexpr uint64_t kDataHoldUS = 3000000;

enum class InfraClass : uint8_t { Idle = 0, Streaming = 2, Data = 3 };

/* The router's traffic class once a second, Apple's thresholds: streaming at
 * an average packet of 1401 bytes or more, data above 100 packets/s. */
struct InfraMeter {
  uint64_t last_us = 0, last_pkts = 0, last_bytes = 0, pps = 0, avg = 0;
  InfraClass cls = InfraClass::Idle;
  uint32_t changes = 0;
  /* True when the class changed. */
  bool note(uint64_t now_us, uint64_t pkts, uint64_t bytes) {
    if (last_us && now_us - last_us < 1000000) return false;
    const uint64_t elapsed = last_us ? now_us - last_us : 1000000;
    const uint64_t dp = pkts - last_pkts, db = bytes - last_bytes;
    last_us = now_us;
    last_pkts = pkts;
    last_bytes = bytes;
    pps = elapsed ? dp * 1000000 / elapsed : 0;
    avg = dp ? db / dp : 0;
    InfraClass c = InfraClass::Idle;
    if (dp >= 6 && avg >= 1401) c = InfraClass::Streaming;
    else if (pps > 100) c = InfraClass::Data;
    if (c == cls) return false;
    cls = c;
    changes++;
    return true;
  }
};

/* Which entries go to the router; false (and all zero) when the plan is off:
 * a 5 GHz station on another channel than the listener, or a 160 MHz one. A
 * 2.4 GHz station keeps none, as Apple's sdbModeAnd24Infra does. */
inline bool plan_entries(bool station_up, unsigned station_ch, unsigned station_width, unsigned listener_ch,
                         bool awdl_data, InfraClass cls, uint8_t away[16]) {
  std::memset(away, 0, 16);
  if (!station_up || station_ch <= 14 || station_ch > 255 || !router_entry_channel(station_ch) ||
      !fixed_channel(listener_ch) || (station_ch == listener_ch && station_width < 160))
    return false;
  const uint8_t *t = !awdl_data                     ? kIdle
                     : cls == InfraClass::Streaming ? kDataInfraStream
                     : cls == InfraClass::Data      ? kDataInfraData
                                                    : kDataInfraIdle;
  std::memcpy(away, t, 16);
  return true;
}

/* Rewrites one channel sequence (count, encoding, duplicate, step, fill, 16
 * entries) at `p`, `n` bytes available: entries in `away` name the router.
 * False when the layout isn't the 16-entry one awdld sends. */
inline bool rewrite_chanseq(uint8_t *p, size_t n, const uint8_t away[16], uint8_t router_ch) {
  if (n < 6 || p[0] != 15) return false;
  const uint8_t enc = p[1];
  const size_t elen = enc == 0 ? 1 : (enc == 1 || enc == 3) ? 2 : 0;
  if (!elen || n < 6 + 16 * elen) return false;
  for (unsigned k = 0; k < 16; k++) {
    if (!away[k]) continue;
    uint8_t *e = p + 6 + k * elen;
    if (enc == 0) e[0] = router_ch;
    else if (enc == 1) { e[0] = router_ch <= 14 ? 0x2b : 0x1b; e[1] = router_ch; }
    else { e[0] = router_ch; e[1] = operating_class20(router_ch); }
  }
  return true;
}

/* The AW counter our sync TLV (4) carries, or -1. */
inline int frame_aw_counter(const uint8_t *f, size_t len) {
  for (size_t i = 40; i + 3 <= len;) {
    const size_t n = size_t(f[i + 1]) | size_t(f[i + 2]) << 8;
    if (i + 3 + n > len) return -1;
    if (f[i] == 4 && n >= 31) return int(f[i + 3 + 29] | f[i + 3 + 30] << 8);
    i += 3 + n;
  }
  return -1;
}

/* Whether our counter's extended AW is the peer's, give or take one. */
inline bool in_step(int our_counter, uint32_t peer_eaw) {
  if (our_counter < 0) return false;
  const unsigned d = (unsigned(our_counter) / 4 - peer_eaw) & 15u;
  return d == 0 || d == 1 || d == 15;
}

/* Rewrites the channel sequences of an AWDL action frame of ours (TLV 4's
 * appended one and TLV 18). True when at least one was rewritten. */
inline bool rewrite_frame(uint8_t *f, size_t len, const uint8_t away[16], uint8_t router_ch) {
  if (len < 40 || f[0] != 0xd0 || f[24] != 0x7f || f[28] != 0x08) return false;
  bool any = false;
  for (size_t i = 40; i + 3 <= len;) {
    const size_t n = size_t(f[i + 1]) | size_t(f[i + 2]) << 8;
    if (i + 3 + n > len) break;
    uint8_t *v = f + i + 3;
    if (f[i] == 18) any |= rewrite_chanseq(v, n, away, router_ch);
    else if (f[i] == 4 && n > 33) any |= rewrite_chanseq(v + 33, n - 33, away, router_ch);
    i += 3 + n;
  }
  return any;
}

}  // namespace awdl_plan
