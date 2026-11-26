// SPDX-License-Identifier: GPL-2.0-only
/*
 * 32-bit DMA physical memory pool for MT7921/MT7922 WFDMA.
 *
 * Linux mt7921/pci.c sets DMA_BIT_MASK(32). This pool uses Windows AWE
 * (AllocateUserPhysicalPages) and filters for physical frames below 4 GiB
 * (PFN < 0x100000), sorting them so physically contiguous runs can be carved
 * for WFDMA descriptor rings while single 4 KiB pages back TX/RX buffers.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mtk {

struct DmaBuf {
  uint8_t *va = nullptr;
  uint32_t pa = 0; /* 32-bit bus address (< 4 GiB) */
  size_t size = 0;
  explicit operator bool() const { return va != nullptr; }
};

class DmaPool32 {
public:
  DmaPool32() = default;
  ~DmaPool32();
  DmaPool32(const DmaPool32 &) = delete;
  DmaPool32 &operator=(const DmaPool32 &) = delete;

  static constexpr size_t kPageBytes = 4096;
  static constexpr uintptr_t kMaxPfn32 = 0x100000u; /* 4 GiB / 4 KiB */

  /* Locks at least `min_pages` 4 KiB frames below 4 GiB (aiming for `want_pages`). */
  bool init(size_t want_pages, size_t min_pages, std::string &error);

  /* Allocates a zeroed, physically contiguous buffer aligned to `align` (<= 4096). */
  DmaBuf alloc(size_t size, size_t align = 256);

  /* Allocates one zeroed 4 KiB page. */
  DmaBuf alloc_page();

  /* Frees unused locked frames back to Windows. */
  void trim();

  size_t pages_locked() const { return _frames.size(); }
  size_t pages_used() const { return _used_pages; }

private:
  DmaBuf page_at(size_t index);

  uint8_t *_view = nullptr;
  std::vector<uintptr_t> _frames;
  std::vector<bool> _taken;
  size_t _cursor = 0;
  size_t _back = 0;
  std::vector<size_t> _spare;
  size_t _part = SIZE_MAX;
  size_t _part_used = 0;
  size_t _used_pages = 0;
};

} // namespace mtk
