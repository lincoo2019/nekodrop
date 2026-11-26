// SPDX-License-Identifier: GPL-2.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>

#include <cwchar>
#include <vector>

#include "proxy.h"
#include "regproxy_ioctl.h"

namespace {

uint16_t id_field(const wchar_t *instance, const wchar_t *key) {
  const wchar_t *at = std::wcsstr(instance, key);
  return at ? static_cast<uint16_t>(std::wcstoul(at + std::wcslen(key), nullptr, 16))
            : 0;
}

/* SUBSYS_40708086: the subsystem device id is the high half. */
uint16_t subsystem_device(const wchar_t *instance) {
  const wchar_t *at = std::wcsstr(instance, L"SUBSYS_");
  if (!at) return 0;
  const unsigned long both = std::wcstoul(at + 7, nullptr, 16);
  return static_cast<uint16_t>(both >> 16);
}

} // namespace

RegProxy::~RegProxy() {
  if (_handle) CloseHandle(_handle);
}

bool RegProxy::open(std::string &error) {
  const GUID *iface = &GUID_DEVINTERFACE_WINAWDL_REGPROXY;
  HDEVINFO set = SetupDiGetClassDevsW(iface, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) {
    error = "device enumeration failed";
    return false;
  }
  HANDLE handle = INVALID_HANDLE_VALUE;
  SP_DEVICE_INTERFACE_DATA data{};
  data.cbSize = sizeof(data);
  if (SetupDiEnumDeviceInterfaces(set, nullptr, iface, 0, &data)) {
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &size, nullptr);
    std::vector<uint8_t> storage(size);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA devinfo{};
    devinfo.cbSize = sizeof(devinfo);
    wchar_t instance[MAX_DEVICE_ID_LEN] = {};
    if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, size, nullptr, &devinfo) &&
        SetupDiGetDeviceInstanceIdW(set, &devinfo, instance, MAX_DEVICE_ID_LEN, nullptr)) {
      _vendor = id_field(instance, L"VEN_");
      _device = id_field(instance, L"DEV_");
      _subsystem = subsystem_device(instance);
      handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
  }
  const DWORD open_error = GetLastError();
  SetupDiDestroyDeviceInfoList(set);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "register proxy not openable (error " + std::to_string(open_error) +
            "): is the driver bound to the card, the process elevated, and no "
            "other radio running?";
    return false;
  }
  REGPROXY_INFO info{};
  DWORD returned = 0;
  if (!DeviceIoControl(handle, IOCTL_REGPROXY_GET_INFO, nullptr, 0, &info, sizeof(info),
                       &returned, nullptr) ||
      info.version != REGPROXY_VERSION) {
    error = "register proxy GET_INFO failed or the driver is another version";
    CloseHandle(handle);
    return false;
  }
  _handle = handle;
  _bar_len = info.bar_len;
  return true;
}

bool RegProxy::batch(std::vector<RegOp> &ops) {
  if (!_handle || ops.empty()) return false;
  std::vector<REGPROXY_OP> wire(ops.size());
  for (size_t i = 0; i < ops.size(); i++) {
    wire[i].offset = ops[i].offset;
    wire[i].value = ops[i].value;
    wire[i].width = 4;
    wire[i].kind = ops[i].kind;
  }
  const DWORD bytes = static_cast<DWORD>(wire.size() * sizeof(REGPROXY_OP));
  DWORD returned = 0;
  if (!DeviceIoControl(_handle, IOCTL_REGPROXY_BATCH, wire.data(), bytes, wire.data(), bytes,
                       &returned, nullptr)) {
    _failed = true;
    return false;
  }
  for (size_t i = 0; i < ops.size(); i++) ops[i].value = wire[i].value;
  return true;
}

uint32_t RegProxy::read32(uint32_t off) {
  std::vector<RegOp> ops{{off, 0, REGPROXY_READ}};
  return batch(ops) ? ops[0].value : 0xFFFFFFFFu;
}

void RegProxy::write32(uint32_t off, uint32_t value) {
  std::vector<RegOp> ops{{off, value, REGPROXY_WRITE}};
  batch(ops);
}

void RegProxy::write8(uint32_t off, uint8_t value) {
  if (!_handle) return;
  REGPROXY_OP op{};
  op.offset = off;
  op.value = value;
  op.width = 1;
  op.kind = REGPROXY_WRITE;
  DWORD returned = 0;
  if (!DeviceIoControl(_handle, IOCTL_REGPROXY_BATCH, &op, sizeof(op), &op, sizeof(op),
                       &returned, nullptr))
    _failed = true;
}

void RegProxy::set_bits(uint32_t off, uint32_t bits) {
  std::vector<RegOp> ops{{off, bits, REGPROXY_SET_BITS}};
  batch(ops);
}

void RegProxy::clear_bits(uint32_t off, uint32_t bits) {
  std::vector<RegOp> ops{{off, bits, REGPROXY_CLEAR_BITS}};
  batch(ops);
}

bool RegProxy::set_close_ops(const std::vector<RegOp> &ops) {
  if (!_handle) return false;
  std::vector<REGPROXY_OP> wire(ops.size());
  for (size_t i = 0; i < ops.size(); i++) {
    wire[i].offset = ops[i].offset;
    wire[i].value = ops[i].value;
    wire[i].width = 4;
    wire[i].kind = ops[i].kind;
  }
  DWORD returned = 0;
  return DeviceIoControl(_handle, IOCTL_REGPROXY_SET_CLOSE_OPS,
                         wire.empty() ? nullptr : wire.data(),
                         static_cast<DWORD>(wire.size() * sizeof(REGPROXY_OP)), nullptr, 0,
                         &returned, nullptr) != 0;
}
