// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "iwxreg.h"

/* The primary channel's position relative to the 80 MHz center. Keep this
 * geometry separate from the firmware's regulatory check. */
constexpr int iwx_primary80_position(unsigned channel) {
  constexpr unsigned starts[] = {36, 52, 100, 116, 132, 149, 165};
  constexpr int positions[] = {IWX_PHY_VHT_CTRL_POS_2_BELOW, IWX_PHY_VHT_CTRL_POS_1_BELOW,
                               IWX_PHY_VHT_CTRL_POS_1_ABOVE, IWX_PHY_VHT_CTRL_POS_2_ABOVE};
  for (unsigned lo : starts)
    if (channel >= lo && channel <= lo + 12 && (channel - lo) % 4 == 0)
      return positions[(channel - lo) / 4];
  return -1;
}
