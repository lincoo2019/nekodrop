// SPDX-License-Identifier: GPL-2.0-only
/*
 * The station beside the AWDL listener: firmware side.
 *
 * The context layout is ForbiddenItlwm 0.6.0's coex mode turned around (there
 * the station is macOS's and the listener is added beside it; here the
 * listener comes first): MAC 1 is IWX_FW_MAC_TYPE_BSS_STA on TSF B, the access
 * point is firmware station 0, and its frames go through one queue on the
 * management TID, as itlwm sends every non-aggregated frame. On another
 * channel than the listener the station gets PHY 1 and binding 1 and the
 * firmware shares the air between the bindings; on the same channel it joins
 * binding 0. The commands follow itlwm's iwx_auth / iwx_run / iwx_set_key.
 */
#include "iwx.h"

#include "plat.h"

#include <cstring>

#include "iwxreg.h"

namespace {

constexpr unsigned kStaMac = 1;
constexpr unsigned kStaPhy = 1;
constexpr unsigned kStaBinding = 1;
constexpr unsigned kStaLink = 1;

/* EDCA defaults for a station (802.11-2016 table 9-137), per firmware AC. */
struct Edca {
  uint16_t cw_min, cw_max;
  uint8_t aifsn;
  uint16_t txop_32us;
  uint8_t fifo;
};
const Edca kEdca[IWX_AC_NUM] = {
    /* IWX_AC_BK */ {15, 1023, 7, 0, IWX_GEN2_EDCA_TX_FIFO_BK},
    /* IWX_AC_BE */ {15, 1023, 3, 0, IWX_GEN2_EDCA_TX_FIFO_BE},
    /* IWX_AC_VI */ {7, 15, 2, 94, IWX_GEN2_EDCA_TX_FIFO_VI},
    /* IWX_AC_VO */ {3, 7, 2, 47, IWX_GEN2_EDCA_TX_FIFO_VO},
};

} // namespace

