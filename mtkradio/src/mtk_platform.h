// SPDX-License-Identifier: GPL-2.0-only
/* Tiny cross-platform primitives so mtk_dev.cpp compiles identically on
 * Windows (RegProxy + AWE DMA) and Linux (vfio-pci bring-up harness). */
#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mtk::plat {
inline uint64_t now_ms() { return GetTickCount64(); }
inline void sleep_ms(unsigned ms) { Sleep(ms); }
inline void mem_barrier() { MemoryBarrier(); }
inline std::string path_join(const std::string &a, const std::string &b) {
  return a + "\\" + b;
}
} // namespace mtk::plat

#else
#include <atomic>
#include <chrono>
#include <thread>

namespace mtk::plat {
inline uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
inline void sleep_ms(unsigned ms) {
  if (ms == 0)
    std::this_thread::yield();
  else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
inline void mem_barrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }
inline std::string path_join(const std::string &a, const std::string &b) {
  return a + "/" + b;
}
} // namespace mtk::plat
#endif
