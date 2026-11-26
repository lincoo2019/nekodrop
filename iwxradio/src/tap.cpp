// SPDX-License-Identifier: GPL-2.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winioctl.h>

#include <cstring>

#include "tap.h"

namespace {

/* tap-windows6's control codes (tap-windows.h). */
constexpr DWORD tap_ctl(DWORD n) { return CTL_CODE(FILE_DEVICE_UNKNOWN, n, METHOD_BUFFERED, FILE_ANY_ACCESS); }
const DWORD kTapGetMac = tap_ctl(1);
const DWORD kTapSetMediaStatus = tap_ctl(6);

const char kAdapterClass[] =
    "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
const char kConnections[] =
    "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}";

std::string reg_string(HKEY key, const char *value) {
  char buf[256] = {};
  DWORD size = sizeof(buf) - 1, type = 0;
  if (RegQueryValueExA(key, value, nullptr, &type, reinterpret_cast<BYTE *>(buf), &size) != ERROR_SUCCESS ||
      type != REG_SZ)
    return {};
  return buf;
}

std::string connection_name(const std::string &guid) {
  HKEY key;
  const std::string path = std::string(kConnections) + "\\" + guid + "\\Connection";
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
  std::string name = reg_string(key, "Name");
  RegCloseKey(key);
  return name;
}

/* DeviceIoControl on the overlapped handle, waited for (Codex review #22:
 * a null OVERLAPPED on an overlapped handle is outside the API contract). */
bool ioctl_wait(HANDLE h, DWORD code, void *in, DWORD in_len, void *out, DWORD out_len, DWORD *got) {
  OVERLAPPED ov{};
  ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return false;
  DWORD n = 0;
  bool ok = DeviceIoControl(h, code, in, in_len, out, out_len, &n, &ov) != 0;
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(ov.hEvent, 2000) == WAIT_OBJECT_0) {
      ok = GetOverlappedResult(h, &ov, &n, FALSE) != 0;
    } else {
      CancelIoEx(h, &ov);
      GetOverlappedResult(h, &ov, &n, TRUE);
      ok = false;
    }
  }
  CloseHandle(ov.hEvent);
  if (got) *got = n;
  return ok;
}

} // namespace

TapAdapter::~TapAdapter() {
  if (_handle) {
    /* The read may have been started by another thread: cancel it by its
     * OVERLAPPED and wait before its buffer and event go (Codex review #21). */
    if (_read_pending) {
      auto *ov = reinterpret_cast<OVERLAPPED *>(_read_ov);
      DWORD n = 0;
      CancelIoEx(_handle, ov);
      GetOverlappedResult(_handle, ov, &n, TRUE);
      _read_pending = false;
    }
    CloseHandle(_handle);
  }
  if (_read_event) CloseHandle(_read_event);
  if (_write_event) CloseHandle(_write_event);
}

