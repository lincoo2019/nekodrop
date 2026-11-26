// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Bz-family (BE200) transport registers/descriptors and MLD firmware API
 * command structures (API 86..100, monitor/inject subset).
 *
 * Derived from Linux drivers/net/wireless/intel/iwlwifi (dual BSD/GPL:
 * iwl-csr.h, iwl-fh.h, fw/api/mac-cfg.h, fw/api/rx.h, fw/api/phy-ctxt.h,
 * fw/api/nvm-reg.h, fw/api/ctxt-info-v2.h) and OpenBSD sys/dev/pci/if_iwxreg.h.
 *
 * Included from iwxreg.h inside its MSVC #pragma pack(push, 1) block.
 */
#pragma once

/* ---- Bz transport CSR / FH definitions ---- */
#define IWX_CSR_FUNC_SCRATCH                            0x02cu
#define IWX_CSR_FUNC_SCRATCH_INIT_VALUE                 0x01010101u

#define IWX_CSR_GP_CNTRL_REG_FLAG_MAC_INIT              (1u << 6)
#define IWX_CSR_GP_CNTRL_REG_FLAG_ROM_START             (1u << 7)
#define IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS            (1u << 20)
#define IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ     (1u << 21)
#define IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_STATUS (1u << 28)
#define IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_REQ    (1u << 29)
#define IWX_CSR_GP_CNTRL_REG_FLAG_SW_RESET              (1u << 31)

#define IWX_HBUS_TARG_WRPTR_RX_Q(q)                     (((uint32_t)(q) + 512u) << 16)
#define IWX_CSR_ADDR_BASE_BZ                            0x030u

#define IWX_MSIX_HW_INT_CAUSES_REG_ALIVE                (1u << 0)
/* Bz renumbers these: the image loader shares WAKEUP, and the firmware's own
 * error is reported on bit 5 (Linux iwl-csr.h, Bz). */
#define IWX_MSIX_HW_INT_CAUSES_REG_IML_BZ               (1u << 1)
#define IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR_BZ            (1u << 5)
#define IWX_MSIX_HW_INT_CAUSES_REG_HW_ERR               (1u << 29)

/* A hint (legacy group) that frames held for Block Ack may go up to an NSSN. */
#ifndef IWX_FRAME_RELEASE
#define IWX_FRAME_RELEASE                               0xc3u
#endif

/* Bz boot diagnostics: the CNVi reports the MAC step, WFPM ownership and the
 * four-phase handshake show who held the AUX bus, and the FSEQ/IML status is
 * read when the ROM does not hand over to the firmware. */
#define IWX_CNVI_AUX_MISC_CHIP                          (0xa200b0)
#define IWX_CNVI_AUX_MISC_CHIP_MAC_STEP(_val)           (((_val) & 0xf000000) >> 24)
#define IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE(_val)          ((_val) & 0xfff)
#define IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE_BZ_U           (0x930)
#define IWX_SD_REG_VER_GEN2                             (0xa2b800)
#define IWX_SILICON_A_STEP                              0
#define IWX_SILICON_B_STEP                              1
#define IWX_WFPM_OTP_CFG1_ADDR                          (0xa03098)
#define IWX_WFPM_RSRCS_4PHS_REQ_STTS                    (0xa033f8)
#define IWX_WFPM_RSRCS_4PHS_ACK_STTS                    (0xa033fc)
#define IWX_RSRC_CNVR_TOP                               (1u << 6)
#define IWX_BZ_UREG_UMAC_CURRENT_PC1                    (0xa05c1c)
#define IWX_BZ_UREG_LMAC1_CURRENT_PC                    (0xa05c20)

#define IWX_TFD_QUEUE_BC_SIZE_BZ                        4096u
#define IWX_IPC_DRAM_MAP_ENTRY_NUM_MAX                  32u

/* ---- Firmware TLV & capability additions (API 86..100) ---- */
#define IWX_UCODE_TLV_PNVM_DATA                         74u
#define IWX_UCODE_TLV_API_SMART_FIFO_OFFLOAD             68u
#define IWX_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG          32u
#define IWX_UCODE_TLV_CAPA_MLD_API_SUPPORT              110u
#define IWX_UCODE_TLV_CAPA_STA_EXP_MFP_SUPPORT          114u

/* ---- Bz transport structures ---- */
struct iwx_rx_completion_desc_bz {
  uint16_t rbid;
  uint8_t flags;
  uint8_t reserved[1];
} __packed;

struct iwx_bz_bc_tbl {
  uint16_t tfd_offset[IWX_TFD_QUEUE_BC_SIZE_BZ];
} __packed;

