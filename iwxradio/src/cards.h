// SPDX-License-Identifier: GPL-2.0-only
/* Which Intel card this is and how to drive it: itlwm's iwx_devices table
 * (transport parameters by PCI device ID) and iwl_dev_info_table (firmware by
 * MAC type, step and radio type), reduced to the cards whose firmware
 * ForbiddenItlwm ships (API 68). Pure data, so the self-test can check it. */
#pragma once

#include <cstdint>
#include <string>

struct CardInfo {
  bool known = false;     /* false: `why` says what is missing */
  std::string why;
  std::string name;       /* "Intel(R) Wi-Fi 6 AX201 160MHz" */
  std::string firmware;   /* iwlwifi-*.ucode */
  std::string pnvm;       /* iwlwifi-*.pnvm, AX210 family with a GF radio; else empty */
  bool ax210 = false;     /* AX210 family: gen3 context info, PNVM, gen3 TX/RX formats */
  bool bz = false;        /* Bz family (BE200): BZ CSR bits, 4B RX completion, 8 KiB BC table, MLD API */
  bool integrated = true; /* CNVi (MAC in the chipset) or discrete */
  uint32_t xtal_latency = 0;
  uint32_t ltr_delay = 0; /* IWX_SOC_FLAGS_LTR_APPLY_DELAY_* */
  bool low_latency_xtal = false;
  bool imr = false;       /* IWX_PRPH_SCRATCH_IMR_DEBUG_EN (Linux's *_imr_trans_cfg) */
  bool tested = false;    /* run on real hardware by the project */
};

