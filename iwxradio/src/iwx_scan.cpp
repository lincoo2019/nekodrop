// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware (UMAC) scan, as itlwm's iwx_config_umac_scan and
 * iwx_umac_scan_v14: the firmware visits the channels itself, between the
 * station's and the listener's time slots, with adaptive dwell, and delivers
 * beacons and probe responses through the normal receive path; the end comes
 * as IWX_SCAN_COMPLETE_UMAC. Used by the kernel driver for Windows' scans so
 * the station and AWDL keep their channels while the firmware looks around.
 */
#include "iwx.h"

#include "plat.h"

#include <cstring>

#include "iwxreg.h"

namespace {
constexpr uint8_t kDwellActive = 10, kDwellPassive = 110;
constexpr uint8_t kAdwellLowBandAps = 2, kAdwellHighBandAps = 8, kAdwellSocialAps = 10;
constexpr uint16_t kAdwellBudgetFull = 300, kAdwellBudgetDirected = 100;
constexpr uint8_t kApsGoFriendly = 10, kApsSocial = 2;
} // namespace

bool IwxDevice::scan_config() {
  if (_scan_configured) return true;
  if (!_fw.has_api(IWX_UCODE_TLV_API_REDUCED_SCAN_CONFIG)) {
    _log("scan: firmware wants the legacy scan configuration; not supported");
    return false;
  }
  iwx_scan_config cfg{};
  cfg.tx_chains = htole32(valid_tx_ant());
  cfg.rx_chains = htole32(valid_rx_ant());
  if (!use_mld()) {
    const uint8_t addSta = cmd_version(IWX_LONG_GROUP, IWX_ADD_STA);
    if (addSta == IWX_FW_CMD_VER_UNKNOWN || addSta < 12) cfg.bcast_sta_id = IWX_AUX_STA_ID;
  } else {
    const uint8_t scanCfgVer = cmd_version(IWX_LONG_GROUP, IWX_SCAN_CFG_CMD);
    /* Linux iwl_mvm_config_scan: deprecated on SCAN_CFG_CMD >= 5 (API 100);
     * older MLD images want 0xff. */
    if (scanCfgVer != IWX_FW_CMD_VER_UNKNOWN && scanCfgVer < 5) cfg.bcast_sta_id = 0xff;
  }
  if (!nic_lock()) return false;
  const int err = send_cmd(iwx_cmd_id(IWX_SCAN_CFG_CMD, IWX_LONG_GROUP, 0), &cfg, sizeof(cfg), false, nullptr);
  nic_unlock();
  if (err) {
    _log("scan: configuration refused (" + std::to_string(err) + ")");
    return false;
  }
  _scan_configured = true;
  return true;
}

