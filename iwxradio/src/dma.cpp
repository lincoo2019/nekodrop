// SPDX-License-Identifier: GPL-2.0-only
/* AWE use follows devourer's PcieTransportWin.cpp (winawdl fork, GPL-2.0);
 * the sorted small-page pool is this project's, for PCs with no free large
 * pages. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "dma.h"

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

DmaPool::~DmaPool() {
  if (_view) {
    for (size_t i = 0; i < _frames.size(); i++)
      if (_frames[i]) MapUserPhysicalPages(_view + i * kPageBytes, 1, nullptr);
    VirtualFree(_view, 0, MEM_RELEASE);
  }
  std::vector<ULONG_PTR> held;
  for (uintptr_t f : _frames)
    if (f) held.push_back(f);
  ULONG_PTR n = held.size();
  if (n) FreeUserPhysicalPages(GetCurrentProcess(), &n, held.data());
}

bool DmaPool::init(size_t pages, size_t min_pages, std::string &error) {
  if (!enable_lock_memory_privilege()) {
    error = "SeLockMemoryPrivilege is not available: run as SYSTEM, or elevated with "
            "'Lock pages in memory' granted";
    return false;
  }
  std::vector<ULONG_PTR> frames(pages);
  ULONG_PTR got = pages;
  if (!AllocateUserPhysicalPages(GetCurrentProcess(), &got, frames.data())) got = 0;
  frames.resize(got);
  if (got < min_pages) {
    error = "only " + std::to_string(got) + " lockable 4 KiB pages (" +
            std::to_string(min_pages) + " needed)";
    ULONG_PTR n = frames.size();
    if (n) FreeUserPhysicalPages(GetCurrentProcess(), &n, frames.data());
    return false;
  }
  std::sort(frames.begin(), frames.end());
  _frames.assign(frames.begin(), frames.end());
  _taken.assign(_frames.size(), false);
  _runs.reset(_frames);
  _view = static_cast<uint8_t *>(
      VirtualAlloc(nullptr, _frames.size() * kPageBytes, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE));
  if (!_view || !MapUserPhysicalPages(_view, frames.size(), frames.data())) {
    error = "mapping the DMA pages failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  return true;
}

bool DmaPool::grow(size_t pages, std::string &error) {
  if (_used_pages || !_reserved.empty() || !_view) {
    error = "the DMA pool can only grow before it is used";
    return false;
  }
  std::vector<ULONG_PTR> more(pages);
  ULONG_PTR got = pages;
  if (!AllocateUserPhysicalPages(GetCurrentProcess(), &got, more.data()) || !got) {
    error = "no more lockable pages (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  more.resize(got);
  // Unmap the old view; the frames stay locked and join the new, larger one.
  MapUserPhysicalPages(_view, _frames.size(), nullptr);
  VirtualFree(_view, 0, MEM_RELEASE);
  _view = nullptr;
  std::vector<ULONG_PTR> all(_frames.begin(), _frames.end());
  all.insert(all.end(), more.begin(), more.end());
  std::sort(all.begin(), all.end());
  _frames.assign(all.begin(), all.end());
  _taken.assign(_frames.size(), false);
  _runs.reset(_frames);
  _view = static_cast<uint8_t *>(
      VirtualAlloc(nullptr, all.size() * kPageBytes, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE));
  if (!_view || !MapUserPhysicalPages(_view, all.size(), all.data())) {
    error = "mapping the grown DMA pool failed (" + std::to_string(GetLastError()) + ")";
    if (_view) VirtualFree(_view, 0, MEM_RELEASE);
    _view = nullptr;
    return false;
  }
  return true;
}

bool DmaPool::fits(std::vector<size_t> runs) const {
  return _view && _runs.can_reserve(runs);
}

bool DmaPool::reserve_runs(std::vector<size_t> runs) {
  if (!_view || _used_pages || !_reserved.empty()) return false;
  auto planned = _runs.reserve(std::move(runs));
  if (!planned) return false;
  _reserved = std::move(*planned);
  return true;
}

uint64_t DmaPool::available_bytes() {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys : 0;
}

DmaBuf DmaPool::page_at(size_t index) {
  _taken[index] = true;
  _used_pages++;
  DmaBuf b;
  b.va = _view + index * kPageBytes;
  b.pa = static_cast<uint64_t>(_frames[index]) * kPageBytes;
  b.size = kPageBytes;
  std::memset(b.va, 0, kPageBytes);
  return b;
}

DmaBuf DmaPool::alloc_page() {
  if (!_view) return {};
  const auto page = _runs.take(1);
  return page ? page_at(*page) : DmaBuf{};
}

DmaBuf DmaPool::alloc(size_t size, size_t align) {
  if (!_view || !size || !align || align > kPageBytes || (align & (align - 1))) return {};
  if (size <= kPageBytes) {
    /* Pack small buffers into one page. */
    const size_t start = (_part_used + align - 1) & ~(align - 1);
    if (_part == SIZE_MAX || start + size > kPageBytes) {
      DmaBuf p = alloc_page();
      if (!p) return {};
      _part = static_cast<size_t>((p.va - _view) / kPageBytes);
      _part_used = 0;
    }
    const size_t at = (_part_used + align - 1) & ~(align - 1);
    _part_used = at + size;
    DmaBuf b;
    b.va = _view + _part * kPageBytes + at;
    b.pa = static_cast<uint64_t>(_frames[_part]) * kPageBytes + at;
    b.size = size;
    return b;
  }
  const size_t n = (size + kPageBytes - 1) / kPageBytes;
  std::optional<size_t> first;
  auto held = _reserved.find(n);
  if (held != _reserved.end()) {
    first = held->second;
    _reserved.erase(held);
  } else {
    first = _runs.take(n);
  }
  if (!first) return {};
  const size_t i = *first;
  DmaBuf b{_view + i * kPageBytes, uint64_t(_frames[i]) * kPageBytes, size};
  for (size_t j = 0; j < n; j++) _taken[i + j] = true;
  _used_pages += n;
  std::memset(b.va, 0, n * kPageBytes);
  return b;
}

void DmaPool::trim() {
  std::vector<ULONG_PTR> unused;
  for (size_t i = 0; i < _frames.size(); i++) {
    if (_taken[i] || !_frames[i]) continue;
    MapUserPhysicalPages(_view + i * kPageBytes, 1, nullptr);
    unused.push_back(_frames[i]);
    _frames[i] = 0;
    _taken[i] = true; /* never hand it out again */
  }
  _runs.clear();
  _reserved.clear();
  ULONG_PTR n = unused.size();
  if (n) FreeUserPhysicalPages(GetCurrentProcess(), &n, unused.data());
}

size_t DmaPool::longest_run() const {
  return _runs.longest();
}

size_t DmaPool::runs_of(size_t n) const {
  return _runs.count_runs(n);
}