namespace cards {

/* IWX_SOC_FLAGS_LTR_APPLY_DELAY_* values */
constexpr uint32_t kLtrNone = 0, kLtr200 = 1, kLtr2500 = 2, kLtr1820 = 3;

constexpr uint32_t mac_type(uint32_t hw_rev) { return (hw_rev & 0x000FFF0) >> 4; }
constexpr uint32_t mac_step(uint32_t hw_rev) { return (hw_rev & 0x000000C) >> 2; }
constexpr uint32_t rf_type(uint32_t rf_id) { return (rf_id & 0x0FFF000) >> 12; }
constexpr bool rf_cdb(uint32_t rf_id) { return (rf_id >> 28) & 1; }

/* The PCI device IDs nekodrop binds to (the register proxy's INF lists the same). */
constexpr uint16_t kDeviceIds[] = {0x02F0, 0x06F0, 0x34F0, 0x3DF0, 0x4DF0, 0x43F0, 0xA0F0, 0x2723,
                                   0x2725, 0x7A70, 0x7AF0, 0x51F0, 0x51F1, 0x54F0, 0x7F70, 0x7740};

/* --family <name>: a representative device ID of each family, for a card
 * the user matched to a model by hand (Settings, "Wi-Fi card"). The keys are
 * the service's CardCatalog.ManualModels for Intel; 0 for an unknown name. */
inline uint16_t family_device(const std::string &name) {
  if (name == "qu") return 0x06F0;    /* iwl_qu_trans_cfg */
  if (name == "ax200") return 0x2723; /* discrete 22000 */
  if (name == "ax210") return 0x2725; /* discrete Ty */
  if (name == "so") return 0x51F0;    /* So CNVi, long-latency (the safe one) */
  if (name == "be200") return 0x272B; /* discrete Bz/Gl (experimental) */
  return 0;
}

/* `hw_rev` as iwx keeps it: CSR_HW_REV with the step moved to bits 2-3. */
inline CardInfo select(uint16_t device, uint32_t hw_rev, uint32_t rf_id) {
  CardInfo c;
  /* Transport parameters (itlwm's iwl_*_trans_cfg, by PCI device ID). */
  switch (device) {
  case 0x02F0: case 0x06F0: /* iwl_qu_trans_cfg */
    c.xtal_latency = 5000; c.ltr_delay = kLtr200; break;
  case 0x34F0: case 0x3DF0: case 0x4DF0: /* iwl_qu_medium_latency_trans_cfg */
    c.xtal_latency = 1820; c.ltr_delay = kLtr1820; break;
  case 0x43F0: case 0xA0F0: /* iwl_qu_long_latency_trans_cfg */
    c.xtal_latency = 12000; c.ltr_delay = kLtr2500; c.low_latency_xtal = true; break;
  case 0x2723: /* iwl_ax200_trans_cfg: discrete */
    c.integrated = false; break;
  case 0x2725: /* iwl_ma_trans_cfg: the discrete AX210 (Linux and OpenBSD alike) */
    c.ax210 = true; c.integrated = false; break;
  case 0x7AF0: case 0x7F70: /* iwl_so_trans_cfg */
    c.ax210 = true; c.xtal_latency = 500; c.ltr_delay = kLtr200; break;
  case 0x51F0: case 0x54F0: /* iwl_so_long_latency_trans_cfg */
    c.ax210 = true; c.xtal_latency = 12000; c.ltr_delay = kLtr2500; c.low_latency_xtal = true; break;
  case 0x7A70: case 0x51F1: /* iwl_so_long_latency_imr_trans_cfg (OpenBSD: sc_imr_enabled) */
    c.ax210 = true; c.xtal_latency = 12000; c.ltr_delay = kLtr2500; c.low_latency_xtal = true;
    c.imr = true; break;
  case 0x272B: /* iwl_gl_mac_cfg: discrete BE200 (Bz family, experimental) */
    c.ax210 = true; c.bz = true; c.integrated = false;
    c.xtal_latency = 12000; c.low_latency_xtal = true; break;
  case 0x7740: /* iwl_bz_mac_cfg: Bz on Arrow Lake (AX203) */
    c.ax210 = true; c.bz = true;
    c.xtal_latency = 12000; c.ltr_delay = kLtr2500; c.low_latency_xtal = true; break;
  case 0x2720: c.why = "Intel QnJ (8086:2720): its firmware (QuQnj, API 48) is not shipped upstream"; return c;
  case 0x2726: c.why = "Intel SnJ (8086:2726): its firmware (SoSnj, API 63) is not shipped upstream"; return c;
  case 0x2729: case 0x7E40: c.why = "Intel Ma (Meteor Lake CNVi): its firmware (ma, API 63) is not shipped upstream"; return c;
  default: c.why = "not an Intel Wi-Fi 6/6E card nekodrop knows"; return c;
  }

  const uint32_t mac = mac_type(hw_rev), rf = rf_type(rf_id);
  const bool jf = rf == 0x105 || rf == 0x108; /* JF2, JF1: Wireless-AC 9560 class radio */
  const bool hr = rf == 0x10A || rf == 0x10C; /* HR2, HR1: Wi-Fi 6 */
  const bool gf = rf == 0x10D;                /* GF: Wi-Fi 6E */
  const bool fm = rf == 0x112;                /* FM: Wi-Fi 7 (BE200) */
  if (device == 0x2723) {
    c.firmware = "iwlwifi-cc-a0-68.ucode";
    c.name = "Intel(R) Wi-Fi 6 AX200 160MHz";
  } else if (mac == 0x33 || mac == 0x35) { /* Qu, QuZ */
    if (c.ax210) { c.why = "a Qu MAC on an AX210-family device ID"; return c; }
    const std::string base = mac == 0x35 ? "iwlwifi-QuZ-a0-" : ((hw_rev & 0xfffc) == 0x338 ? "iwlwifi-Qu-c0-" : "iwlwifi-Qu-b0-");
    if (hr) {
      c.firmware = base + "hr-b0-68.ucode";
      c.name = "Intel(R) Wi-Fi 6 AX201 160MHz";
      c.tested = mac == 0x35; /* the project's own AX201 is QuZ */
    } else if (jf) {
      c.firmware = base + "jf-b0-68.ucode";
      c.name = "Intel(R) Wireless-AC 9560/9462/9461";
    } else {
      c.why = "unknown radio type " + std::to_string(rf) + " on a Qu MAC"; return c;
    }
  } else if (mac == 0x37 || mac == 0x43) { /* So, SoF */
    if (!c.ax210 || c.bz) { c.why = "an So MAC on a non-AX210 device ID"; return c; }
    if (gf && rf_cdb(rf_id)) {
      c.firmware = "iwlwifi-so-a0-gf4-a0-68.ucode"; c.pnvm = "iwlwifi-so-a0-gf4-a0.pnvm";
      c.name = "Intel(R) Wi-Fi 6E AX411 160MHz";
    } else if (gf) {
      c.firmware = "iwlwifi-so-a0-gf-a0-68.ucode"; c.pnvm = "iwlwifi-so-a0-gf-a0.pnvm";
      c.name = device == 0x2725 ? "Intel(R) Wi-Fi 6E AX210 160MHz" : "Intel(R) Wi-Fi 6E AX211 160MHz";
    } else if (hr) {
      c.firmware = "iwlwifi-so-a0-hr-b0-68.ucode";
      c.name = "Intel(R) Wi-Fi 6 AX203/AX201 (So)";
    } else if (jf) {
      c.firmware = "iwlwifi-so-a0-jf-b0-68.ucode";
      c.name = "Intel(R) Wireless-AC 9560 (So)";
    } else {
      c.why = "unknown radio type " + std::to_string(rf) + " on an So MAC"; return c;
    }
  } else if (mac == 0x42 && device == 0x2725) { /* Ty: the discrete AX210 */
    c.firmware = "iwlwifi-ty-a0-gf-a0-68.ucode"; c.pnvm = "iwlwifi-ty-a0-gf-a0.pnvm";
    c.name = "Intel(R) Wi-Fi 6E AX210 160MHz";
  } else if (mac == 0x47 && device == 0x272B) { /* Gl: the discrete BE200 (Bz family, experimental) */
    if (fm) {
      c.firmware = "iwlwifi-gl-c0-fm-c0-100.ucode";
      c.name = "Intel(R) Wi-Fi 7 BE200 320MHz";
    } else {
      c.why = "unknown radio type " + std::to_string(rf) + " on a Gl MAC"; return c;
    }
  } else if (c.bz) { /* Bz MAC (0x46) on the Arrow Lake AX203 (device 0x7740) */
    if (mac != 0x46) {
      c.why = "a non-Bz MAC (" + std::to_string(mac) + ") on a Bz device ID";
      return c;
    }
    if (!hr) {
      c.why = "Bz with RF type " + std::to_string(rf) + ", only the HR radio (AX203) is ported";
      return c;
    }
    /* Intel's Bz firmware for the HR radio. The file's "100" is the packaging
     * version; its core is 97, the newest published for this RF. It brings its
     * own image loader (IML). A PNVM ships as iwlwifi-bz-b0-hr-b0.pnvm and is
     * loaded after ALIVE (Linux iwl_mld_run_fw_init_sequence). */
    c.firmware = "iwlwifi-bz-b0-hr-b0-100.ucode";
    c.pnvm = "iwlwifi-bz-b0-hr-b0.pnvm";
    c.name = "Intel(R) Wi-Fi 6 AX203";
    c.tested = true;
  } else {
    c.why = "MAC type " + std::to_string(mac) + " on device " + std::to_string(device) +
            " has no firmware upstream";
    return c;
  }
  c.known = true;
  return c;
}

} // namespace cards
