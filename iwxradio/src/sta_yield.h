// SPDX-License-Identifier: GPL-2.0-only
/*
 * When the station gives the radio to AWDL. With the access point on another
 * channel the firmware keeps visiting it, and an iPhone's unicast frames sent
 * while we are away are lost: TCP on AWDL falls into retransmission timeouts
 * (hardware, AX203: 3-6 KB/s concurrent against 230 KB/s AWDL-only). While
 * AWDL carries a transfer the station link is paused (WifiStation::set_yield)
 * and comes back once AWDL is idle, and at least every kMaxPauseUS for
 * kVisitUS so the access point keeps the association.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <utility>

namespace sta_yield {

/* AWDL unicast data frames (both directions) within kBusyWindowUS that
 * start a pause. Low on purpose: a transfer already starved by the station
 * arrives as bursts of 40-70 frames between TCP retransmission timeouts,
 * 5-10 frames/s on average (hardware, AX203, 2026-11-26). */
constexpr uint64_t kBusyWindowUS = 1000000;
constexpr uint64_t kBusyFrames = 8;
/* Fewer than kIdleFrames within kIdleWindowUS end it; longer than a TCP
 * retransmission backoff, so a stalled transfer does not end the pause. */
constexpr uint64_t kIdleWindowUS = 5000000;
constexpr uint64_t kIdleFrames = 3;
/* Longest pause, then the station is back for kVisitUS. */
constexpr uint64_t kMaxPauseUS = 20000000;
constexpr uint64_t kVisitUS = 2000000;
/* Least time back on the access point's channel between pauses. */
constexpr uint64_t kMinBackUS = 1000000;

enum class Action { None, Pause, Resume };
enum class Why { None, Busy, Idle, Visit, Ineligible };

class Controller {
public:
  /* `frames`: running count of AWDL unicast data frames; `eligible`: the
   * station is up on a channel other than AWDL's. Call every ~100 ms. */
  Action tick(uint64_t now, uint64_t frames, bool eligible) {
    _hist.emplace_back(now, frames);
    while (_hist.size() > 2 && now - _hist[1].first >= idle_us) _hist.pop_front();
    why = Why::None;
    if (!eligible) {
      if (!paused) return Action::None;
      why = Why::Ineligible;
      return toggle(now, false);
    }
    if (paused) {
      if (now - since >= idle_us && frames - since_window(now, idle_us) < kIdleFrames) {
        why = Why::Idle;
        return toggle(now, false);
      }
      if (now - since >= max_pause_us) {
        why = Why::Visit;
        _visit_until = now + kVisitUS;
        return toggle(now, false);
      }
      return Action::None;
    }
    if (now < _visit_until || (since && now - since < kMinBackUS)) return Action::None;
    if (frames - since_window(now, kBusyWindowUS) >= kBusyFrames) {
      why = Why::Busy;
      return toggle(now, true);
    }
    return Action::None;
  }
  /* The pause ended without a Resume (the station failed or could not
   * pause after all). */
  void resumed(uint64_t now) {
    if (paused) toggle(now, false);
  }
  /* The Pause just returned was not carried out. */
  void refused(uint64_t now) {
    if (!paused) return;
    paused = false;
    pauses--;
    since = now;
  }

  /* radio_main holds pauses (30 s and 120 s) unless --sta-yield-short: a
   * pause covers a whole AirDrop session (ask, accept, upload). */
  uint64_t idle_us = kIdleWindowUS, max_pause_us = kMaxPauseUS;
  bool paused = false;
  uint64_t since = 0; /* last change */
  Why why = Why::None;
  uint64_t pauses = 0, paused_us = 0;

private:
  Action toggle(uint64_t now, bool pause) {
    if (paused && !pause) paused_us += now - since;
    if (pause) pauses++;
    paused = pause;
    since = now;
    return pause ? Action::Pause : Action::Resume;
  }
  /* The count at the oldest sample within `window` of now. */
  uint64_t since_window(uint64_t now, uint64_t window) const {
    for (const auto &s : _hist)
      if (now - s.first <= window) return s.second;
    return _hist.back().second;
  }
  std::deque<std::pair<uint64_t, uint64_t>> _hist;
  uint64_t _visit_until = 0;
};

inline const char *why_name(Why w) {
  switch (w) {
  case Why::Busy: return "AWDL transfer";
  case Why::Idle: return "AWDL idle";
  case Why::Visit: return "keeping the access point";
  case Why::Ineligible: return "station down or on the AWDL channel";
  default: return "";
  }
}

} // namespace sta_yield
