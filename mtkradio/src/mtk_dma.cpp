// SPDX-License-Identifier: GPL-2.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "mtk_dma.h"

namespace mtk {

namespace {

bool enable_lock_memory_privilege() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    return false;
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  const bool found =
      LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &privileges.Privileges[0].Luid);
  if (found) AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
  const bool enabled = found && GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return enabled;
}

} // namespace

DmaPool32::~DmaPool32() {
  if (_view) {
    for (size_t i = 0; i < _frames.size(); i++) {
      if (_frames[i]) MapUserPhysicalPages(_view + i * kPageBytes, 1, nullptr);
    }
    VirtualFree(_view, 0, MEM_RELEASE);
  }
  std::vector<ULONG_PTR> held;
  held.reserve(_frames.size());
  for (uintptr_t f : _frames) {
    if (f) held.push_back(static_cast<ULONG_PTR>(f));
  }
  ULONG_PTR n = held.size();
  if (n) FreeUserPhysicalPages(GetCurrentProcess(), &n, held.data());
}

bool DmaPool32::init(size_t want_pages, size_t min_pages, std::string &error) {
  if (!enable_lock_memory_privilege()) {
    error = "SeLockMemoryPrivilege is not available: run as SYSTEM or elevated with "
            "'Lock pages in memory' granted";
    return false;
  }

  std::vector<ULONG_PTR> low_frames;
  std::vector<ULONG_PTR> high_frames;
  low_frames.reserve(want_pages);

  /* Probe in batches, holding high (>= 4 GiB) frames temporarily so Windows hands
   * us fresh physical frames on subsequent calls, then releasing high frames.
   * On a freshly booted 16-64 GiB PC, free pages at the head of the list are
   * often above 4 GiB, so allow scanning up to 60% of available RAM. */
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  size_t max_high_pages = 262144; /* 1 GiB fallback */
  if (GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys > (512ULL << 20)) {
    max_high_pages = static_cast<size_t>((ms.ullAvailPhys * 85 / 100) / kPageBytes);
  }

  constexpr size_t kBatchPages = 65536; /* 256 MiB per batch */
  std::vector<ULONG_PTR> batch(kBatchPages);

  for (int round = 0; round < 256 && low_frames.size() < want_pages; round++) {
    ULONG_PTR got = kBatchPages;
    if (!AllocateUserPhysicalPages(GetCurrentProcess(), &got, batch.data()) || got == 0)
      break;
    for (ULONG_PTR i = 0; i < got; i++) {
      if (batch[i] < kMaxPfn32)
        low_frames.push_back(batch[i]);
      else
        high_frames.push_back(batch[i]);
    }
    if (high_frames.size() >= max_high_pages)
      break;
  }

  /* Release all >= 4 GiB frames immediately. */
  if (!high_frames.empty()) {
    ULONG_PTR n = high_frames.size();
    FreeUserPhysicalPages(GetCurrentProcess(), &n, high_frames.data());
    high_frames.clear();
  }

  if (low_frames.size() < min_pages) {
    error = "only " + std::to_string(low_frames.size()) +
            " lockable 4 KiB pages below 4 GiB (" + std::to_string(min_pages) + " needed)";
    if (!low_frames.empty()) {
      ULONG_PTR n = low_frames.size();
      FreeUserPhysicalPages(GetCurrentProcess(), &n, low_frames.data());
    }
    return false;
  }

  std::sort(low_frames.begin(), low_frames.end());
  _frames.assign(low_frames.begin(), low_frames.end());
  _taken.assign(_frames.size(), false);
  _back = _frames.size();

  _view = static_cast<uint8_t *>(
      VirtualAlloc(nullptr, _frames.size() * kPageBytes, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE));
  if (!_view || !MapUserPhysicalPages(_view, low_frames.size(), low_frames.data())) {
    error = "mapping 32-bit DMA pages failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  return true;
}

DmaBuf DmaPool32::page_at(size_t index) {
  DmaBuf b;
  b.va = _view + index * kPageBytes;
  b.pa = static_cast<uint32_t>(_frames[index] * kPageBytes);
  b.size = kPageBytes;
  std::memset(b.va, 0, kPageBytes);
  return b;
}

DmaBuf DmaPool32::alloc_page() {
  if (!_spare.empty()) {
    const size_t i = _spare.back();
    _spare.pop_back();
    _taken[i] = true;
    _used_pages++;
    return page_at(i);
  }
  while (_back > _cursor) {
    const size_t i = --_back;
    if (_taken[i] || !_frames[i]) continue;
    _taken[i] = true;
    _used_pages++;
    return page_at(i);
  }
  return {};
}

DmaBuf DmaPool32::alloc(size_t size, size_t align) {
  if (!size || !_view) return {};
  if (align == 0) align = 1;

  /* Small buffers packed into a shared page. */
  if (size <= kPageBytes / 2 && align <= kPageBytes) {
    if (_part != SIZE_MAX) {
      const size_t off = (_part_used + align - 1) & ~(align - 1);
      if (off + size <= kPageBytes) {
        DmaBuf b;
        b.va = _view + _part * kPageBytes + off;
        b.pa = static_cast<uint32_t>(_frames[_part] * kPageBytes + off);
        b.size = size;
        std::memset(b.va, 0, size);
        _part_used = off + size;
        return b;
      }
    }
    DmaBuf p = alloc_page();
    if (!p) return {};
    _part = static_cast<size_t>(p.va - _view) / kPageBytes;
    _part_used = size;
    p.size = size;
    return p;
  }

  const size_t need = (size + kPageBytes - 1) / kPageBytes;
  if (need == 1) {
    DmaBuf p = alloc_page();
    if (p) p.size = size;
    return p;
  }

  /* Find a physically contiguous run of `need` pages. */
  while (_cursor + need <= _back) {
    size_t run = 1;
    while (run < need && _cursor + run < _back &&
           !_taken[_cursor + run] && _frames[_cursor + run] == _frames[_cursor + run - 1] + 1) {
      run++;
    }
    if (run == need && !_taken[_cursor]) {
      const size_t start = _cursor;
      for (size_t k = 0; k < need; k++) _taken[start + k] = true;
      _cursor += need;
      _used_pages += need;
      DmaBuf b;
      b.va = _view + start * kPageBytes;
      b.pa = static_cast<uint32_t>(_frames[start] * kPageBytes);
      b.size = size;
      std::memset(b.va, 0, need * kPageBytes);
      return b;
    }
    for (size_t k = 0; k < run; k++) {
      if (!_taken[_cursor + k]) _spare.push_back(_cursor + k);
    }
    _cursor += run;
  }
  return {};
}

void DmaPool32::trim() {
  if (!_view) return;
  std::vector<ULONG_PTR> drop;
  for (size_t i = 0; i < _frames.size(); i++) {
    if (_taken[i] || !_frames[i]) continue;
    MapUserPhysicalPages(_view + i * kPageBytes, 1, nullptr);
    drop.push_back(static_cast<ULONG_PTR>(_frames[i]));
    _frames[i] = 0;
  }
  _spare.clear();
  _cursor = _back = 0;
  if (!drop.empty()) {
    ULONG_PTR n = drop.size();
    FreeUserPhysicalPages(GetCurrentProcess(), &n, drop.data());
  }
}

} // namespace mtk