int IwxDevice::sta_mac_cmd(uint32_t action, bool assoc) {
  iwx_mac_ctx_cmd cmd{};
  cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
  cmd.action = htole32(action);
  if (action == IWX_FW_CTXT_ACTION_REMOVE)
    return send_cmd(IWX_MAC_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
  cmd.mac_type = htole32(IWX_FW_MAC_TYPE_BSS_STA);
  cmd.tsf_id = htole32(IWX_TSF_ID_B);
  std::memcpy(cmd.node_addr, _sta.addr, 6);
  std::memcpy(cmd.bssid_addr, _sta.bssid, 6);
  cmd.cck_rates = htole32(_sta.channel <= 14 ? 0x0f : 0);
  cmd.ofdm_rates = htole32(0x15); /* 6, 12, 24: the mandatory set */
  cmd.short_slot = htole32(_sta.channel > 14 ? IWX_MAC_FLG_SHORT_SLOT : 0);
  for (unsigned ac = 0; ac < IWX_AC_NUM; ac++) {
    cmd.ac[ac].cw_min = htole16(kEdca[ac].cw_min);
    cmd.ac[ac].cw_max = htole16(kEdca[ac].cw_max);
    cmd.ac[ac].aifsn = kEdca[ac].aifsn;
    cmd.ac[ac].fifos_mask = static_cast<uint8_t>(1u << kEdca[ac].fifo);
    cmd.ac[ac].edca_txop = htole16(static_cast<uint16_t>(kEdca[ac].txop_32us * 32));
  }
  cmd.qos_flags = htole32(IWX_MAC_QOS_FLG_UPDATE_EDCA | (_sta.ht ? IWX_MAC_QOS_FLG_TGN : 0u));
  cmd.filter_flags = htole32(IWX_MAC_FILTER_ACCEPT_GRP | (assoc ? 0u : IWX_MAC_FILTER_IN_BEACON));
  if (assoc) {
    const uint32_t bi = _sta.beacon_int ? _sta.beacon_int : 100;
    cmd.sta.is_assoc = htole32(1);
    cmd.sta.bi = htole32(bi);
    cmd.sta.bi_reciprocal = htole32(iwx_reciprocal(bi));
    cmd.sta.dtim_interval = htole32(bi * (_sta_dtim ? _sta_dtim : 1));
    cmd.sta.dtim_reciprocal = htole32(iwx_reciprocal(bi * (_sta_dtim ? _sta_dtim : 1)));
    cmd.sta.listen_interval = htole32(10);
    cmd.sta.assoc_id = htole32(_sta_aid);
  }
  return send_cmd(IWX_MAC_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_sta_mac_cmd(uint32_t action, bool assoc) {
  iwx_mac_config_cmd cmd{};
  cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
  cmd.action = htole32(action);
  if (action == IWX_FW_CTXT_ACTION_REMOVE)
    return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_MAC_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
  cmd.mac_type = htole32(IWX_FW_MAC_TYPE_BSS_STA);
  std::memcpy(cmd.local_mld_addr, _sta.addr, 6);
  cmd.filter_flags = htole32(IWX_MAC_CFG_FILTER_ACCEPT_GRP | (assoc ? 0u : IWX_MAC_CFG_FILTER_ACCEPT_BEACON));
  if (assoc) {
    cmd.client.is_assoc = 1;
    cmd.client.assoc_id = htole16(_sta_aid);
  }
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_MAC_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_sta_link_add() {
  iwx_link_config_cmd cmd{};
  cmd.action = htole32(IWX_FW_CTXT_ACTION_ADD);
  cmd.link_id = htole32(kStaLink);
  cmd.mac_id = htole32(kStaMac);
  cmd.phy_id = htole32(IWX_FW_CTXT_INVALID);
  std::memcpy(cmd.local_link_addr, _sta.addr, 6);
  cmd.spec_link_id = 0;
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_sta_link_modify(bool active, uint32_t modify_mask) {
  iwx_link_config_cmd cmd{};
  cmd.action = htole32(IWX_FW_CTXT_ACTION_MODIFY);
  cmd.link_id = htole32(kStaLink);
  cmd.mac_id = htole32(kStaMac);
  cmd.phy_id = htole32(kStaPhy);
  std::memcpy(cmd.local_link_addr, _sta.addr, 6);
  cmd.modify_mask = htole32(modify_mask);
  cmd.active = htole32(active ? 1u : 0u);
  cmd.cck_rates = htole32(_sta.channel <= 14 ? 0x0fu : 0u);
  cmd.ofdm_rates = htole32(0x15u); /* 6, 12, 24: the mandatory set */
  cmd.short_slot = htole32(_sta.channel > 14 ? 1u : 0u);
  for (unsigned ac = 0; ac < IWX_AC_NUM; ac++) {
    cmd.ac[ac].cw_min = htole16(kEdca[ac].cw_min);
    cmd.ac[ac].cw_max = htole16(kEdca[ac].cw_max);
    cmd.ac[ac].aifsn = kEdca[ac].aifsn;
    cmd.ac[ac].fifos_mask = static_cast<uint8_t>(1u << kEdca[ac].fifo);
    cmd.ac[ac].edca_txop = htole16(static_cast<uint16_t>(kEdca[ac].txop_32us * 32));
  }
  cmd.qos_flags = htole32(IWX_MAC_QOS_FLG_UPDATE_EDCA | (_sta.ht ? IWX_MAC_QOS_FLG_TGN : 0u));
  const uint32_t bi = _sta.beacon_int ? _sta.beacon_int : 100;
  cmd.bi = htole32(bi);
  cmd.dtim_interval = htole32(bi * (_sta_dtim ? _sta_dtim : 1));
  cmd.spec_link_id = 0;
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_sta_link_remove() {
  iwx_link_config_cmd cmd{};
  cmd.action = htole32(IWX_FW_CTXT_ACTION_REMOVE);
  cmd.link_id = htole32(kStaLink);
  cmd.phy_id = htole32(IWX_FW_CTXT_INVALID);
  cmd.spec_link_id = 0;
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_sta_cfg_ap(bool assoc) {
  iwx_sta_cfg_cmd_v2 cmd{};
  cmd.sta_id = htole32(IWX_STATION_ID);
  cmd.link_id = htole32(kStaLink);
  std::memcpy(cmd.peer_mld_address, _sta.bssid, 6);
  std::memcpy(cmd.peer_link_address, _sta.bssid, 6);
  cmd.station_type = htole32(IWX_STATION_TYPE_PEER);
  if (assoc) cmd.assoc_id = htole32(_sta_aid);
  /* Linux iwl_mvm_mld_cfg_sta: set mfp before authorization when the firmware
   * advertises STA_EXP_MFP_SUPPORT, or whenever PMF is enabled. */
  if (_sta_mfp || (!assoc && _fw.has_capa(IWX_UCODE_TLV_CAPA_STA_EXP_MFP_SUPPORT)))
    cmd.mfp = htole32(1);
  if (assoc && (_sta.ht || _sta.vht)) {
    cmd.mimo = htole32(_sta.mimo ? 1u : 0u);
    cmd.tx_ampdu_max_size = htole32(3u); /* 64 KiB (exponent 3) */
    cmd.tx_ampdu_spacing = htole32(6u);  /* 8 us (density 6) */
  }
  const uint8_t ver = cmd_version(IWX_MAC_CONF_GROUP, IWX_STA_CONFIG_CMD);
  const size_t sz =
      (ver != IWX_FW_CMD_VER_UNKNOWN && ver > 1) ? sizeof(iwx_sta_cfg_cmd_v2) : sizeof(iwx_sta_cfg_cmd_v1);
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_CONFIG_CMD), &cmd, sz, false, nullptr);
}

int IwxDevice::mld_sec_key_remove(uint32_t key_id, uint32_t key_flags) {
  iwx_sec_key_cmd cmd{};
  cmd.action = htole32(IWX_FW_CTXT_ACTION_REMOVE);
  cmd.u.remove.sta_mask = htole32(1u << IWX_STATION_ID);
  cmd.u.remove.key_id = htole32(key_id);
  cmd.u.remove.key_flags = htole32(key_flags);
  return send_cmd(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_SEC_KEY_CMD), &cmd, sizeof(cmd), false, nullptr);
}

bool IwxDevice::sta_join(const StaParams &p) {
  if (use_mld()) {
    if (!_queue_enabled) return false; /* the listener comes first */
    sta_leave();
    _sta = p;
    _sta_shared_binding = false;
    if (!nic_lock()) return false;
    int err = phy_cmd(kStaPhy, p.channel, 20, IWX_FW_CTXT_ACTION_ADD);
    if (err) _log("station: MLD PHY context refused (" + std::to_string(err) + ")");
    else _sta_phy_added = true;
    if (!err) {
      err = mld_sta_mac_cmd(IWX_FW_CTXT_ACTION_ADD, false);
      if (err) _log("station: MLD MAC config refused (" + std::to_string(err) + ")");
      else _sta_mac_added = true;
    }
    if (!err) {
      err = mld_sta_link_add();
      if (err) _log("station: MLD link add refused (" + std::to_string(err) + ")");
      else _sta_link_added = true;
    }
    if (!err) {
      /* Bind link 1 to PHY 1 while inactive, then activate with rates + EDCA. */
      err = mld_sta_link_modify(false, 0);
      if (err) _log("station: MLD link PHY bind refused (" + std::to_string(err) + ")");
    }
    if (!err) {
      err = mld_sta_link_modify(true, IWX_LINK_CONTEXT_MODIFY_ACTIVE |
                                          IWX_LINK_CONTEXT_MODIFY_RATES_INFO |
                                          IWX_LINK_CONTEXT_MODIFY_QOS_PARAMS);
      if (err) _log("station: MLD link activate refused (" + std::to_string(err) + ")");
      else _sta_link_active = true;
    }
    if (!err) {
      err = mld_sta_cfg_ap(false);
      if (err) _log("station: MLD access point station refused (" + std::to_string(err) + ")");
      else _sta_sta_added = true;
    }
    if (!err) {
      err = enable_queue(_staq, IWX_STATION_ID, IWX_MGMT_TID);
      if (err) _log("station: MLD queue refused (" + std::to_string(err) + ")");
      else _sta_queue_enabled = true;
    }
    if (!err && _fw.has_capa(IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD)) {
      const uint8_t sp_ver = cmd_version(IWX_MAC_CONF_GROUP, IWX_SESSION_PROTECTION_CMD);
      iwx_session_prot_cmd sp{};
      sp.id_and_color = htole32((sp_ver != IWX_FW_CMD_VER_UNKNOWN && sp_ver >= 2)
                                    ? kStaLink
                                    : IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
      sp.action = htole32(IWX_FW_CTXT_ACTION_ADD);
      sp.conf_id = htole32(IWX_SESSION_PROTECT_CONF_ASSOC);
      sp.duration_tu = htole32(900);
      const int e = send_cmd(iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0), &sp,
                             sizeof(sp), false, nullptr);
      if (e) _log("station: MLD session protection refused (" + std::to_string(e) + "), joining anyway");
      else _sta_session_prot = true;
    }
    nic_unlock();
    if (err) {
      sta_leave();
      return false;
    }
    char b[96];
    plat_snprintf(b, sizeof(b), "station: joining %02x:%02x:%02x:%02x:%02x:%02x on channel %u (MLD)",
                  p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5], p.channel);
    _log(b);
    return true;
  }
  if (!_queue_enabled) return false; /* the listener comes first */
  sta_leave();
  _sta = p;
  /* Always its own PHY and binding: sharing the listener's binding left
   * authentication unanswered on a passive channel (hardware, 2026-11-17),
   * while a separate PHY on the same 5 GHz channel joins fine. */
  _sta_shared_binding = false;
  if (!nic_lock()) return false;
  int err = 0;
  if (!_sta_shared_binding) {
    err = phy_cmd(kStaPhy, p.channel, 20, IWX_FW_CTXT_ACTION_ADD);
    if (err) _log("station: PHY context refused (" + std::to_string(err) + ")");
    else _sta_phy_added = true;
  }
  if (!err) {
    err = sta_mac_cmd(IWX_FW_CTXT_ACTION_ADD, false);
    if (err) _log("station: MAC context refused (" + std::to_string(err) + ")");
    else _sta_mac_added = true;
  }
  if (!err) {
    if (_sta_shared_binding) {
      err = binding_cmd(IWX_FW_CTXT_ACTION_MODIFY);
    } else {
      const uint32_t mac = IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0);
      err = binding_for(kStaBinding, kStaPhy, &mac, 1, p.channel, IWX_FW_CTXT_ACTION_ADD);
    }
    if (err) _log("station: binding refused (" + std::to_string(err) + ")");
    else _sta_binding_added = true;
  }
  if (!err && !_fw.has_capa(IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA)) {
    /* Air time halves between two bindings. */
    iwx_time_quota_cmd q{};
    for (unsigned i = 0; i < IWX_MAX_BINDINGS; i++) q.quotas[i].id_and_color = htole32(IWX_FW_CTXT_INVALID);
    q.quotas[0].id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    if (_sta_shared_binding) {
      q.quotas[0].quota = htole32(IWX_MAX_QUOTA);
    } else {
      q.quotas[0].quota = htole32(IWX_MAX_QUOTA / 2);
      q.quotas[1].id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaBinding, 0));
      q.quotas[1].quota = htole32(IWX_MAX_QUOTA / 2);
    }
    err = send_cmd(IWX_TIME_QUOTA_CMD, &q, sizeof(q), false, nullptr);
    if (err) _log("station: time quota refused (" + std::to_string(err) + ")");
  }
  if (!err) {
    iwx_add_sta_cmd cmd{};
    cmd.sta_id = IWX_STATION_ID;
    cmd.station_type = IWX_STA_LINK;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
    std::memcpy(cmd.addr, p.bssid, 6);
    cmd.station_flags_msk = htole32(IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK);
    cmd.tid_disable_tx = htole16(0xffff);
    uint32_t status = IWX_ADD_STA_SUCCESS;
    err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
    if (!err && (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) err = EIO;
    if (err) _log("station: access point station refused (" + std::to_string(err) + ")");
    else _sta_sta_added = true;
  }
  if (!err) {
    err = enable_queue(_staq, IWX_STATION_ID, IWX_MGMT_TID);
    if (err) _log("station: queue refused (" + std::to_string(err) + ")");
    else _sta_queue_enabled = true;
  }
  if (!err && _fw.has_capa(IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD)) {
    /* Keep the radio on the access point's channel while joining
     * (iwx_schedule_protect_session: 900 TU, fragmentable). */
    iwx_session_prot_cmd sp{};
    sp.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
    sp.action = htole32(IWX_FW_CTXT_ACTION_ADD);
    sp.conf_id = htole32(IWX_SESSION_PROTECT_CONF_ASSOC);
    sp.duration_tu = htole32(900);
    const int e = send_cmd(iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0), &sp,
                           sizeof(sp), false, nullptr);
    if (e) _log("station: session protection refused (" + std::to_string(e) + "), joining anyway");
    else _sta_session_prot = true;
  }
  nic_unlock();
  if (err) {
    sta_leave();
    return false;
  }
  char b[96];
  plat_snprintf(b, sizeof(b), "station: joining %02x:%02x:%02x:%02x:%02x:%02x on channel %u%s",
                p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5], p.channel,
                _sta_shared_binding ? " (shared with AWDL)" : "");
  _log(b);
  return true;
}

