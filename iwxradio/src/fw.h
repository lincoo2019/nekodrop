// SPDX-License-Identifier: GPL-2.0-only
/* The iwlwifi firmware file (TLV format) as the 22000 family loads it: one
 * "regular" image whose sections the card fetches itself through the context
 * info, plus the capability, API and command-version tables the driver
 * consults. Parsing follows itlwm's iwx_read_firmware. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FwSection {
  uint32_t devoff = 0;
  const uint8_t *data = nullptr;
  size_t len = 0;
};

struct FwCmdVersion {
  uint8_t cmd, group, cmd_ver, notif_ver;
};

struct Firmware {
  std::vector<uint8_t> raw;
  std::string version;
  std::vector<FwSection> regular; /* LMAC, separator, UMAC, separator, paging */
  uint32_t phy_config = 0;
  uint32_t calib_event_trigger = 0, calib_flow_trigger = 0;
  uint8_t ucode_api[16] = {};  /* bit per IWX_UCODE_TLV_API_* */
  uint8_t capa[24] = {};       /* bit per IWX_UCODE_TLV_CAPA_* (API 100 uses bit 128) */
  std::vector<FwCmdVersion> cmd_versions;
  /* AX210/Bz families: the image loader the card runs before the firmware, and
   * optional embedded PNVM data (TLV 74, present in BE200 API 100). */
  const uint8_t *iml = nullptr;
  size_t iml_len = 0;
  const uint8_t *pnvm = nullptr;
  size_t pnvm_len = 0;

  bool load(const std::string &path, std::string &error);
  bool has_api(unsigned bit) const;
  bool has_capa(unsigned bit) const;
  /* IWX_FW_CMD_VER_UNKNOWN (99) when the firmware does not say. */
  uint8_t cmd_version(uint8_t group, uint8_t cmd) const;
  uint8_t notif_version(uint8_t group, uint8_t cmd) const;
};