struct iwx_prph_scratch_mem_desc_addr_array {
  uint64_t mem_descs[IWX_IPC_DRAM_MAP_ENTRY_NUM_MAX];
} __packed;

/* ---- MLD firmware API commands (IWX_MAC_CONF_GROUP = 0x3) ---- */
#define IWX_MAC_CONFIG_CMD                              0x08u
#define IWX_LINK_CONFIG_CMD                             0x09u
#define IWX_STA_CONFIG_CMD                              0x0au
#define IWX_STA_REMOVE_CMD                              0x0cu

/* IWX_DATA_PATH_GROUP = 0x5 */
#define IWX_RLC_CONFIG_CMD                              0x08u
#define IWX_RX_BAID_ALLOCATION_CONFIG_CMD               0x16u
#define IWX_SCD_QUEUE_CONFIG_CMD                        0x17u
#define IWX_SEC_KEY_CMD                                 0x18u

#define IWX_MAX_BAID                                    32u
#define IWX_CHAN_CFG_FLAGS_BAND_POS                     30u

/* MAC_FILTER_FLAGS_MASK_E_VER_1 */
#define IWX_MAC_CFG_FILTER_PROMISC                      (1u << 0)
#define IWX_MAC_CFG_FILTER_ACCEPT_CONTROL_AND_MGMT      (1u << 1)
#define IWX_MAC_CFG_FILTER_ACCEPT_GRP                   (1u << 2)
#define IWX_MAC_CFG_FILTER_ACCEPT_BEACON                (1u << 3)
#define IWX_MAC_CFG_FILTER_ACCEPT_BCAST_PROBE_RESP      (1u << 4)
#define IWX_MAC_CFG_FILTER_ACCEPT_PROBE_REQ             (1u << 5)

struct iwx_mac_client_data {
  uint8_t is_assoc;
  uint8_t esr_transition_timeout;
  uint16_t medium_sync_delay;
  uint16_t assoc_id;
  uint16_t reserved1;
  uint16_t data_policy;
  uint16_t reserved2;
  uint32_t ctwin;
} __packed; /* MAC_CONTEXT_CONFIG_CLIENT_DATA_API_S_VER_2 */

struct iwx_mac_p2p_dev_data {
  uint32_t is_disc_extended;
} __packed; /* MAC_CONTEXT_CONFIG_P2P_DEV_DATA_API_S_VER_1 */

/* MAC_CONTEXT_CONFIG_CMD_API_S_VER_2 / _VER_3 (52 bytes) */
struct iwx_mac_config_cmd {
  uint32_t id_and_color;
  uint32_t action;
  uint32_t mac_type;
  uint8_t local_mld_addr[6];
  uint16_t reserved_for_local_mld_addr;
  uint32_t filter_flags;
  uint16_t he_support;
  uint16_t he_ap_support;
  uint32_t eht_support;
  uint32_t nic_not_ack_enabled;
  union {
    struct iwx_mac_client_data client;
    struct iwx_mac_p2p_dev_data p2p_dev;
  };
} __packed;

/* enum iwl_link_ctx_modify_flags */
#define IWX_LINK_CONTEXT_MODIFY_ACTIVE                  (1u << 0)
#define IWX_LINK_CONTEXT_MODIFY_RATES_INFO              (1u << 1)
#define IWX_LINK_CONTEXT_MODIFY_PROTECT_FLAGS           (1u << 2)
#define IWX_LINK_CONTEXT_MODIFY_QOS_PARAMS              (1u << 3)

/* LINK_CONTEXT_CONFIG_CMD_API_S_VER_1 .. _VER_9 (208 bytes) */
struct iwx_link_config_cmd {
  uint32_t action;
  uint32_t link_id;
  uint32_t mac_id;
  uint32_t phy_id;
  uint8_t local_link_addr[6];
  uint16_t reserved_for_local_link_addr;
  uint32_t modify_mask;
  uint32_t active;
  uint32_t listen_lmac;
  uint32_t cck_rates;
  uint32_t ofdm_rates;
  uint32_t cck_short_preamble;
  uint32_t short_slot;
  uint32_t protection_flags;
  uint32_t qos_flags;
  struct iwx_ac_qos ac[IWX_AC_NUM + 1];
  uint8_t htc_trig_based_pkt_ext;
  uint8_t rand_alloc_ecwmin;
  uint8_t rand_alloc_ecwmax;
  uint8_t ndp_fdbk_buff_th_exp;
  struct iwx_he_backoff_conf trig_based_txf[IWX_AC_NUM];
  uint32_t bi;
  uint32_t dtim_interval;
  uint16_t puncture_mask;
  uint16_t frame_time_rts_th;
  uint32_t flags;
  uint32_t flags_mask;
  uint8_t ref_bssid_addr[6];
  uint16_t reserved_for_ref_bssid_addr;
  uint8_t bssid_index;
  uint8_t bss_color;
  uint8_t spec_link_id;
  uint8_t ul_mu_data_disable;
  uint8_t ibss_bssid_addr[6];
  uint16_t reserved_for_ibss_bssid_addr;
  uint32_t reserved1[8];
} __packed;