bool IwxDevice::umac_scan(const uint8_t *channels, size_t count, const uint8_t *ssid, size_t ssid_len,
                          const uint8_t address[6]) {
  if (!_open || _scan_running) return false;
  const uint8_t ver = cmd_version(IWX_LONG_GROUP, IWX_SCAN_REQ_UMAC);
  const bool ver_ok = use_mld() ? (ver >= 14 && ver <= 17) : (ver == 14);
  if (!ver_ok) {
    _log("scan: SCAN_REQ_UMAC version " + std::to_string(ver) + " not implemented");
    return false;
  }
  if (!scan_config()) return false;
  auto req = std::make_unique<iwx_scan_req_umac_v14>();
  std::memset(req.get(), 0, sizeof(*req));
  const bool active = ssid_len != 0;
  req->ooc_priority = htole32(IWX_SCAN_PRIORITY_EXT_6);
  auto &gp = req->scan_params.general_params;
  uint16_t flags = IWX_UMAC_SCAN_GEN_FLAGS_V2_PASS_ALL | IWX_UMAC_SCAN_GEN_FLAGS_V2_ADAPTIVE_DWELL;
  if (!active) flags |= IWX_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE;
  gp.flags = htole16(flags);
  gp.adwell_default_social_chn = kAdwellSocialAps;
  gp.adwell_default_2g = kAdwellLowBandAps;
  gp.adwell_default_5g = kAdwellHighBandAps;
  gp.adwell_max_budget = htole16(active ? kAdwellBudgetDirected : kAdwellBudgetFull);
  gp.scan_priority = htole32(IWX_SCAN_PRIORITY_EXT_6);
  /* Associated or listening: leave the home channel for short slices. */
  const uint32_t slice = htole32(_sta_assoc || _queue_enabled ? 120 : 0);
  gp.max_out_of_time[IWX_SCAN_LB_LMAC_IDX] = slice;
  gp.suspend_time[IWX_SCAN_LB_LMAC_IDX] = slice;
  if (_fw.has_capa(IWX_UCODE_TLV_CAPA_CDB_SUPPORT)) {
    gp.max_out_of_time[IWX_SCAN_HB_LMAC_IDX] = slice;
    gp.suspend_time[IWX_SCAN_HB_LMAC_IDX] = slice;
  }
  gp.active_dwell[IWX_SCAN_LB_LMAC_IDX] = kDwellActive;
  gp.passive_dwell[IWX_SCAN_LB_LMAC_IDX] = kDwellPassive;
  gp.active_dwell[IWX_SCAN_HB_LMAC_IDX] = kDwellActive;
  gp.passive_dwell[IWX_SCAN_HB_LMAC_IDX] = kDwellPassive;
  req->scan_params.periodic_params.schedule[0].interval = 0;
  req->scan_params.periodic_params.schedule[0].iter_count = 1;

  /* Probe request template: broadcast, SSID filled by the firmware, rates. */
  auto &preq = req->scan_params.probe_params.preq;
  uint8_t *f = preq.buf;
  f[0] = 0x40;
  std::memset(f + 4, 0xff, 6);
  std::memcpy(f + 10, address, 6);
  std::memset(f + 16, 0xff, 6);
  size_t n = 24;
  f[n++] = 0; /* SSID: the firmware inserts it */
  f[n++] = 0;
  preq.mac_header.offset = 0;
  preq.mac_header.len = htole16(static_cast<uint16_t>(n));
  static const uint8_t rates24[] = {1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6c};
  static const uint8_t rates5[] = {1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
  preq.band_data[0].offset = htole16(static_cast<uint16_t>(n));
  std::memcpy(f + n, rates24, sizeof(rates24));
  n += sizeof(rates24);
  preq.band_data[0].len = htole16(sizeof(rates24));
  preq.band_data[1].offset = htole16(static_cast<uint16_t>(n));
  std::memcpy(f + n, rates5, sizeof(rates5));
  n += sizeof(rates5);
  preq.band_data[1].len = htole16(sizeof(rates5));
  preq.common_data.offset = htole16(static_cast<uint16_t>(n));
  preq.common_data.len = 0;
  if (active) {
    auto &d = req->scan_params.probe_params.direct_scan[0];
    d.id = 0;
    d.len = static_cast<uint8_t>(ssid_len > 32 ? 32 : ssid_len);
    std::memcpy(d.ssid, ssid, d.len);
  }

  auto &cp = req->scan_params.channel_params;
  cp.flags = IWX_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER;
  cp.n_aps_override[0] = kApsGoFriendly;
  cp.n_aps_override[1] = kApsSocial;
  const bool extChan = _fw.has_api(IWX_UCODE_TLV_API_SCAN_EXT_CHAN_VER);
  uint8_t used = 0;
  for (size_t i = 0; i < count && used < IWX_SCAN_MAX_NUM_CHANS_V3; i++) {
    const uint8_t ch = channels[i];
    auto &c = cp.channel_config[used];
    uint32_t chan_flags = active ? 1u : 0u; /* probe for SSID 0 */
    if (use_mld() && ver >= 17) {
      /* SCAN_REQ_UMAC v17 (same struct size as v14): band moves to flags[31:30]
       * and v2.band becomes v5.psd_20 (0 for 2.4/5 GHz). */
      const uint32_t band = ch <= 14 ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
      c.v2.channel_num = ch;
      c.v2.band = 0;
      c.v2.iter_count = 1;
      c.v2.iter_interval = 0;
      chan_flags |= band << IWX_CHAN_CFG_FLAGS_BAND_POS;
    } else if (extChan) {
      c.v2.channel_num = ch;
      c.v2.band = ch <= 14 ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
      c.v2.iter_count = 1;
      c.v2.iter_interval = 0;
    } else {
      c.v1.channel_num = ch;
      c.v1.iter_count = 1;
      c.v1.iter_interval = 0;
    }
    c.flags = htole32(chan_flags);
    used++;
  }
  cp.count = used;
  if (!nic_lock()) return false;
  _scan_succeeded = false;
  _scan_running = true; /* send_cmd can dispatch firmware notifications while waiting */
  const int err = send_cmd(iwx_cmd_id(IWX_SCAN_REQ_UMAC, IWX_LONG_GROUP, 0), req.get(), sizeof(*req), false, nullptr);
  nic_unlock();
  if (err) {
    _scan_running = false;
    _log("scan: request refused (" + std::to_string(err) + ")");
    return false;
  }
  return true;
}

void IwxDevice::scan_abort() {
  if (!_scan_running) return;
  iwx_umac_scan_abort a{};
  a.uid = 0;
  if (nic_lock()) {
    send_cmd(iwx_cmd_id(IWX_SCAN_ABORT_UMAC, IWX_LONG_GROUP, 0), &a, sizeof(a), false, nullptr);
    nic_unlock();
  }
  /* IWX_SCAN_COMPLETE_UMAC still follows and clears _scan_running. */
}