bool IwxDevice::sta_associated(uint16_t aid, uint8_t dtim_period) {
  if (!_sta_mac_added) return false;
  _sta_aid = aid & 0x3fff;
  _sta_dtim = dtim_period ? dtim_period : 1;
  if (use_mld()) {
    if (!nic_lock()) return false;
    int err = 0;
    if (_sta.width > 20) {
      err = phy_cmd(kStaPhy, _sta.channel, _sta.width, IWX_FW_CTXT_ACTION_MODIFY);
      if (err) {
        _log("station: MLD wide PHY refused, staying at 20 MHz");
        _sta.width = 20;
        err = 0;
      }
    }
    if (!err)
      err = mld_sta_link_modify(true, IWX_LINK_CONTEXT_MODIFY_RATES_INFO |
                                          IWX_LINK_CONTEXT_MODIFY_QOS_PARAMS);
    if (!err) err = mld_sta_mac_cmd(IWX_FW_CTXT_ACTION_MODIFY, true);
    if (!err) err = mld_sta_cfg_ap(true);
    if (!err) {
      iwx_mac_power_cmd pm{};
      pm.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
      pm.keep_alive_seconds = htole16(25);
      send_cmd(IWX_MAC_PM_POWER_TABLE, &pm, sizeof(pm), false, nullptr);
    }
    nic_unlock();
    if (err) {
      _log("station: MLD association update refused (" + std::to_string(err) + ")");
      return false;
    }
    _sta_assoc = true;
    _log("station: associated, AID " + std::to_string(_sta_aid));
    return true;
  }
  if (!nic_lock()) return false;
  int err = 0;
  if (!_sta_shared_binding && _sta.width > 20) {
    err = phy_cmd(kStaPhy, _sta.channel, _sta.width, IWX_FW_CTXT_ACTION_MODIFY);
    if (err) {
      _log("station: wide PHY refused, staying at 20 MHz");
      _sta.width = 20;
      err = 0;
    }
  }
  err = sta_mac_cmd(IWX_FW_CTXT_ACTION_MODIFY, true);
  if (!err) {
    /* HT/VHT now that the AP agreed (iwx_add_sta_cmd update). */
    iwx_add_sta_cmd cmd{};
    cmd.sta_id = IWX_STATION_ID;
    cmd.station_type = IWX_STA_LINK;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
    cmd.add_modify = 1;
    uint32_t flags = 0, mask = IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK;
    if (_sta.ht || _sta.vht) {
      mask |= IWX_STA_FLG_MAX_AGG_SIZE_MSK | IWX_STA_FLG_AGG_MPDU_DENS_MSK;
      flags |= _sta.width >= 80 ? IWX_STA_FLG_FAT_EN_80MHZ
               : _sta.width >= 40 ? IWX_STA_FLG_FAT_EN_40MHZ
                                  : IWX_STA_FLG_FAT_EN_20MHZ;
      flags |= _sta.mimo ? IWX_STA_FLG_MIMO_EN_MIMO2 : IWX_STA_FLG_MIMO_EN_SISO;
      flags |= IWX_STA_FLG_MAX_AGG_SIZE_64K | IWX_STA_FLG_AGG_MPDU_DENS_8US;
    }
    cmd.station_flags = htole32(flags);
    cmd.station_flags_msk = htole32(mask);
    cmd.tid_disable_tx = htole16(0xffff);
    uint32_t status = IWX_ADD_STA_SUCCESS;
    err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
    if (!err && (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) err = EIO;
  }
  if (!err) {
    iwx_mac_power_cmd pm{};
    pm.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
    pm.keep_alive_seconds = htole16(25);
    send_cmd(IWX_MAC_PM_POWER_TABLE, &pm, sizeof(pm), false, nullptr);
  }
  nic_unlock();
  if (err) {
    _log("station: association update refused (" + std::to_string(err) + ")");
    return false;
  }
  _sta_assoc = true;
  _log("station: associated, AID " + std::to_string(_sta_aid));
  return true;
}

bool IwxDevice::sta_set_key(bool group, unsigned key_index, const uint8_t key[16]) {
  if (!_sta_sta_added) return false;
  if (use_mld()) {
    const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_SEC_KEY_CMD);
    if (ver == IWX_FW_CMD_VER_UNKNOWN || ver < 1) {
      _log("station: SEC_KEY_CMD not advertised by firmware");
      return false;
    }
    uint32_t flags = IWX_SEC_KEY_FLAG_CIPHER_CCMP;
    if (group) flags |= IWX_SEC_KEY_FLAG_MCAST_KEY;
    else if (_sta_mfp) flags |= IWX_SEC_KEY_FLAG_MFP;
    iwx_sec_key_cmd cmd{};
    cmd.action = htole32(IWX_FW_CTXT_ACTION_ADD);
    cmd.u.add.sta_mask = htole32(1u << IWX_STATION_ID);
    cmd.u.add.key_id = htole32(key_index);
    cmd.u.add.key_flags = htole32(flags);
    std::memcpy(cmd.u.add.key, key, 16);
    if (!nic_lock()) return false;
    if (!group && _sta_ptk_flags) {
      mld_sec_key_remove(key_index, _sta_ptk_flags);
      _sta_ptk_flags = 0;
    } else if (group && key_index < 4 && ((_sta_gtk_mask >> key_index) & 1)) {
      mld_sec_key_remove(key_index, flags);
      _sta_gtk_mask &= static_cast<uint8_t>(~(1u << key_index));
    }
    const int err =
        send_cmd(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_SEC_KEY_CMD), &cmd, sizeof(cmd), false, nullptr);
    nic_unlock();
    if (err) {
      _log("station: MLD key install refused (" + std::to_string(err) + ")");
      return false;
    }
    if (!group) _sta_ptk_flags = flags;
    else if (key_index < 4) _sta_gtk_mask |= static_cast<uint8_t>(1u << key_index);
    return true;
  }
  iwx_add_sta_key_cmd cmd{};
  cmd.common.key_flags = htole16(static_cast<uint16_t>(
      IWX_STA_KEY_FLG_CCM | IWX_STA_KEY_FLG_WEP_KEY_MAP |
      ((key_index << IWX_STA_KEY_FLG_KEYID_POS) & IWX_STA_KEY_FLG_KEYID_MSK) |
      (group ? IWX_STA_KEY_MULTICAST : 0)));
  cmd.common.key_offset = group ? 1 : 0;
  std::memcpy(cmd.common.key, key, 16);
  cmd.common.sta_id = IWX_STATION_ID;
  cmd.transmit_seq_cnt = 0;
  if (!nic_lock()) return false;
  /* The firmware answers with an ADD_STA status (Linux iwl_mvm_send_sta_key
   * accepts only ADD_STA_SUCCESS). A reply of another shape is taken as
   * success, as before, so an unexpected format cannot break key installs. */
  std::vector<uint8_t> resp;
  int err = send_cmd(IWX_ADD_STA_KEY, &cmd, sizeof(cmd), true, &resp);
  nic_unlock();
  if (!err && resp.size() >= sizeof(iwx_rx_packet) + sizeof(uint32_t)) {
    const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
    uint32_t status = 0;
    std::memcpy(&status, pkt->data, sizeof(status));
    status = le32toh(status);
    if (iwx_rx_packet_payload_len(pkt) == sizeof(iwx_cmd_response) &&
        (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) {
      _log("station: key install refused, status " + std::to_string(status));
      return false;
    }
  }
  if (err) _log("station: key install refused (" + std::to_string(err) + ")");
  return err == 0;
}