/* enum iwl_fw_sta_type */
#define IWX_STATION_TYPE_PEER                           0u
#define IWX_STATION_TYPE_BCAST_MGMT                     1u

struct iwx_he_pkt_ext_v2 {
  uint8_t pkt_ext_qam_th[2][5][2];
} __packed;

/* First of the AWDL peers' station entries on the listener's link (0 and 1
 * are taken by the monitor station and the AP beside the listener). */
#define IWX_BZ_PEER_STA_ID                              2u

/* STA_CMD_API_S_VER_1 (96 bytes) */
struct iwx_sta_cfg_cmd_v1 {
  uint32_t sta_id;
  uint32_t link_id;
  uint8_t peer_mld_address[6];
  uint16_t reserved_for_peer_mld_address;
  uint8_t peer_link_address[6];
  uint16_t reserved_for_peer_link_address;
  uint32_t station_type;
  uint32_t assoc_id;
  uint32_t beamform_flags;
  uint32_t mfp;
  uint32_t mimo;
  uint32_t mimo_protection;
  uint32_t ack_enabled;
  uint32_t trig_rnd_alloc;
  uint32_t tx_ampdu_spacing;
  uint32_t tx_ampdu_max_size;
  uint32_t sp_length;
  uint32_t uapsd_acs;
  struct iwx_he_pkt_ext_v2 pkt_ext;
  uint32_t htc_flags;
} __packed;

/* STA_CMD_API_S_VER_2 (104 bytes) */
struct iwx_sta_cfg_cmd_v2 {
  uint32_t sta_id;
  uint32_t link_id;
  uint8_t peer_mld_address[6];
  uint16_t reserved_for_peer_mld_address;
  uint8_t peer_link_address[6];
  uint16_t reserved_for_peer_link_address;
  uint32_t station_type;
  uint32_t assoc_id;
  uint32_t beamform_flags;
  uint32_t mfp;
  uint32_t mimo;
  uint32_t mimo_protection;
  uint32_t ack_enabled;
  uint32_t trig_rnd_alloc;
  uint32_t tx_ampdu_spacing;
  uint32_t tx_ampdu_max_size;
  uint32_t sp_length;
  uint32_t uapsd_acs;
  struct iwx_he_pkt_ext_v2 pkt_ext;
  uint32_t htc_flags;
  uint8_t use_ldpc_x2_cw;
  uint8_t use_icf;
  uint8_t dps_pad_time;
  uint8_t dps_trans_delay;
  uint8_t mic_prep_pad_delay;
  uint8_t mic_compute_pad_delay;
  uint8_t reserved[2];
} __packed;

/* REMOVE_STA_API_S_VER_1 (4 bytes) */
struct iwx_remove_sta_cmd {
  uint32_t sta_id;
} __packed;

/* RLC_CONFIG_CMD_API_S_VER_2 (32 bytes) */
struct iwx_rlc_properties {
  uint32_t rx_chain_info;
  uint32_t reserved;
} __packed;

struct iwx_sad_properties {
  uint32_t chain_a_sad_mode;
  uint32_t chain_b_sad_mode;
  uint32_t mac_id;
  uint32_t reserved;
} __packed;

struct iwx_rlc_config_cmd {
  uint32_t phy_id;
  struct iwx_rlc_properties rlc;
  struct iwx_sad_properties sad;
  uint8_t flags;
  uint8_t reserved[3];
} __packed;

/* LAR_UPDATE_MCC_CMD_RESP_S_VER_8 (24 bytes before channels[]) */
struct iwx_mcc_update_resp_v8 {
  uint32_t status;
  uint16_t mcc;
  uint8_t padding[2];
  uint32_t cap;
  uint16_t time;
  uint16_t geo_info;
  uint8_t source_id;
  uint8_t reserved[3];
  uint32_t n_channels;
  uint32_t channels[0];
} __packed;

/* enum iwl_rx_baid_action (RX_BAID_ALLOCATION_ACTION_E_VER_1) */
#define IWX_RX_BAID_ACTION_ADD                          0u
#define IWX_RX_BAID_ACTION_MODIFY                       1u
#define IWX_RX_BAID_ACTION_REMOVE                       2u

