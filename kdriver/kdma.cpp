// SPDX-License-Identifier: GPL-2.0-only
/* DmaPool (dma.h) in the kernel: every buffer is a WDF common buffer from the
 * device's DMA enabler, so addresses are bus (logical) addresses that respect
 * the IOMMU / Kernel DMA Protection. The user-mode page-pool tricks are not
 * needed here; init() only checks the enabler, the diagnostics report what
 * was allocated. */
#include "plat.h"
#include "dma.h"

DmaPool::~DmaPool() {
  /* Common buffers are children of the enabler and go with the device; they
   * are released explicitly so a restart of the hardware layer frees them. */
  for (uintptr_t h : _frames) WdfObjectDelete(reinterpret_cast<WDFCOMMONBUFFER>(h));
  _frames.clear();
}

bool DmaPool::init(size_t, size_t, std::string &error) {
  if (!g_neko_card.dma) {
    error = "no DMA enabler";
    return false;
  }
  return true;
}

DmaBuf DmaPool::alloc(size_t size, size_t align) {
  DmaBuf b;
  if (!size || !g_neko_card.dma) return b;
  WDF_COMMON_BUFFER_CONFIG cfg;
  WDF_COMMON_BUFFER_CONFIG_INIT(&cfg, align > 1 ? static_cast<ULONG>(align - 1) : 0);
  WDFCOMMONBUFFER cb;
  if (!NT_SUCCESS(WdfCommonBufferCreateWithConfig(g_neko_card.dma, size, &cfg, WDF_NO_OBJECT_ATTRIBUTES, &cb)))
    return b;
  b.va = static_cast<uint8_t *>(WdfCommonBufferGetAlignedVirtualAddress(cb));
  b.pa = static_cast<uint64_t>(WdfCommonBufferGetAlignedLogicalAddress(cb).QuadPart);
  b.size = size;
  RtlZeroMemory(b.va, size);
  _frames.push_back(reinterpret_cast<uintptr_t>(cb));
  _used_pages += (size + kPageBytes - 1) / kPageBytes;
  return b;
}

DmaBuf DmaPool::alloc_page() { return alloc(kPageBytes, kPageBytes); }

void DmaPool::trim() {}
size_t DmaPool::longest_run() const { return 0; }
size_t DmaPool::runs_of(size_t) const { return 0; }
/* Kernel buffers are physically contiguous already: nothing to grow or check. */
bool DmaPool::grow(size_t, std::string &error) {
  error = "the kernel pool does not grow";
  return false;
}
bool DmaPool::fits(std::vector<size_t>) const { return true; }
uint64_t DmaPool::available_bytes() { return 0; }

bool enable_lock_memory_privilege() { return true; }