bool IwxDevice::sta_set_mfp(bool on) {
  if (!_sta_sta_added) return false;
  if (use_mld()) {
    const bool prev = _sta_mfp;
    _sta_mfp = on;
    if (!nic_lock()) {
      _sta_mfp = prev;
      return false;
    }
    const int err = mld_sta_cfg_ap(_sta_assoc);
    nic_unlock();
    if (err) _sta_mfp = prev;
    _log(!err ? std::string("station: management frame protection ") + (on ? "on" : "off")
              : "station: MLD management frame protection refused (" + std::to_string(err) + ")");
    return err == 0;
  }
  constexpr uint32_t kMfpEn = 1u << 9; /* STA_FLG_MFP_EN (iwlwifi) */
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = IWX_STATION_ID;
  cmd.station_type = IWX_STA_LINK;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(1, 0));
  cmd.add_modify = IWX_STA_MODE_MODIFY;
  cmd.station_flags = htole32(on ? kMfpEn : 0u);
  cmd.station_flags_msk = htole32(kMfpEn);
  cmd.tid_disable_tx = htole16(0xffff);
  uint32_t status = IWX_ADD_STA_SUCCESS;
  if (!nic_lock()) return false;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  nic_unlock();
  const bool ok = !err && (status & IWX_ADD_STA_STATUS_MASK) == IWX_ADD_STA_SUCCESS;
  _log(ok ? std::string("station: management frame protection ") + (on ? "on" : "off")
          : "station: management frame protection refused (" + std::to_string(err) + ")");
  return ok;
}

