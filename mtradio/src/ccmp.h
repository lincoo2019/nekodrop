// SPDX-License-Identifier: GPL-2.0-only
/* CCMP-128 (IEEE 802.11-2016 12.5.3) on the host, with Windows CNG's AES-CCM:
 * the MT7902 radio runs the card's sniffer and sends frames as given, so the
 * Wi-Fi station's frames are sealed and opened here instead of in the
 * firmware's key table. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ccmp {

constexpr size_t kHeaderLen = 8, kMicLen = 8;

/* The 802.11 header length of a frame (3 or 4 addresses, QoS control, HTC). */
size_t header_len(const uint8_t *f, size_t len);

/* The 48-bit packet number in a CCMP header. */
uint64_t packet_number(const uint8_t hdr[8]);

class Key {
public:
  Key() = default;
  ~Key();
  Key(const Key &) = delete;
  Key &operator=(const Key &) = delete;

  bool set(const uint8_t tk[16]);
  void clear();
  bool valid() const { return _key != nullptr; }

  /* `plain`: a frame without the protected bit. Out: the same header with
   * the protected bit, the CCMP header (PN, key ID), the sealed body and the
   * MIC. */
  bool seal(const uint8_t *plain, size_t len, uint64_t pn, unsigned key_id, std::vector<uint8_t> &out) const;
  /* `frame`: a protected frame with its CCMP header and MIC. Out: header and
   * CCMP header as received, then the plaintext body (no MIC). */
  bool open(const uint8_t *frame, size_t len, std::vector<uint8_t> &out) const;

private:
  void *_alg = nullptr, *_key = nullptr;
};

} // namespace ccmp
