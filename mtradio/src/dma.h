// SPDX-License-Identifier: GPL-2.0-only
/* Memory the card reads and writes by DMA, from a user-mode process.
 *
 * UMDF gives a user-mode driver register access but no DMA. Following
 * winawdl's design, the process locks physical pages with the documented AWE
 * call AllocateUserPhysicalPages (SeLockMemoryPrivilege) and uses their
 * physical addresses as bus addresses; winawdl measured that this works with
 * Kernel DMA Protection on.
 *
 * winawdl asked for 2 MiB large pages, which a long-running PC may have none
 * of. This pool takes ordinary 4 KiB pages instead: a large batch, sorted by
 * physical address and mapped in that order, so physically adjacent frames
 * are adjacent in the view too. A buffer larger than a page is carved from a
 * run of adjacent frames (the largest this driver needs is 32 KiB, 8 pages);
 * page-sized buffers take any page; pages never handed out are returned by
 * trim(). The Intel 22000 family takes 64-bit addresses, so any frame will do. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DmaBuf {
  uint8_t *va = nullptr;
  uint64_t pa = 0;
  size_t size = 0;
  explicit operator bool() const { return va != nullptr; }
};

class DmaPool {
public:
  DmaPool() = default;
  ~DmaPool();
  DmaPool(const DmaPool &) = delete;
  DmaPool &operator=(const DmaPool &) = delete;

  static constexpr size_t kPageBytes = 4096;

  /* Locks up to `pages` 4 KiB pages; fails below `min_pages`. With
   * `below` set, keeps only pages under that physical address (MediaTek's
   * WFDMA takes 32-bit addresses), asking Windows for more batches until
   * enough are found. */
  bool init(size_t pages, size_t min_pages, std::string &error, uint64_t below = 0);
  /* A zeroed, physically contiguous buffer aligned to `align` (at most 4096).
   * Never freed individually: the pool goes as a whole after DMA stops. */
  DmaBuf alloc(size_t size, size_t align = 256);
  /* One zeroed 4 KiB page. */
  DmaBuf alloc_page();
  /* Returns every page not handed out to Windows. */
  void trim();
  /* Locks `pages` more pages and merges them in, for more runs of adjacent
   * frames. Only before the first allocation. */
  bool grow(size_t pages, std::string &error);
  /* Whether buffers of these sizes (in pages) can all be carved from runs of
   * adjacent frames (first fit, largest first). */
  bool fits(std::vector<size_t> runs) const;
  /* Physical memory Windows has free, in bytes (0 if unknown). */
  static uint64_t available_bytes();
  /* Longest run of adjacent frames, in pages (a diagnostic). */
  size_t longest_run() const;
  /* How many disjoint runs of `n` adjacent frames there are. */
  size_t runs_of(size_t n) const;
  size_t pages_locked() const { return _frames.size(); }
  size_t pages_used() const { return _used_pages; }

private:
  DmaBuf page_at(size_t index);

  uint8_t *_view = nullptr;
  std::vector<uintptr_t> _frames; /* sorted; index i is mapped at _view + i * 4 KiB */
  std::vector<bool> _taken;
  size_t _cursor = 0;            /* frames below have been handed out or set aside */
  size_t _back = 0;              /* single pages are taken downward from here */
  std::vector<size_t> _spare;    /* set aside while looking for a run */
  size_t _part = SIZE_MAX;       /* page being packed with small buffers */
  size_t _part_used = 0;
  size_t _used_pages = 0;
};

bool enable_lock_memory_privilege();