/* SEC_MGMT_MULTICAST_KEY_CMD_API_S_VER_2 (new TX API firmware: one 32-byte
 * key field; iwlwifi's iwl_mvm_send_sta_igtk). */
struct IgtkCmdV2 {
  uint32_t ctrl_flags;
  uint8_t igtk[32];
  uint32_t key_id;
  uint32_t sta_id;
  uint64_t receive_seq_cnt;
};

bool IwxDevice::sta_set_igtk(unsigned key_id, const uint8_t key[16], uint64_t rsc) {
  if (!_sta_sta_added || (key_id != 4 && key_id != 5)) return false;
  if (use_mld()) {
    const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_SEC_KEY_CMD);
    if (ver == IWX_FW_CMD_VER_UNKNOWN || ver < 1) {
      _log("station: SEC_KEY_CMD not advertised by firmware");
      return false;
    }
    const uint32_t flags =
        IWX_SEC_KEY_FLAG_CIPHER_CCMP | IWX_SEC_KEY_FLAG_MCAST_KEY | IWX_SEC_KEY_FLAG_MFP;
    iwx_sec_key_cmd cmd{};
    cmd.action = htole32(IWX_FW_CTXT_ACTION_ADD);
    cmd.u.add.sta_mask = htole32(1u << IWX_STATION_ID);
    cmd.u.add.key_id = htole32(key_id);
    cmd.u.add.key_flags = htole32(flags);
    std::memcpy(cmd.u.add.key, key, 16);
    cmd.u.add.rx_seq = htole64(rsc);
    if (!nic_lock()) return false;
    if (_sta_igtk_id) {
      mld_sec_key_remove(_sta_igtk_id, flags);
      _sta_igtk_id = 0;
    }
    const int err =
        send_cmd(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_SEC_KEY_CMD), &cmd, sizeof(cmd), false, nullptr);
    nic_unlock();
    if (err) {
      _log("station: MLD integrity group key refused (" + std::to_string(err) + ")");
      return false;
    }
    _sta_igtk_id = static_cast<uint8_t>(key_id);
    return true;
  }
  IgtkCmdV2 cmd{};
  cmd.ctrl_flags = htole32(IWX_STA_KEY_FLG_CMAC);
  std::memcpy(cmd.igtk, key, 16);
  cmd.key_id = htole32(key_id);
  cmd.sta_id = htole32(IWX_STATION_ID);
  cmd.receive_seq_cnt = htole64(rsc);
  if (!nic_lock()) return false;
  const int err = send_cmd(IWX_MGMT_MCAST_KEY, &cmd, sizeof(cmd), false, nullptr);
  nic_unlock();
  if (err) _log("station: integrity group key refused (" + std::to_string(err) + ")");
  return err == 0;
}

