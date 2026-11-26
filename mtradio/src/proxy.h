// SPDX-License-Identifier: GPL-2.0-only
/* Client of the register-proxy driver (driver/regproxy): the UMDF driver bound
 * to the Wi-Fi card maps its register BAR and performs reads and writes for us.
 * One call costs a system round trip (~30 us), so callers that can batch do. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RegOp {
  uint32_t offset;
  uint32_t value;
  uint8_t kind; /* 0 read, 1 write, 2 set bits, 3 clear bits */
};

class RegProxy {
public:
  RegProxy() = default;
  ~RegProxy();
  RegProxy(const RegProxy &) = delete;
  RegProxy &operator=(const RegProxy &) = delete;

  /* Opens the one card bound to the proxy. `error` says why not. */
  bool open(std::string &error);
  bool is_open() const { return _handle != nullptr; }

  uint16_t vendor() const { return _vendor; }
  uint16_t device() const { return _device; }
  uint16_t subsystem() const { return _subsystem; }
  uint32_t bar_len() const { return _bar_len; }

  uint32_t read32(uint32_t off);
  void write32(uint32_t off, uint32_t value);
  void write8(uint32_t off, uint8_t value);
  void set_bits(uint32_t off, uint32_t bits);
  void clear_bits(uint32_t off, uint32_t bits);
  /* Runs the ops in one call; reads come back in `value`. */
  bool batch(std::vector<RegOp> &ops);
  /* Writes the driver replays when our handle closes, however we die. Used to
   * stop the card's DMA before Windows reuses the pages it writes into. */
  bool set_close_ops(const std::vector<RegOp> &ops);

  /* True once any call failed: the card is gone or the driver refused. */
  bool failed() const { return _failed; }

private:
  void *_handle = nullptr;
  uint16_t _vendor = 0, _device = 0, _subsystem = 0;
  uint32_t _bar_len = 0;
  bool _failed = false;
};
