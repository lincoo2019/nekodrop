// SPDX-License-Identifier: GPL-2.0-only
/* Includes the register header (third_party/iwx/if_iwxreg.h) with the layout
 * the firmware expects under either compiler. With clang its __packed
 * attributes do the work. With MSVC every struct is packed by #pragma pack(1);
 * the six structs there without __packed have no padding to lose (all their
 * fields fall on natural boundaries), so the layout is the same, which
 * tests/selftest checks against the iwlwifi sizes. */
#pragma once

#include "compat.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4200 4201 4505 4214)
#pragma pack(push, 1)
#endif
#include "../third_party/iwx/if_iwxreg.h"
/* The AX210 family's receive transfer descriptor, which itlwm defines in
 * ItlIwx.cpp rather than the header (Linux: struct iwl_rx_transfer_desc). */
struct iwx_rx_transfer_desc {
  uint16_t rbid;
  uint16_t reserved[3];
  uint64_t addr;
} __packed;
#include "iwx_mld.h"
#ifdef _MSC_VER
#pragma pack(pop)
#pragma warning(pop)
#endif
