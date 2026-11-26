// SPDX-License-Identifier: GPL-2.0-only
/*
 * WPA3 and friends for the station: NIST P-256 (group 19), SAE with hunting
 * and pecking (IEEE 802.11-2020 12.4), OWE (RFC 8110), the SHA-256 KDF
 * (12.7.1.7.2) and AES-CMAC (RFC 4493). Portable C++ on the crypto.h
 * primitives; checked against IEEE 802.11-2020 Annex J.10 in the self-test.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wpa3 {

/* KDF-Hash-Length with HMAC-SHA256: `bits` of output (a multiple of 8). */
bool kdf_sha256(const uint8_t *key, size_t key_len, const char *label, const uint8_t *context,
                size_t context_len, uint8_t *out, size_t bits);
bool aes_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t out[16]);

/* A point on P-256 in big-endian x || y. */
struct Element {
  uint8_t xy[64] = {};
};

/* One SAE exchange, from our side (group 19, hunting and pecking). */
class Sae {
public:
  /* Derives PWE from the password and both addresses; false if none was found. */
  bool prepare(const uint8_t own[6], const uint8_t peer[6], const std::string &password);
  /* A fresh commit: random rand and mask unless given (the self-test's vector). */
  bool commit(const uint8_t *rand = nullptr, const uint8_t *mask = nullptr);
  /* group (LE16 19) || token || scalar || element: the commit's body. */
  std::vector<uint8_t> commit_body(const std::vector<uint8_t> &token) const;
  /* The peer's commit body (group, scalar, element; no token): false if invalid or reflected.
   * On success the KCK, PMK and PMKID are ready. */
  bool process_commit(const uint8_t *body, size_t len);
  /* Our confirm for send-confirm `sc`, and the check of the peer's. */
  bool confirm(uint16_t sc, uint8_t out[32]) const;
  bool verify(uint16_t peer_sc, const uint8_t peer_confirm[32]) const;

  const uint8_t *pmk() const { return _pmk; }
  const uint8_t *pmkid() const { return _pmkid; }
  const uint8_t *kck() const { return _kck; }

private:
  uint8_t _pwe[64] = {};
  bool _have_pwe = false;
  uint8_t _rand[32] = {}, _scalar[32] = {}, _element[64] = {};
  uint8_t _peer_scalar[32] = {}, _peer_element[64] = {};
  uint8_t _kck[32] = {}, _pmk[32] = {}, _pmkid[16] = {};
};

/* OWE with group 19: our key pair, then the PMK from the AP's public x. */
class Owe {
public:
  bool generate(const uint8_t *priv = nullptr);
  const uint8_t *public_x() const { return _pub; }
  /* The AP's 32-byte public x: PMK = HKDF(C | A | group, z). */
  bool derive(const uint8_t peer_x[32]);
  const uint8_t *pmk() const { return _pmk; }

private:
  uint8_t _priv[32] = {}, _pub[32] = {}, _pmk[32] = {};
};

/* Self-test helpers: P-256 scalar multiple of the generator, as x || y. */
bool p256_mul_base(const uint8_t scalar[32], uint8_t out[64]);

} // namespace wpa3