/* A Block Ack session the AP ended (DELBA), or one about to be replaced. */
bool IwxDevice::sta_remove_rx_ba(unsigned tid) {
  if (!_sta_sta_added || tid >= 8 || !((_sta_ba_tids >> tid) & 1)) return true;
  if (use_mld()) {
    const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD);
    iwx_rx_baid_cfg_cmd cmd{};
    cmd.action = htole32(IWX_RX_BAID_ACTION_REMOVE);
    if (ver == 1) {
      cmd.remove_v1.baid = htole32(_sta_baid[tid]);
    } else {
      cmd.remove.sta_id_mask = htole32(1u << IWX_STATION_ID);
      cmd.remove.tid = htole32(tid);
    }
    uint32_t baid = 0;
    if (!nic_lock()) return false;
    const int err = send_cmd_status(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD),
                                    &cmd, sizeof(cmd), &baid);
    nic_unlock();
    if (!err) {
      /* Buffered frames go up before the BAID can be reused. */
      _reorder.close(_sta_baid[tid]);
      _sta_ba_tids &= static_cast<uint8_t>(~(1u << tid));
      _sta_baid[tid] = 0xff;
    } else {
      _log("station: MLD removing Block Ack on TID " + std::to_string(tid) + " failed");
    }
    return err == 0;
  }
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = IWX_STATION_ID;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
  cmd.add_modify = IWX_STA_MODE_MODIFY;
  cmd.remove_immediate_ba_tid = static_cast<uint8_t>(tid);
  cmd.modify_mask = IWX_STA_MODIFY_REMOVE_BA_TID;
  uint32_t status = IWX_ADD_STA_SUCCESS;
  if (!nic_lock()) return false;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  nic_unlock();
  const bool ok = !err && (status & IWX_ADD_STA_STATUS_MASK) == IWX_ADD_STA_SUCCESS;
  /* Only forgotten once the firmware confirms (Codex review #23); a failed
   * removal is logged, and a following add still replaces the session. */
  if (ok) _sta_ba_tids &= static_cast<uint8_t>(~(1u << tid));
  else _log("station: removing Block Ack on TID " + std::to_string(tid) + " failed");
  return ok;
}

bool IwxDevice::sta_add_rx_ba(unsigned tid, uint16_t ssn, uint16_t window) {
  if (!_sta_sta_added || tid >= 8) return false;
  /* A new ADDBA for an open TID is a new session with its own start (the
   * AP restarts sessions after idling, or after a DELBA we missed). Keeping
   * the old one left the firmware's reorder window at the old sequence
   * number, and it dropped the AP's frames as stale before we ever saw them
   * (hardware, 2026-11-17: no internet after a while, "connected", every
   * counter clean). Replace it, as the AWDL side does. */
  if ((_sta_ba_tids >> tid) & 1) sta_remove_rx_ba(tid);
  if (use_mld()) {
    const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD);
    if (ver == IWX_FW_CMD_VER_UNKNOWN || ver < 1) return false;
    iwx_rx_baid_cfg_cmd cmd{};
    cmd.action = htole32(IWX_RX_BAID_ACTION_ADD);
    cmd.alloc.sta_id_mask = htole32(1u << IWX_STATION_ID);
    cmd.alloc.tid = static_cast<uint8_t>(tid);
    cmd.alloc.ssn = htole16(ssn);
    cmd.alloc.win_size = htole16(window);
    uint32_t baid = 0xffffffffu;
    if (!nic_lock()) return false;
    const int err = send_cmd_status(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD),
                                    &cmd, sizeof(cmd), &baid);
    nic_unlock();
    if (err || baid >= IWX_MAX_BAID) return false;
    _sta_ba_tids |= static_cast<uint8_t>(1u << tid);
    _sta_baid[tid] = static_cast<uint8_t>(baid);
    /* The firmware reorders by BAID/NSSN; the host does the same (Linux
     * iwl_mvm_reorder) so the frames it releases come up in order. */
    _reorder.open(baid, ssn, window);
    _log("station: Block Ack open (BAID " + std::to_string(baid) + "), tid " + std::to_string(tid) +
         ", window " + std::to_string(window));
    return true;
  }
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = IWX_STATION_ID;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
  cmd.add_modify = IWX_STA_MODE_MODIFY;
  cmd.add_immediate_ba_tid = static_cast<uint8_t>(tid);
  cmd.add_immediate_ba_ssn = htole16(ssn);
  cmd.rx_ba_window = htole16(window);
  cmd.modify_mask = IWX_STA_MODIFY_ADD_BA_TID;
  uint32_t status = IWX_ADD_STA_SUCCESS;
  if (!nic_lock()) return false;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  nic_unlock();
  if (err || (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) return false;
  _sta_ba_tids |= static_cast<uint8_t>(1u << tid);
  return true;
}

