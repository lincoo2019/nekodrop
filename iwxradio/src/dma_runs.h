// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

/* Offsets in an already-owned page pool. Never span a gap in physical pages. */
class DmaRuns {
public:
  void reset(const std::vector<uintptr_t> &frames) {
    free_.clear();
    for (size_t first = 0; first < frames.size();) {
      if (!frames[first]) { first++; continue; }
      size_t next = first + 1;
      while (next < frames.size() && frames[next] == frames[next - 1] + 1) next++;
      free_.emplace(next - first, first);
      first = next;
    }
  }

  std::optional<size_t> take(size_t pages) {
    if (!pages) return {};
    auto it = free_.lower_bound(pages);
    if (it == free_.end()) return {};
    const auto [available, start] = *it;
    free_.erase(it);
    if (available > pages) free_.emplace(available - pages, start + pages);
    return start;
  }

  using Reservations = std::multimap<size_t, size_t>;

  bool can_reserve(const std::vector<size_t> &requests) const {
    auto trial = *this;
    return trial.plan(requests).has_value();
  }

  std::optional<Reservations> reserve(std::vector<size_t> requests) {
    auto trial = *this;
    auto planned = trial.plan(std::move(requests));
    if (planned) free_.swap(trial.free_);
    return planned;
  }

  size_t longest() const { return free_.empty() ? 0 : free_.rbegin()->first; }
  size_t count_runs(size_t n) const {
    if (n == 0) return 0;
    size_t count = 0;
    for (auto it = free_.lower_bound(n); it != free_.end(); ++it)
      count += it->first / n;
    return count;
  }

  void clear() { free_.clear(); }

private:
  std::optional<Reservations> plan(std::vector<size_t> requests) {
    Reservations reserve;
    std::sort(requests.begin(), requests.end(), std::greater<size_t>());
    for (size_t n : requests) {
      if (n <= 1) break;
      const auto start = take(n);
      if (!start) return {};
      reserve.emplace(n, *start);
    }
    return reserve;
  }
  std::multimap<size_t, size_t> free_; // run length -> start offset
};
