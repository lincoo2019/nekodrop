// SPDX-License-Identifier: GPL-2.0-only
#include "fw.h"

#include "plat.h"

#include <cstdio>
#include <cstring>

#include "iwxreg.h"

namespace {

uint32_t u32_at(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

std::string version_string(uint32_t major, uint32_t minor, uint32_t api) {
  char buf[64];
  if (major >= 35)
    plat_snprintf(buf, sizeof(buf), "%u.%08x.%u", major, minor, api);
  else
    plat_snprintf(buf, sizeof(buf), "%u.%u.%u", major, minor, api);
  return buf;
}

} // namespace

bool Firmware::has_api(unsigned bit) const {
  return bit < sizeof(ucode_api) * 8 && isset_u8(ucode_api, bit);
}

bool Firmware::has_capa(unsigned bit) const {
  return bit < sizeof(capa) * 8 && isset_u8(capa, bit);
}

uint8_t Firmware::cmd_version(uint8_t group, uint8_t cmd) const {
  for (const auto &v : cmd_versions)
    if (v.group == group && v.cmd == cmd) return v.cmd_ver;
  return IWX_FW_CMD_VER_UNKNOWN;
}

uint8_t Firmware::notif_version(uint8_t group, uint8_t cmd) const {
  for (const auto &v : cmd_versions)
    if (v.group == group && v.cmd == cmd) return v.notif_ver;
  return IWX_FW_CMD_VER_UNKNOWN;
}

bool Firmware::load(const std::string &path, std::string &error) {
  if (!plat_read_file(path, raw, error)) return false;
  const size_t got = raw.size();
  if (got != raw.size() || raw.size() < sizeof(iwx_tlv_ucode_header)) {
    error = "firmware file too short";
    return false;
  }
  const auto *hdr = reinterpret_cast<const iwx_tlv_ucode_header *>(raw.data());
  if (u32_at(raw.data()) != 0 || le32toh(hdr->magic) != IWX_TLV_UCODE_MAGIC) {
    error = "not an iwlwifi TLV firmware file";
    return false;
  }
  version = version_string(IWX_UCODE_MAJOR(le32toh(hdr->ver)), IWX_UCODE_MINOR(le32toh(hdr->ver)),
                           IWX_UCODE_API(le32toh(hdr->ver)));
  const uint8_t *data = hdr->data;
  size_t len = raw.size() - sizeof(*hdr);

  while (len >= sizeof(iwx_ucode_tlv)) {
    iwx_ucode_tlv tlv;
    std::memcpy(&tlv, data, sizeof(tlv));
    const size_t tlv_len = le32toh(tlv.length);
    const uint32_t type = le32toh(tlv.type);
    len -= sizeof(tlv);
    data += sizeof(tlv);
    if (len < tlv_len) {
      error = "firmware truncated in TLV " + std::to_string(type);
      return false;
    }
    switch (type) {
    case IWX_UCODE_TLV_SEC_RT:
      if (tlv_len < 4) {
        error = "short firmware section";
        return false;
      }
      regular.push_back({u32_at(data), data + 4, tlv_len - 4});
      break;
    case IWX_UCODE_TLV_PHY_SKU:
      if (tlv_len == 4) phy_config = u32_at(data);
      break;
    case IWX_UCODE_TLV_DEF_CALIB:
      if (tlv_len == 12 && u32_at(data) == IWX_UCODE_TYPE_REGULAR) {
        calib_flow_trigger = u32_at(data + 4);
        calib_event_trigger = u32_at(data + 8);
      }
      break;
    case IWX_UCODE_TLV_API_CHANGES_SET:
      if (tlv_len == sizeof(iwx_ucode_api)) {
        const uint32_t idx = u32_at(data), flags = u32_at(data + 4);
        for (unsigned i = 0; i < 32; i++)
          if ((flags >> i) & 1 && i + 32 * idx < sizeof(ucode_api) * 8)
            setbit_u8(ucode_api, i + 32 * idx);
      }
      break;
    case IWX_UCODE_TLV_ENABLED_CAPABILITIES:
      if (tlv_len == sizeof(iwx_ucode_capa)) {
        const uint32_t idx = u32_at(data), flags = u32_at(data + 4);
        for (unsigned i = 0; i < 32; i++)
          if ((flags >> i) & 1 && i + 32 * idx < sizeof(capa) * 8)
            setbit_u8(capa, i + 32 * idx);
      }
      break;
    case IWX_UCODE_TLV_FW_VERSION:
      if (tlv_len == 12)
        version = version_string(u32_at(data), u32_at(data + 4), u32_at(data + 8));
      break;
    case IWX_UCODE_TLV_IML:
      iml = data;
      iml_len = tlv_len;
      break;
    case IWX_UCODE_TLV_PNVM_DATA:
      pnvm = data;
      pnvm_len = tlv_len;
      break;
    case IWX_UCODE_TLV_CMD_VERSIONS:
      cmd_versions.clear();
      for (size_t off = 0; off + 4 <= tlv_len; off += 4)
        cmd_versions.push_back({data[off], data[off + 1], data[off + 2], data[off + 3]});
      break;
    default:
      break;
    }
    if (roundup(tlv_len, 4) > len) break;
    len -= roundup(tlv_len, 4);
    data += roundup(tlv_len, 4);
  }
  if (regular.empty()) {
    error = "firmware has no runtime image";
    return false;
  }
  return true;
}