bool IwxDevice::sta_rate_control() {
  if (!_sta_sta_added) return false;
  iwx_tlc_config_cmd_v4 c{};
  c.sta_id = IWX_STATION_ID;
  /* Legacy rate indexes: 0-3 CCK, 4-11 OFDM 6..54. */
  c.non_ht_rates = htole16(static_cast<uint16_t>(_sta.channel <= 14 ? 0x0fff : 0x0ff0));
  const uint8_t ant = valid_tx_ant();
  c.chains = static_cast<uint8_t>(((ant & 1) ? IWX_TLC_MNG_CHAIN_A_MSK : 0) |
                                  ((ant & 2) ? IWX_TLC_MNG_CHAIN_B_MSK : 0));
  const bool mimo = _sta.mimo && (ant & 3) == 3;
  if (_sta.vht) {
    c.mode = IWX_TLC_MNG_MODE_VHT;
    c.ht_rates[IWX_TLC_NSS_1][IWX_TLC_MCS_PER_BW_80] = htole16(0x03ff); /* MCS 0-9 */
    if (mimo) c.ht_rates[IWX_TLC_NSS_2][IWX_TLC_MCS_PER_BW_80] = htole16(0x03ff);
  } else if (_sta.ht) {
    c.mode = IWX_TLC_MNG_MODE_HT;
    c.ht_rates[IWX_TLC_NSS_1][IWX_TLC_MCS_PER_BW_80] = htole16(0x00ff); /* MCS 0-7 */
    if (mimo) c.ht_rates[IWX_TLC_NSS_2][IWX_TLC_MCS_PER_BW_80] = htole16(0x00ff);
  } else {
    c.mode = IWX_TLC_MNG_MODE_NON_HT;
  }
  c.max_ch_width = static_cast<uint8_t>(_sta.width >= 80 ? IWX_TLC_MNG_CH_WIDTH_80MHZ
                                        : _sta.width >= 40 ? IWX_TLC_MNG_CH_WIDTH_40MHZ
                                                           : IWX_TLC_MNG_CH_WIDTH_20MHZ);
  if (_sta.ht || _sta.vht) {
    c.sgi_ch_width_supp = static_cast<uint8_t>((1u << IWX_TLC_MNG_CH_WIDTH_20MHZ) |
                                               (1u << IWX_TLC_MNG_CH_WIDTH_40MHZ) |
                                               (_sta.vht ? (1u << IWX_TLC_MNG_CH_WIDTH_80MHZ) : 0u));
    c.flags = htole16(IWX_TLC_MNG_CFG_FLAGS_STBC_MSK);
  }
  const uint32_t id = iwx_cmd_id(IWX_TLC_MNG_CONFIG_CMD, IWX_DATA_PATH_GROUP, 0);
  const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_TLC_MNG_CONFIG_CMD);
  if (use_mld() && ver != 4) {
    _sta_tlc = false;
    _log("station: MLD TLC_MNG_CONFIG_CMD version " + std::to_string(ver) + " not supported, fixed rates");
    return false;
  }
  if (!nic_lock()) return false;
  int err;
  if (ver == 4) {
    err = send_cmd(id, &c, sizeof(c), false, nullptr);
  } else {
    iwl_tlc_config_cmd_v3 v3{};
    v3.sta_id = c.sta_id;
    v3.max_ch_width = c.max_ch_width;
    v3.mode = c.mode;
    v3.chains = c.chains;
    v3.flags = c.flags;
    v3.non_ht_rates = c.non_ht_rates;
    v3.ht_rates[0][0] = c.ht_rates[0][0];
    v3.ht_rates[1][0] = c.ht_rates[1][0];
    v3.sgi_ch_width_supp = c.sgi_ch_width_supp;
    err = send_cmd(id, &v3, sizeof(v3), false, nullptr);
  }
  nic_unlock();
  _sta_tlc = err == 0;
  _log(_sta_tlc ? "station: firmware rate control on (TLC v" + std::to_string(ver) + ")"
                : "station: rate control refused (" + std::to_string(err) + "), fixed rates");
  return _sta_tlc;
}