struct iwx_rx_baid_cfg_cmd_alloc {
  uint32_t sta_id_mask;
  uint8_t tid;
  uint8_t reserved[3];
  uint16_t ssn;
  uint16_t win_size;
} __packed; /* RX_BAID_ALLOCATION_ADD_CMD_API_S_VER_1 */

struct iwx_rx_baid_cfg_cmd_modify {
  uint32_t old_sta_id_mask;
  uint32_t new_sta_id_mask;
  uint32_t tid;
} __packed; /* RX_BAID_ALLOCATION_MODIFY_CMD_API_S_VER_2 */

struct iwx_rx_baid_cfg_cmd_remove_v1 {
  uint32_t baid;
} __packed; /* RX_BAID_ALLOCATION_REMOVE_CMD_API_S_VER_1 */

struct iwx_rx_baid_cfg_cmd_remove {
  uint32_t sta_id_mask;
  uint32_t tid;
} __packed; /* RX_BAID_ALLOCATION_REMOVE_CMD_API_S_VER_2 */

/* RX_BAID_ALLOCATION_CONFIG_CMD_API_S_VER_2 (16 bytes) */
struct iwx_rx_baid_cfg_cmd {
  uint32_t action;
  union {
    struct iwx_rx_baid_cfg_cmd_alloc alloc;
    struct iwx_rx_baid_cfg_cmd_modify modify;
    struct iwx_rx_baid_cfg_cmd_remove_v1 remove_v1;
    struct iwx_rx_baid_cfg_cmd_remove remove;
  };
} __packed;

/* RX_BAID_ALLOCATION_RESPONSE_API_S_VER_1 (4 bytes) */
struct iwx_rx_baid_cfg_resp {
  uint32_t baid;
} __packed;

/* enum iwl_scd_queue_cfg_operation */
#define IWX_SCD_QUEUE_ADD                               0u
#define IWX_SCD_QUEUE_REMOVE                            1u
#define IWX_SCD_QUEUE_MODIFY                            2u

/* TX_QUEUE_CFG_CMD_API_S_VER_3 (36 bytes, DATA_PATH_GROUP:SCD_QUEUE_CONFIG_CMD) */
struct iwx_scd_queue_cfg_cmd {
  uint32_t operation;
  union {
    struct {
      uint32_t sta_mask;
      uint8_t tid;
      uint8_t reserved[3];
      uint32_t flags;
      uint32_t cb_size;
      uint64_t bc_dram_addr;
      uint64_t tfdq_dram_addr;
    } __packed add;
    struct {
      uint32_t sta_mask;
      uint32_t tid;
    } __packed remove;
    struct {
      uint32_t old_sta_mask;
      uint32_t tid;
      uint32_t new_sta_mask;
    } __packed modify;
  } __packed u;
} __packed;

/* enum iwl_sec_key_flags */
#define IWX_SEC_KEY_FLAG_CIPHER_MASK                    0x07u
#define IWX_SEC_KEY_FLAG_CIPHER_WEP                     0x01u
#define IWX_SEC_KEY_FLAG_CIPHER_CCMP                    0x02u
#define IWX_SEC_KEY_FLAG_CIPHER_TKIP                    0x03u
#define IWX_SEC_KEY_FLAG_CIPHER_GCMP                    0x05u
#define IWX_SEC_KEY_FLAG_NO_TX                          0x08u
#define IWX_SEC_KEY_FLAG_KEY_SIZE                       0x10u
#define IWX_SEC_KEY_FLAG_MFP                            0x20u
#define IWX_SEC_KEY_FLAG_MCAST_KEY                      0x40u
#define IWX_SEC_KEY_FLAG_SPP_AMSDU                      0x80u

/* SEC_KEY_CMD_API_S_VER_1 (80 bytes, DATA_PATH_GROUP:SEC_KEY_CMD) */
struct iwx_sec_key_cmd {
  uint32_t action;
  union {
    struct {
      uint32_t sta_mask;
      uint32_t key_id;
      uint32_t key_flags;
      uint8_t key[32];
      uint8_t tkip_mic_rx_key[8];
      uint8_t tkip_mic_tx_key[8];
      uint64_t rx_seq;
      uint64_t tx_seq;
    } __packed add;
    struct {
      uint32_t old_sta_mask;
      uint32_t new_sta_mask;
      uint32_t key_id;
      uint32_t key_flags;
    } __packed modify;
    struct {
      uint32_t sta_mask;
      uint32_t key_id;
      uint32_t key_flags;
    } __packed remove;
  } __packed u;
} __packed;
