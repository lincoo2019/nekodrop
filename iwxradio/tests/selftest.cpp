// SPDX-License-Identifier: GPL-2.0-only
/* Checks that need no card: the firmware ABI struct sizes the driver depends
 * on, that each bundled Qu/QuZ firmware parses into the section layout
 * and command versions the bring-up expects, and the radio backend contract
 * (tests/radio_backend_test.h).
 * Usage: iwx_selftest <firmware dir> */
#include <cstdio>
#include <string>

#include "../src/iwxreg.h"
#include "../src/iwx_channel.h"
#include "../src/dma_runs.h"
#include "../src/cards.h"
#include "../src/fw.h"
#include "../src/awdl_policy.h"
#include "../src/coex_plan.h"
#include "../src/crypto.h"
#include "../src/wpa3.h"
#include "radio_backend_test.h"
#include <cstring>
#include <vector>

static int failures = 0;

#define EXPECT_SIZE(type, n)                                                            \
  do {                                                                                  \
    if (sizeof(type) != (n)) {                                                          \
      std::printf("FAIL sizeof(%s) = %zu, expected %d\n", #type, sizeof(type), (n));    \
      failures++;                                                                       \
    } else {                                                                            \
      std::printf("ok   sizeof(%s) = %d\n", #type, (n));                                \
    }                                                                                   \
  } while (0)

int main(int argc, char **argv) {
  static_assert(iwx_primary80_position(140) == IWX_PHY_VHT_CTRL_POS_1_ABOVE);
  static_assert(iwx_primary80_position(136) == IWX_PHY_VHT_CTRL_POS_1_BELOW);
  static_assert(iwx_primary80_position(144) == IWX_PHY_VHT_CTRL_POS_2_ABOVE);
  static_assert(iwx_primary80_position(149) == IWX_PHY_VHT_CTRL_POS_2_BELOW);
  static_assert(iwx_primary80_position(14) == -1 && iwx_primary80_position(146) == -1);
  {
    /* An early 4-page ring must not consume the only 8-page firmware span,
     * and RX's individual pages cannot take either reservation. */
    DmaRuns runs;
    runs.reset({100,101,102,103,104,105,106,107, 200,201,202,203, 300});
    const bool impossible = runs.reserve({9}).has_value();
    auto planned = runs.reserve({4, 8});
    if (impossible || !planned || planned->count(4) != 1 || planned->count(8) != 1 ||
        planned->find(4)->second != 8 || planned->find(8)->second != 0 ||
        runs.take(1) != std::optional<size_t>{12} || runs.take(1)) {
      std::printf("FAIL fragmented DMA runs (#30)\n");
      failures++;
    } else {
      std::printf("ok   fragmented DMA runs preserve firmware and rings (#30)\n");
    }
  }
  /* Sizes from Linux iwlwifi's fw/api headers for the same API versions. */
  EXPECT_SIZE(iwx_tfh_tb, 10);
  EXPECT_SIZE(iwx_tfh_tfd, 256);
  EXPECT_SIZE(iwx_cmd_header, 4);
  EXPECT_SIZE(iwx_cmd_header_wide, 8);
  EXPECT_SIZE(iwx_device_cmd, 324);
  EXPECT_SIZE(iwx_tx_cmd_gen2, 20);
  EXPECT_SIZE(iwx_rb_status, 12);
  EXPECT_SIZE(iwx_tx_queue_cfg_cmd, 24);
  EXPECT_SIZE(iwx_tx_queue_cfg_rsp, 8);
  EXPECT_SIZE(iwx_binding_cmd, 28);
  EXPECT_SIZE(iwx_phy_context_cmd, 32);
  EXPECT_SIZE(iwx_add_sta_cmd, 48); /* ADD_STA_CMD_API_S_VER_10 */
  EXPECT_SIZE(iwx_mcc_update_cmd, 28);
  EXPECT_SIZE(iwx_alive_resp_v4, 0x74);
  EXPECT_SIZE(iwx_tx_resp, 44);
  /* AX210 family */
  EXPECT_SIZE(iwx_tx_cmd_gen3, 28);          /* TX_CMD_API_S_VER_8 */
  EXPECT_SIZE(iwx_rx_transfer_desc, 16);
  EXPECT_SIZE(iwx_rx_completion_desc, 32);
  EXPECT_SIZE(iwx_gen3_bc_tbl, 2048);
  EXPECT_SIZE(iwx_context_info_gen3, 104);   /* IPC_CONTEXT_INFO_S */
  EXPECT_SIZE(iwx_alive_resp_v5, 0x74 + 12); /* v4 + sku_id */
  /* Bz family (BE200) and MLD API */
  EXPECT_SIZE(iwx_rx_completion_desc_bz, 4);
  EXPECT_SIZE(iwx_bz_bc_tbl, 8192);
  EXPECT_SIZE(iwx_prph_scratch_mem_desc_addr_array, 256);
  EXPECT_SIZE(iwx_mac_config_cmd, 52);       /* MAC_CONTEXT_CONFIG_CMD_API_S_VER_2 / _VER_3 */
  EXPECT_SIZE(iwx_link_config_cmd, 208);     /* LINK_CONTEXT_CONFIG_CMD_API_S_VER_1..9 */
  EXPECT_SIZE(iwx_sta_cfg_cmd_v1, 96);       /* STA_CMD_API_S_VER_1 */
  EXPECT_SIZE(iwx_sta_cfg_cmd_v2, 104);      /* STA_CMD_API_S_VER_2 */
  EXPECT_SIZE(iwx_remove_sta_cmd, 4);        /* REMOVE_STA_API_S_VER_1 */
  EXPECT_SIZE(iwx_rlc_config_cmd, 32);       /* RLC_CONFIG_CMD_API_S_VER_2 */
  EXPECT_SIZE(iwx_mcc_update_resp_v8, 24);   /* LAR_UPDATE_MCC_CMD_RESP_S_VER_8 */
  EXPECT_SIZE(iwx_rx_baid_cfg_cmd, 16);      /* RX_BAID_ALLOCATION_CONFIG_CMD_API_S_VER_2 */
  EXPECT_SIZE(iwx_rx_baid_cfg_resp, 4);      /* RX_BAID_ALLOCATION_RESPONSE_API_S_VER_1 */
  EXPECT_SIZE(iwx_scd_queue_cfg_cmd, 36);    /* TX_QUEUE_CFG_CMD_API_S_VER_3 */
  EXPECT_SIZE(iwx_sec_key_cmd, 80);          /* SEC_KEY_CMD_API_S_VER_1 */
  EXPECT_SIZE(iwx_tlc_config_cmd_v4, 28);    /* TLC_MNG_CONFIG_CMD_API_S_VER_4 */
  EXPECT_SIZE(iwx_scan_config, 12);          /* SCAN_CONFIG_DB_CMD_API_S_5 */

  /* Card selection (cards.h), with register values as iwx keeps them. */
  struct CardCase { uint16_t dev; uint32_t hw_rev, rf_id; const char *fw; bool ax210, bz, integrated; };
  const CardCase cases[] = {
      {0x43F0, 0x354, 0x0010a100, "iwlwifi-QuZ-a0-hr-b0-68.ucode", false, false, true}, /* this project's AX201 */
      {0xA0F0, 0x334, 0x0010a100, "iwlwifi-Qu-b0-hr-b0-68.ucode", false, false, true},
      {0x34F0, 0x338, 0x0010a100, "iwlwifi-Qu-c0-hr-b0-68.ucode", false, false, true},
      {0x02F0, 0x354, 0x00108000, "iwlwifi-QuZ-a0-jf-b0-68.ucode", false, false, true},  /* 9560 on QuZ */
      {0x2723, 0x340, 0x0010a100, "iwlwifi-cc-a0-68.ucode", false, false, false},        /* AX200 */
      {0x2725, 0x420, 0x0010d000, "iwlwifi-ty-a0-gf-a0-68.ucode", true, false, false},   /* AX210: discrete */
      {0x7AF0, 0x370, 0x0010d000, "iwlwifi-so-a0-gf-a0-68.ucode", true, false, true},    /* AX211 */
      {0x51F0, 0x370, 0x1010d000, "iwlwifi-so-a0-gf4-a0-68.ucode", true, false, true},   /* AX411 */
      {0x272B, 0x470, 0x00112000, "iwlwifi-gl-c0-fm-c0-100.ucode", true, true, false},   /* BE200: discrete Gl/Fm */
      {0x272B, 0x470, 0x0010d000, nullptr, true, true, false},                           /* Gl with non-FM radio */
      {0x7740, 0x460, 0x0010a000, "iwlwifi-bz-b0-hr-b0-100.ucode", true, true, true},     /* AX203 on Arrow Lake */
      {0x2726, 0x420, 0x0010d000, nullptr, true, false, true},                           /* SnJ: no firmware */
      {0x1234, 0x354, 0x0010a100, nullptr, false, false, true},
  };
  for (const auto &k : cases) {
    const CardInfo c = cards::select(k.dev, k.hw_rev, k.rf_id);
    const bool ok = k.fw ? (c.known && c.firmware == k.fw && c.ax210 == k.ax210 && c.bz == k.bz &&
                            c.integrated == k.integrated)
                         : !c.known;
    std::printf("%s card %04x rev %03x rf %08x -> %s%s\n", ok ? "ok  " : "FAIL", k.dev, k.hw_rev, k.rf_id,
                c.known ? c.firmware.c_str() : "unsupported: ", c.known ? "" : c.why.c_str());
    if (!ok) failures++;
  }
  {
    const CardInfo c = cards::select(0x272B, 0x470, 0x00112000);
    if (!c.known || !c.ax210 || !c.bz || c.integrated || c.xtal_latency != 12000 ||
        !c.low_latency_xtal || c.tested || cards::family_device("be200") != 0x272B) {
      std::printf("FAIL BE200 transport parameters or family_device(\"be200\")\n");
      failures++;
    }
  }
  {
    const CardInfo c = cards::select(0x7740, 0x460, 0x0010a000);
    if (!c.known || !c.ax210 || !c.bz || !c.integrated || c.xtal_latency != 12000 ||
        c.ltr_delay != IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500 || !c.low_latency_xtal || !c.tested ||
        c.pnvm != "iwlwifi-bz-b0-hr-b0.pnvm")
      std::printf("FAIL AX203 (8086:7740) transport parameters, PNVM or tested flag\n"), failures++;
  }
  {
    const CardInfo c = cards::select(0x43F0, 0x354, 0x0010a100);
    if (c.xtal_latency != 12000 || c.ltr_delay != IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500 || !c.low_latency_xtal || !c.tested) {
      std::printf("FAIL the tested AX201 lost its transport parameters\n");
      failures++;
    }
  }
  {
    /* The Raptor Lake AX211 (issue #2): Linux's so_long_latency_imr parameters. */
    const CardInfo c = cards::select(0x7A70, 0x370, 0x0010d000);
    if (!c.known || !c.integrated || !c.imr || !c.low_latency_xtal || c.xtal_latency != 12000) {
      std::printf("FAIL the 7A70 AX211 lost its IMR / long-latency parameters\n");
      failures++;
    }
  }
  std::printf("info IWX_RX_DESC_SIZE_V1 = %zu, sizeof(iwx_context_info) = %zu, "
              "sizeof(iwx_mac_ctx_cmd) = %zu\n",
              (size_t)IWX_RX_DESC_SIZE_V1, sizeof(iwx_context_info), sizeof(iwx_mac_ctx_cmd));

  if (argc > 1) {
    const char *names[] = {"iwlwifi-Qu-b0-hr-b0-68.ucode", "iwlwifi-Qu-c0-hr-b0-68.ucode",
                           "iwlwifi-QuZ-a0-hr-b0-68.ucode", "iwlwifi-Qu-b0-jf-b0-68.ucode",
                           "iwlwifi-Qu-c0-jf-b0-68.ucode", "iwlwifi-QuZ-a0-jf-b0-68.ucode",
                           "iwlwifi-cc-a0-68.ucode", "iwlwifi-so-a0-gf-a0-68.ucode",
                           "iwlwifi-so-a0-gf4-a0-68.ucode", "iwlwifi-so-a0-hr-b0-68.ucode",
                           "iwlwifi-so-a0-jf-b0-68.ucode", "iwlwifi-ty-a0-gf-a0-68.ucode"};
    for (const char *name : names) {
      Firmware fw;
      std::string error;
      const std::string path = std::string(argv[1]) + "\\" + name;
      if (!fw.load(path, error)) {
        std::printf("FAIL %s: %s\n", name, error.c_str());
        failures++;
        continue;
      }
      size_t lmac = 0, umac = 0, paging = 0, seps = 0;
      for (const auto &s : fw.regular) {
        if (s.devoff == IWX_CPU1_CPU2_SEPARATOR_SECTION || s.devoff == IWX_PAGING_SEPARATOR_SECTION)
          seps++;
        else if (seps == 0) lmac++;
        else if (seps == 1) umac++;
        else paging++;
      }
      size_t largest = 0;
      for (const auto &s : fw.regular) largest = s.len > largest ? s.len : largest;
      std::printf("ok   %s: version %s, %zu LMAC / %zu UMAC / %zu paging sections, largest %zu "
                  "bytes, phy_config 0x%x\n",
                  name, fw.version.c_str(), lmac, umac, paging, largest, fw.phy_config);
      std::printf("     cmd versions: TX_CMD %u, PHY_CONTEXT %u, ADD_STA %u, SCAN_REQ_UMAC %u, "
                  "MAC_CONTEXT %u; DQA %d, LAR %d, CDB %d, UHB %d, NVM_INFO_v4 %d\n",
                  fw.cmd_version(IWX_LONG_GROUP, IWX_TX_CMD),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_PHY_CONTEXT_CMD),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_ADD_STA),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_SCAN_REQ_UMAC),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_MAC_CONTEXT_CMD),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_DQA_SUPPORT), fw.has_capa(IWX_UCODE_TLV_CAPA_LAR_SUPPORT),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_CDB_SUPPORT),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS),
                  fw.has_api(IWX_UCODE_TLV_API_REGULATORY_NVM_INFO));
      const bool ax210 = std::strncmp(name, "iwlwifi-so-", 11) == 0 || std::strncmp(name, "iwlwifi-ty-", 11) == 0;
      if (ax210 && !fw.iml_len) {
        std::printf("FAIL %s: an AX210-family firmware without an image loader\n", name);
        failures++;
      }
      if (largest > (2u << 20)) {
        std::printf("FAIL a section is larger than one 2 MiB DMA block\n");
        failures++;
      }
    }
    Firmware be200;
    std::string be200_error;
    if (!be200.load(std::string(argv[1]) + "\\iwlwifi-gl-c0-fm-c0-100.ucode", be200_error)) {
      std::printf("FAIL BE200 firmware: %s\n", be200_error.c_str());
      failures++;
    } else if (!be200.has_capa(IWX_UCODE_TLV_CAPA_MLD_API_SUPPORT) ||
               !be200.has_api(IWX_UCODE_TLV_API_SMART_FIFO_OFFLOAD)) {
      std::printf("FAIL BE200 firmware did not advertise MLD and Smart FIFO offload\n");
      failures++;
    } else {
      std::printf("ok   BE200 firmware %s advertises MLD and Smart FIFO offload\n", be200.version.c_str());
    }
  }

  /* AWDL policy (ported from ForbiddenItlwm), on a captured iPhone frame. */
  {
#define CHECK(cond, what)                                                                   \
  do {                                                                                      \
    if (cond) std::printf("ok   %s\n", what);                                               \
    else {                                                                                  \
      std::printf("FAIL %s\n", what);                                                       \
      failures++;                                                                           \
    }                                                                                       \
  } while (0)
    /* TLV 18 as an iPhone in Japan sent it on 2026-11-16, and TLV 12 "JP". */
    const uint8_t tlv18[] = {0x0f, 0x03, 0x00, 0x03, 0xff, 0xff, 0x2c, 0x80, 0x2c, 0x80, 0x2c, 0x80,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x80, 0x06, 0x51,
                             0x2c, 0x80, 0x2c, 0x80, 0x2c, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x2c, 0x80, 0x00, 0x00, 0x00};
    std::vector<uint8_t> f(40, 0);
    f[0] = 0xd0; f[24] = 0x7f; f[25] = 0x00; f[26] = 0x17; f[27] = 0xf2; f[28] = 0x08;
    f.push_back(18); f.push_back(sizeof(tlv18)); f.push_back(0);
    f.insert(f.end(), tlv18, tlv18 + sizeof(tlv18));
    const uint8_t tlv12[] = {12, 4, 0, 0, 0, 'J', 'P'};
    f.insert(f.end(), tlv12, tlv12 + sizeof(tlv12));
    CHECK(awdl::is_awdl_action(f.data(), f.size()), "AWDL action frame recognised");
    {
      /* FCS (#24/#25): CRC-32 check value, then a frame with its FCS. */
      const uint8_t nine[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
      CHECK(awdl::crc32_ieee(nine, 9) == 0xCBF43926u, "CRC-32 check value");
      std::vector<uint8_t> g = f;
      g.push_back(21); g.push_back(2); g.push_back(0); g.push_back(3); g.push_back(2); /* version TLV */
      const size_t bare = g.size();
      CHECK(awdl::strip_valid_fcs(g.data(), bare) == bare, "frame without FCS kept whole");
      const awdl::AwdlTlvFit fit = awdl::awdl_tlv_fit(g.data(), bare);
      CHECK(fit.left == 0 && fit.has_version, "TLVs fill the frame, version TLV found");
      const uint32_t fcs = awdl::crc32_ieee(g.data(), bare);
      for (int i = 0; i < 4; i++) g.push_back(static_cast<uint8_t>(fcs >> (8 * i)));
      CHECK(awdl::strip_valid_fcs(g.data(), g.size()) == bare, "valid FCS removed");
      CHECK(awdl::awdl_tlv_fit(g.data(), g.size()).left == 4, "FCS left on shows as 4 bytes after the TLVs");
      g.back() ^= 1;
      CHECK(awdl::strip_valid_fcs(g.data(), g.size()) == g.size(), "wrong FCS kept");
    }
    {
      /* #4: our DELBA as the session's sender, and when an idle session ends. */
      const uint8_t phone[6] = {2, 1, 1, 1, 1, 1}, pc[6] = {2, 2, 2, 2, 2, 2};
      const uint8_t bss[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
      uint8_t d[awdl::kDelbaLength];
      CHECK(awdl::build_delba(d, phone, pc, bss, 0, true, 7) == 30, "DELBA is 30 bytes");
      bool sender = false;
      uint8_t tid = 9;
      CHECK(awdl::parse_delba(d, sizeof(d), phone, &sender, &tid) && sender && tid == 0,
            "DELBA reads back as the sender's, TID 0");
      CHECK(!awdl::ba_tx_session_idle(true, 0, 0, 1000000, 500000, 9000000), "BA session kept during a bulk send");
      CHECK(!awdl::ba_tx_session_idle(false, 3, 0, 1000000, 500000, 9000000), "BA session kept while frames wait");
      CHECK(!awdl::ba_tx_session_idle(false, 0, 1, 1000000, 500000, 9000000), "BA session kept while frames are queued");
      CHECK(!awdl::ba_tx_session_idle(false, 0, 0, 1000000, 500000, 2000000), "BA session kept 1 s after the last A-MPDU");
      CHECK(awdl::ba_tx_session_idle(false, 0, 0, 1000000, 500000, 2600000), "BA session ended 1.6 s after the last A-MPDU");
      CHECK(awdl::ba_tx_session_idle(false, 0, 0, 0, 500000, 2100000), "BA session with no A-MPDU ends 1.6 s after agreeing");
    }
    {
      /* --coex-plan (ForbiddenItlwm PR #30). */
      using namespace awdl_plan;
      uint8_t away[16];
      CHECK(!plan_entries(true, 44, 80, 44, false, InfraClass::Idle, away), "plan: station on the AWDL channel, no router entries");
      CHECK(!plan_entries(true, 6, 20, 149, false, InfraClass::Idle, away), "plan: 2.4 GHz station keeps none");
      CHECK(!plan_entries(false, 40, 80, 149, false, InfraClass::Idle, away), "plan: no station, none");
      CHECK(plan_entries(true, 44, 160, 44, false, InfraClass::Idle, away), "plan: 160 MHz station over the AWDL channel");
      CHECK(plan_entries(true, 40, 80, 149, false, InfraClass::Idle, away) && away[6] && away[7] && away[14] &&
                away[15] && !away[0] && !away[13], "plan: AWDL idle: entries 6, 7, 14, 15");
      CHECK(plan_entries(true, 40, 80, 149, true, InfraClass::Idle, away) && away[15] && !away[6],
            "plan: AWDL data, router idle: entry 15 only");
      CHECK(plan_entries(true, 100, 80, 149, true, InfraClass::Data, away) && away[4] && away[12] && !away[11],
            "plan: router on a radar channel allowed; router data: 4, 5, 12-15");
      CHECK(plan_entries(true, 40, 80, 149, true, InfraClass::Streaming, away) && away[11],
            "plan: router streaming adds entry 11");
      InfraMeter m;
      m.note(1000000, 0, 0);
      CHECK(m.note(2000000, 200, 200 * 300) && m.cls == InfraClass::Data, "plan: 200 small packets/s is data");
      CHECK(m.note(3000000, 260, 200 * 300 + 60 * 1500) && m.cls == InfraClass::Streaming, "plan: big packets are streaming");
      CHECK(!m.note(3500000, 999, 0), "plan: sampled once a second");
      /* A frame of ours: TLV 4 (33 bytes, AW counter 0x0123 at 29, then a
       * 16-entry op-class sequence) and TLV 18 (the same sequence). */
      std::vector<uint8_t> g(40, 0);
      g[0] = 0xd0; g[24] = 0x7f; g[25] = 0x00; g[26] = 0x17; g[27] = 0xf2; g[28] = 0x08;
      std::vector<uint8_t> cs = {15, 3, 0, 3, 0xff, 0xff};
      for (int k = 0; k < 16; k++) { cs.push_back(149); cs.push_back(124); }
      std::vector<uint8_t> t4(33, 0);
      t4[29] = 0x23; t4[30] = 0x01;
      t4.insert(t4.end(), cs.begin(), cs.end());
      t4.push_back(0); t4.push_back(0);
      g.push_back(4); g.push_back(uint8_t(t4.size())); g.push_back(0);
      g.insert(g.end(), t4.begin(), t4.end());
      const size_t t18 = g.size();
      g.push_back(18); g.push_back(uint8_t(cs.size() + 3)); g.push_back(0);
      g.insert(g.end(), cs.begin(), cs.end());
      g.push_back(0); g.push_back(0); g.push_back(0);
      CHECK(frame_aw_counter(g.data(), g.size()) == 0x0123, "plan: AW counter read from TLV 4");
      CHECK(in_step(0x0123, 0x0123 / 4) && in_step(0x0123, 0x0123 / 4 + 1) && !in_step(0x0123, 0x0123 / 4 + 3),
            "plan: in step within one extended AW");
      plan_entries(true, 40, 80, 149, false, InfraClass::Idle, away);
      CHECK(rewrite_frame(g.data(), g.size(), away, 40), "plan: frame rewritten");
      const uint8_t *e18 = g.data() + t18 + 3 + 6;
      CHECK(e18[2 * 6] == 40 && e18[2 * 6 + 1] == 115 && e18[2 * 15] == 40 && e18[0] == 149 && e18[2 * 13] == 149,
            "plan: TLV 18 names channel 40 (class 115) in 6/15, 149 elsewhere");
      const uint8_t *e4 = g.data() + 40 + 3 + 33 + 6;
      CHECK(e4[2 * 7] == 40 && e4[2 * 8] == 149, "plan: TLV 4's sequence rewritten the same way");
      CHECK(awdl::awdl_tlv_fit(g.data(), g.size()).left == 0, "plan: rewritten frame keeps its TLV layout");
    }
    uint8_t seq[16];
    CHECK(awdl::peer_sequence(f.data(), f.size(), seq), "channel sequence parsed");
    CHECK(awdl::peer_channel(seq) == 44, "iPhone in Japan: peer 5 GHz channel is 44");
    char c[3];
    CHECK(awdl::peer_country(f.data(), f.size(), c) && std::string(c) == "JP", "peer country JP");
    awdl::PeerCountry pc;
    bool changed = false;
    for (int k = 0; k < 5; k++) changed |= pc.note(c);
    CHECK(changed && std::string(pc.confirmed) == "JP", "country confirmed after 5 frames");
    uint8_t s149[16] = {149, 149, 149, 0, 0, 0, 0, 0, 6, 149, 149, 149, 0, 0, 0, 0};
    CHECK(awdl::peer_channel(s149) == 149, "sequence naming 149 gives 149");
    uint8_t wifi44[16] = {149, 149, 0, 0, 0, 0, 44, 44, 149, 149, 0, 0, 0, 0, 44, 44};
    CHECK(awdl::peer_channel(wifi44) == 149, "44 only in Wi-Fi slots, 149 named: 149");

    /* ADDBA request -> response round trip. */
    const uint8_t own[6] = {0x02, 1, 2, 3, 4, 5}, peer[6] = {0x66, 0x71, 0xa5, 0xbe, 0xeb, 0xb3};
    uint8_t req[33] = {};
    req[0] = 0xd0;
    std::memcpy(req + 4, own, 6);
    std::memcpy(req + 10, peer, 6);
    req[24] = 3; req[25] = 0; req[26] = 7;
    const uint16_t params = uint16_t(2 | (5 << 2) | (64 << 6));
    req[27] = uint8_t(params); req[28] = uint8_t(params >> 8);
    req[31] = uint8_t(123 << 4); req[32] = uint8_t(123 >> 4);
    awdl::AddbaRequest r;
    CHECK(awdl::parse_addba_request(req, sizeof(req), own, &r) && r.tid == 5 && r.window == 64 &&
              r.ssn == 123 && r.token == 7 && r.immediate,
          "ADDBA request parsed (tid 5, window 64, ssn 123)");
    uint8_t ans[33];
    awdl::build_addba_response(ans, r, own, 9);
    CHECK(ans[25] == 1 && ans[26] == 7 && std::memcmp(ans + 4, peer, 6) == 0 &&
              (((ans[29] | ans[30] << 8) >> 2) & 15) == 5,
          "ADDBA response built for the same token and tid");

    awdl::BaFilter bf;
    bf.note(1000000, 0);
    CHECK(bf.note(2000001, 40) && bf.off, "busy data turns promiscuous off");
    bf.note(3000002, 41); bf.note(4000003, 42);
    CHECK(bf.note(5000004, 43) && !bf.off, "three quiet seconds turn it back on");

    awdl::RateLadder rl;
    for (int k = 0; k < 32; k++) rl.note(true, false);
    CHECK(rl.step == 1, "32 clean frames step the rate up");
    for (int k = 0; k < 32; k++) rl.note(k % 3 != 0, false);
    CHECK(rl.step == 0, "a third lost steps it back down");
    for (int k = 0; k < 32 * 8; k++) rl.note(true, false);
    CHECK(rl.step == 3, "20 MHz tops out at HT MCS 7");
    for (int k = 0; k < 32 * 8; k++) rl.note(true, true);
    CHECK(rl.step == 6, "80 MHz reaches VHT 2x2 MCS 8");

    awdl::BaTxBulk bulk;
    bulk.note(1000000, 0, false);
    CHECK(!bulk.note(1250000, 23, false) && !bulk.bulk, "BaTxBulk waits for 24 payload frames in 250 ms");
    CHECK(bulk.note(1500000, 48, false) && bulk.bulk, "BaTxBulk triggers at 24+ payload frames in 250 ms");
    for (int k = 1; k <= 3; k++) CHECK(!bulk.note(1500000 + uint64_t(k) * 250000, 48, false) && bulk.bulk, "BaTxBulk holds across brief quiet");
    CHECK(bulk.note(2500000, 48, false) && !bulk.bulk, "BaTxBulk exits after 4 quiet windows (1 s)");
  }

  /* WPA2 crypto against published vectors. */
  {
    const auto hex_eq = [](const uint8_t *p, const char *hex) {
      for (size_t i = 0; hex[2 * i]; i++) {
        unsigned v;
        std::sscanf(hex + 2 * i, "%2x", &v);
        if (p[i] != v) return false;
      }
      return true;
    };
    uint8_t pmk[32];
    CHECK(crypto::pmk_from_passphrase("password", "IEEE", pmk) &&
              hex_eq(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"),
          "PBKDF2 PMK matches IEEE 802.11i H.4 (\"password\"/\"IEEE\")");
    uint8_t key20[20];
    std::memset(key20, 0x0b, 20);
    uint8_t prf[64];
    const char *hi = "Hi There";
    CHECK(crypto::prf_sha1(key20, 20, "prefix", reinterpret_cast<const uint8_t *>(hi), 8, prf, 64) &&
              hex_eq(prf, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                          "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"),
          "PRF-512 matches IEEE 802.11i H.3 test 1");
    uint8_t kek[16], plain[16];
    for (int i = 0; i < 16; i++) {
      kek[i] = static_cast<uint8_t>(i);
      plain[i] = static_cast<uint8_t>(0x11 * i);
    }
    std::vector<uint8_t> wrapped, back;
    CHECK(crypto::aes_wrap(kek, 16, plain, 16, wrapped) &&
              hex_eq(wrapped.data(), "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"),
          "AES key wrap matches RFC 3394 4.1");
    CHECK(crypto::aes_unwrap(kek, 16, wrapped.data(), wrapped.size(), back) && back.size() == 16 &&
              std::memcmp(back.data(), plain, 16) == 0,
          "AES key unwrap round-trips");
    wrapped[3] ^= 1;
    CHECK(!crypto::aes_unwrap(kek, 16, wrapped.data(), wrapped.size(), back),
          "AES key unwrap rejects a corrupted key");

    /* WPA3: AES-CMAC (RFC 4493), P-256, SAE (IEEE 802.11-2020 J.10, as hostap's sae_tests), OWE. */
    auto unhex = [](const char *h, uint8_t *out) {
      for (size_t i = 0; h[2 * i]; i++) {
        unsigned b;
        std::sscanf(h + 2 * i, "%2x", &b);
        out[i] = static_cast<uint8_t>(b);
      }
    };
    uint8_t ck[16], cm[64], tag[16];
    unhex("2b7e151628aed2a6abf7158809cf4f3c", ck);
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
          "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", cm);
    CHECK(wpa3::aes_cmac(ck, cm, 0, tag) && hex_eq(tag, "bb1d6929e95937287fa37d129b756746"), "AES-CMAC RFC 4493 (empty)");
    CHECK(wpa3::aes_cmac(ck, cm, 40, tag) && hex_eq(tag, "dfa66747de9ae63030ca32611497c827"), "AES-CMAC RFC 4493 (40 bytes)");
    CHECK(wpa3::aes_cmac(ck, cm, 64, tag) && hex_eq(tag, "51f0bebf7e3b9d92fc49741779363cfe"), "AES-CMAC RFC 4493 (64 bytes)");
    uint8_t two[32] = {}, g2[64];
    two[31] = 2;
    CHECK(wpa3::p256_mul_base(two, g2) &&
              hex_eq(g2, "7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978"
                         "07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1"),
          "P-256 2G");
    const uint8_t sa1[6] = {0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87}, sa2[6] = {0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c};
    uint8_t srand[32], smask[32], peer[98];
    unhex("992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94", srand);
    unhex("9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322", smask);
    unhex(
        "1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b"
        "5223e71b9bb048d3873f20556953a96c91536fd8ee6ca9b4a68a148b056a909b"
        "e03e83ae208f60f8ef5537858074db06687032399862999b511e0a1552a5fea3"
        "17c2",
        peer);
    wpa3::Sae sae;
    const bool sae_ok = sae.prepare(sa1, sa2, "mekmitasdigoat") && sae.commit(srand, smask);
    const std::vector<uint8_t> body = sae.commit_body({});
    CHECK(sae_ok && body.size() == 98 &&
              hex_eq(body.data(),
        "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c257"
        "5c65d5ad9e00829707aa36ba8b859738fc961d08243505f47c035376d7ac4bc8"
        "d7b95083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e1272621325"
        "dbe1"),
          "SAE commit matches IEEE 802.11-2020 J.10");
    CHECK(sae.process_commit(peer, sizeof(peer)) &&
              hex_eq(sae.kck(), "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a") &&
              hex_eq(sae.pmk(), "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59") &&
              hex_eq(sae.pmkid(), "8747a600eea3f9f22475df58ca1e5498"),
          "SAE KCK, PMK and PMKID match J.10");
    CHECK(!sae.process_commit(body.data(), body.size()), "SAE refuses a reflected commit");
    {
      wpa3::Sae a, b;
      bool ok = a.prepare(sa1, sa2, "correct horse") && b.prepare(sa2, sa1, "correct horse") && a.commit() && b.commit();
      const auto ba = a.commit_body({}), bb = b.commit_body({});
      uint8_t ca[32], cb[32];
      ok = ok && a.process_commit(bb.data(), bb.size()) && b.process_commit(ba.data(), ba.size()) && a.confirm(1, ca) &&
           b.confirm(1, cb);
      CHECK(ok && std::memcmp(a.pmk(), b.pmk(), 32) == 0 && a.verify(1, cb) && b.verify(1, ca),
            "SAE peers with random values agree and confirm");
    }
    wpa3::Owe owe_a, owe_b;
    CHECK(owe_a.generate() && owe_b.generate() && owe_a.derive(owe_b.public_x()), "OWE key agreement");
  }

  /* Radio backends: capability matrix, per-capability policies, and a
   * station-less backend meeting the contract without stubs. */
  failures += radio_backend_test::run();

  /* Out-of-order completion: entries behind an older frame must not go back
   * into the hardware window, regardless of their previous retry deadline. */
  {
    awdl::UnicastRetry head, acked, failed;
    head.slot = 4;
    head.attempts = 1;
    head.first_us = head.sent_us = 100;
    acked.acked = true;
    failed.abandon = true;
    CHECK(!head.ready(101) && !acked.ready(101) && !failed.ready(101),
          "out-of-order completions stay out of the TX window");
    CHECK(!head.status_missing(100 + awdl::kTxStatusTimeoutUS - 1) &&
              head.status_missing(100 + awdl::kTxStatusTimeoutUS) &&
              head.expired(100 + awdl::kTxStatusTimeoutUS),
          "missing TX completion is recognized after the retry window");
    head.slot = -1;
    head.next_us = 2100;
    CHECK(!head.ready(2099) && head.ready(2100) && !head.expired(2100),
          "an unconfirmed frame still retries within its backoff and window");
    CHECK(head.expired(100 + awdl::kResendWindowUS),
          "expired retries cannot be resubmitted after a long pause");
    head.attempts = awdl::kResendMaxAttempts;
    CHECK(head.expired(2100), "unicast also bounds the number of retries");
  }

  /* Experimental A-MPDU send fallback (#4): trip only after two full windows
   * with zero Block Acks and >= 64 single-frame completions. */
  CHECK(!awdl::ba_tx_stalled_as_singles(127, 0, 127), "no-BA fallback waits for 128 frames");
  CHECK(!awdl::ba_tx_stalled_as_singles(128, 0, 63), "no-BA fallback waits for 64 single completions");
  CHECK(!awdl::ba_tx_stalled_as_singles(256, 1, 200), "no-BA fallback stays quiet once any Block Ack arrives");
  CHECK(awdl::ba_tx_stalled_as_singles(128, 0, 64), "no-BA fallback triggers on 128 frames / 0 BA / 64 singles");

  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