bool TapAdapter::open(const std::string &wanted, std::string &error) {
  HKEY cls;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kAdapterClass, 0, KEY_READ, &cls) != ERROR_SUCCESS) {
    error = "cannot read the network adapter class";
    return false;
  }
  /* Try every matching tap0901 entry: a removed phantom adapter can leave a
   * stale registry entry ahead of a live one and fail with ERROR_FILE_NOT_FOUND
   * (issue #20). */
  HANDLE h = INVALID_HANDLE_VALUE;
  std::string last_path;
  DWORD last_err = 0;
  for (DWORD i = 0;; i++) {
    char sub[256];
    DWORD len = sizeof(sub);
    if (RegEnumKeyExA(cls, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY key;
    if (RegOpenKeyExA(cls, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
    const std::string component = reg_string(key, "ComponentId");
    const std::string id = reg_string(key, "NetCfgInstanceId");
    RegCloseKey(key);
    if (component != "tap0901" || id.empty()) continue;
    const std::string name = connection_name(id);
    if (!wanted.empty() && wanted != id && wanted != name) continue;
    const std::string path = "\\\\.\\Global\\" + id + ".tap";
    HANDLE cand = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, nullptr);
    if (cand != INVALID_HANDLE_VALUE) {
      h = cand;
      _name = name;
      break;
    }
    last_path = path;
    last_err = GetLastError();
  }
  RegCloseKey(cls);
  if (h == INVALID_HANDLE_VALUE) {
    if (last_path.empty()) {
      error = "no TAP-Windows (tap0901) adapter found: install tap-windows6";
    } else if (last_err == ERROR_FILE_NOT_FOUND) {
      error = "cannot open " + last_path +
              " (error 2: TAP adapter is not present; reinstall nekodrop to recreate it)";
    } else {
      error = "cannot open " + last_path + " (error " + std::to_string(last_err) +
              "): is another program using the TAP adapter?";
    }
    return false;
  }
  DWORD got = 0;
  if (!ioctl_wait(h, kTapGetMac, _mac.data(), 6, _mac.data(), 6, &got) || got != 6) {
    CloseHandle(h);
    error = "the TAP adapter did not give its MAC address";
    return false;
  }
  _handle = h;
  _read_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  _write_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  _read_buf.resize(2048);
  return true;
}

bool TapAdapter::set_connected(bool up) {
  ULONG status = up ? 1 : 0;
  DWORD got = 0;
  return _handle && ioctl_wait(_handle, kTapSetMediaStatus, &status, sizeof(status), &status,
                               sizeof(status), &got);
}

bool TapAdapter::read(std::vector<uint8_t> &frame, unsigned timeout_ms) {
  frame.clear();
  if (!_handle) return false;
  auto *ov = reinterpret_cast<OVERLAPPED *>(_read_ov);
  static_assert(sizeof(OVERLAPPED) <= sizeof(_read_ov), "overlapped storage");
  if (!_read_pending) {
    std::memset(ov, 0, sizeof(*ov));
    ov->hEvent = _read_event;
    ResetEvent(_read_event);
    DWORD got = 0;
    if (ReadFile(_handle, _read_buf.data(), static_cast<DWORD>(_read_buf.size()), &got, ov)) {
      frame.assign(_read_buf.begin(), _read_buf.begin() + got);
      return true;
    }
    if (GetLastError() != ERROR_IO_PENDING) return false;
    _read_pending = true;
  }
  if (WaitForSingleObject(_read_event, timeout_ms) != WAIT_OBJECT_0) return true;
  DWORD got = 0;
  _read_pending = false;
  if (!GetOverlappedResult(_handle, ov, &got, FALSE)) return false;
  frame.assign(_read_buf.begin(), _read_buf.begin() + got);
  return true;
}

bool TapAdapter::write(const uint8_t *frame, size_t len) {
  if (!_handle) return false;
  /* The request owns its buffer, OVERLAPPED and event: a write that never
   * completes must not stop the radio's only thread (Codex review #8), and
   * a cancellation that never completes either (round 2 #14). After 500 ms
   * the write is cancelled; if even that has not finished a second later,
   * the request is left to the kernel (leaked) rather than freed under it. */
  struct Request {
    OVERLAPPED ov{};
    std::vector<uint8_t> buf;
  };
  auto *r = new Request;
  r->buf.assign(frame, frame + len);
  r->ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!r->ov.hEvent) {
    delete r;
    return false;
  }
  const auto done = [&](bool ok) {
    CloseHandle(r->ov.hEvent);
    delete r;
    return ok;
  };
  DWORD put = 0;
  if (WriteFile(_handle, r->buf.data(), static_cast<DWORD>(len), &put, &r->ov)) return done(true);
  if (GetLastError() != ERROR_IO_PENDING) return done(false);
  if (WaitForSingleObject(r->ov.hEvent, 500) == WAIT_OBJECT_0)
    return done(GetOverlappedResult(_handle, &r->ov, &put, FALSE) != 0);
  CancelIoEx(_handle, &r->ov);
  if (WaitForSingleObject(r->ov.hEvent, 1000) == WAIT_OBJECT_0) {
    GetOverlappedResult(_handle, &r->ov, &put, FALSE);
    return done(false);
  }
  return false; /* still owned by the kernel: leaked on purpose */
}