void IwxDevice::sta_leave() {
  if (!_sta_phy_added && !_sta_mac_added && !_sta_binding_added && !_sta_link_added && !_sta_sta_added) return;
  if (use_mld()) {
    if (!_fw_error && nic_lock()) {
      /* MLD teardown order (Linux mvm/mld-mac80211.c + mld-sta.c + link.c):
       * session protection, RX BAIDs, TX flush, SCD queue remove, keys,
       * STA remove, link deactivate, link remove, MAC remove, PHY remove. */
      if (_sta_session_prot && _sta_link_active && !_fw_error) {
        const uint8_t sp_ver = cmd_version(IWX_MAC_CONF_GROUP, IWX_SESSION_PROTECTION_CMD);
        iwx_session_prot_cmd sp{};
        sp.id_and_color = htole32((sp_ver != IWX_FW_CMD_VER_UNKNOWN && sp_ver >= 2)
                                      ? kStaLink
                                      : IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
        sp.action = htole32(IWX_FW_CTXT_ACTION_REMOVE);
        sp.conf_id = htole32(IWX_SESSION_PROTECT_CONF_ASSOC);
        if (const int e = send_cmd(iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0), &sp, sizeof(sp),
                                   false, nullptr))
          _log("station: MLD could not cancel session protection (" + std::to_string(e) + ")");
      }
      if (_sta_sta_added && _sta_ba_tids && !_fw_error) {
        const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD);
        for (unsigned tid = 0; tid < 8 && !_fw_error; tid++) {
          if (!((_sta_ba_tids >> tid) & 1)) continue;
          iwx_rx_baid_cfg_cmd cmd{};
          cmd.action = htole32(IWX_RX_BAID_ACTION_REMOVE);
          if (ver == 1) cmd.remove_v1.baid = htole32(_sta_baid[tid]);
          else {
            cmd.remove.sta_id_mask = htole32(1u << IWX_STATION_ID);
            cmd.remove.tid = htole32(tid);
          }
          uint32_t baid = 0;
          send_cmd_status(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD),
                          &cmd, sizeof(cmd), &baid);
        }
      }
      /* Buffered frames go up before the firmware can reuse these BAIDs. */
      for (unsigned tid = 0; tid < 8; tid++)
        if ((_sta_ba_tids >> tid) & 1) _reorder.close(_sta_baid[tid]);
      if (_sta_sta_added && !_fw_error) {
        iwx_tx_path_flush_cmd f{};
        f.sta_id = htole32(IWX_STATION_ID);
        f.tid_mask = htole16(0xffff);
        if (const int e = send_cmd(IWX_TXPATH_FLUSH, &f, sizeof(f), false, nullptr))
          _log("station: MLD could not flush before removing it (" + std::to_string(e) + ")");
      }
      if (_sta_queue_enabled && !_fw_error) {
        if (const int e = disable_queue(IWX_STATION_ID, IWX_MGMT_TID))
          _log("station: MLD could not disable queue (" + std::to_string(e) + ")");
      }
      if (_sta_sta_added && !_fw_error) {
        if (_sta_ptk_flags) mld_sec_key_remove(0, _sta_ptk_flags);
        for (unsigned i = 0; i < 4 && !_fw_error; i++) {
          if ((_sta_gtk_mask >> i) & 1)
            mld_sec_key_remove(i, IWX_SEC_KEY_FLAG_CIPHER_CCMP | IWX_SEC_KEY_FLAG_MCAST_KEY);
        }
        if (_sta_igtk_id && !_fw_error)
          mld_sec_key_remove(_sta_igtk_id,
                             IWX_SEC_KEY_FLAG_CIPHER_CCMP | IWX_SEC_KEY_FLAG_MCAST_KEY | IWX_SEC_KEY_FLAG_MFP);
      }
      if (_sta_sta_added && !_fw_error) {
        iwx_remove_sta_cmd rm{};
        rm.sta_id = htole32(IWX_STATION_ID);
        send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_REMOVE_CMD), &rm, sizeof(rm), false, nullptr);
      }
      if (_sta_link_active && !_fw_error)
        mld_sta_link_modify(false, IWX_LINK_CONTEXT_MODIFY_ACTIVE);
      if (_sta_link_added && !_fw_error) mld_sta_link_remove();
      if (_sta_mac_added && !_fw_error) mld_sta_mac_cmd(IWX_FW_CTXT_ACTION_REMOVE, false);
      if (_sta_phy_added && !_fw_error) phy_cmd(kStaPhy, _sta.channel, 20, IWX_FW_CTXT_ACTION_REMOVE);
      nic_unlock();
    }
    _sta_phy_added = _sta_mac_added = _sta_binding_added = _sta_link_added = _sta_link_active = false;
    _sta_sta_added = _sta_queue_enabled = _sta_assoc = _sta_tlc = _sta_session_prot = _sta_mfp = false;
    _sta_ba_tids = _sta_gtk_mask = _sta_igtk_id = 0;
    _sta_ptk_flags = 0;
    std::memset(_sta_baid, 0xff, sizeof(_sta_baid));
    _staq.queued = 0;
    return;
  }
  if (!_fw_error && nic_lock()) {
    /* iwlwifi's teardown order: queue, station, quota, binding, MAC, PHY.
     * A station removed with its queue still enabled, or a time quota naming
     * a binding that is gone, asserts the firmware at the next join. */
    /* The TVQM queue needs no command: the firmware frees a removed
     * station's queues (iwl_mvm_disable_txq with the new TX API). */
    /* OpenBSD/itlwm's iwx_deauth starts by cancelling the session protection
     * and flushing the station (iwx_flush_sta: drain on, TXPATH_FLUSH of all
     * TIDs, drain off) before REMOVE_STA. Skipping both, a join whose access
     * point never answered authentication left frames queued and the time
     * event live: REMOVE_STA timed out and the firmware asserted (LMAC 0x125F),
     * three joins in a row in #21. */
    if (_sta_session_prot && _sta_mac_added && !_fw_error) {
      iwx_session_prot_cmd sp{};
      sp.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
      sp.action = htole32(IWX_FW_CTXT_ACTION_REMOVE);
      sp.conf_id = htole32(IWX_SESSION_PROTECT_CONF_ASSOC);
      if (const int e = send_cmd(iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0), &sp, sizeof(sp),
                                 false, nullptr))
        _log("station: could not cancel session protection (" + std::to_string(e) + ")");
    }
    if (_sta_sta_added && !_fw_error) {
      const auto drain = [this](bool on) {
        iwx_add_sta_cmd d{};
        d.sta_id = IWX_STATION_ID;
        d.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0));
        d.add_modify = IWX_STA_MODE_MODIFY;
        d.station_flags = htole32(on ? IWX_STA_FLG_DRAIN_FLOW : 0u);
        d.station_flags_msk = htole32(IWX_STA_FLG_DRAIN_FLOW);
        uint32_t status = IWX_ADD_STA_SUCCESS;
        return send_cmd_status(IWX_ADD_STA, &d, sizeof(d), &status);
      };
      int e = drain(true);
      if (!e && !_fw_error) {
        iwx_tx_path_flush_cmd f{};
        f.sta_id = htole32(IWX_STATION_ID);
        f.tid_mask = htole16(0xffff);
        e = send_cmd(IWX_TXPATH_FLUSH, &f, sizeof(f), false, nullptr);
        if (!e && !_fw_error) e = drain(false);
      }
      if (e) _log("station: could not flush before removing it (" + std::to_string(e) + ")");
    }
    if (_sta_sta_added && !_fw_error) {
      iwx_rm_sta_cmd rm{};
      rm.sta_id = IWX_STATION_ID;
      send_cmd(IWX_REMOVE_STA, &rm, sizeof(rm), false, nullptr);
    }
    if (_sta_binding_added && !_sta_shared_binding && !_fw.has_capa(IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA)) {
      iwx_time_quota_cmd tq{};
      for (unsigned i = 0; i < IWX_MAX_BINDINGS; i++) tq.quotas[i].id_and_color = htole32(IWX_FW_CTXT_INVALID);
      tq.quotas[0].id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
      tq.quotas[0].quota = htole32(IWX_MAX_QUOTA);
      send_cmd(IWX_TIME_QUOTA_CMD, &tq, sizeof(tq), false, nullptr);
    }
    if (_sta_binding_added) {
      if (_sta_shared_binding) {
        _sta_mac_added = false; /* binding_cmd() then lists the listener alone */
        binding_cmd(IWX_FW_CTXT_ACTION_MODIFY);
        _sta_mac_added = true;
      } else {
        const uint32_t mac = IWX_FW_CMD_ID_AND_COLOR(kStaMac, 0);
        binding_for(kStaBinding, kStaPhy, &mac, 1, _sta.channel, IWX_FW_CTXT_ACTION_REMOVE);
      }
    }
    if (_sta_mac_added) sta_mac_cmd(IWX_FW_CTXT_ACTION_REMOVE, false);
    if (_sta_phy_added) phy_cmd(kStaPhy, _sta.channel, 20, IWX_FW_CTXT_ACTION_REMOVE);
    nic_unlock();
  }
  _sta_phy_added = _sta_mac_added = _sta_binding_added = _sta_sta_added = false;
  _sta_queue_enabled = _sta_assoc = _sta_tlc = _sta_session_prot = false;
  _sta_ba_tids = 0;
  _staq.queued = 0;
}
