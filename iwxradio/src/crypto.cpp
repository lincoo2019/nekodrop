// SPDX-License-Identifier: GPL-2.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>

#include <cstring>

#include "crypto.h"

#pragma comment(lib, "bcrypt.lib")

namespace crypto {

namespace {

bool hmac(LPCWSTR alg, const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
          uint8_t *out, ULONG out_len) {
  BCRYPT_ALG_HANDLE h = nullptr;
  if (BCryptOpenAlgorithmProvider(&h, alg, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return false;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptCreateHash(h, &hash, nullptr, 0, const_cast<PUCHAR>(key), static_cast<ULONG>(key_len),
                             0) == 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0) == 0 &&
            BCryptFinishHash(hash, out, out_len, 0) == 0;
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(h, 0);
  return ok;
}

/* AES-ECB on single 16-byte blocks with one key. */
class AesEcb {
public:
  AesEcb(const uint8_t *key, size_t len) {
    if (BCryptOpenAlgorithmProvider(&_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return;
    BCryptSetProperty(_alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                      sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    if (BCryptGenerateSymmetricKey(_alg, &_key, nullptr, 0, const_cast<PUCHAR>(key),
                                   static_cast<ULONG>(len), 0) != 0)
      _key = nullptr;
  }
  ~AesEcb() {
    if (_key) BCryptDestroyKey(_key);
    if (_alg) BCryptCloseAlgorithmProvider(_alg, 0);
  }
  bool ok() const { return _key != nullptr; }
  bool encrypt(const uint8_t in[16], uint8_t out[16]) {
    ULONG got = 0;
    return BCryptEncrypt(_key, const_cast<PUCHAR>(in), 16, nullptr, nullptr, 0, out, 16, &got, 0) == 0;
  }
  bool decrypt(const uint8_t in[16], uint8_t out[16]) {
    ULONG got = 0;
    return BCryptDecrypt(_key, const_cast<PUCHAR>(in), 16, nullptr, nullptr, 0, out, 16, &got, 0) == 0;
  }

private:
  BCRYPT_ALG_HANDLE _alg = nullptr;
  BCRYPT_KEY_HANDLE _key = nullptr;
};

} // namespace

bool pmk_from_passphrase(const std::string &passphrase, const std::string &ssid, uint8_t pmk[32]) {
  BCRYPT_ALG_HANDLE h = nullptr;
  if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
    return false;
  const bool ok =
      BCryptDeriveKeyPBKDF2(h, (PUCHAR)passphrase.data(), static_cast<ULONG>(passphrase.size()),
                            (PUCHAR)ssid.data(), static_cast<ULONG>(ssid.size()), 4096, pmk, 32, 0) == 0;
  BCryptCloseAlgorithmProvider(h, 0);
  return ok;
}

bool hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[20]) {
  return hmac(BCRYPT_SHA1_ALGORITHM, key, key_len, data, len, out, 20);
}

bool hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]) {
  return hmac(BCRYPT_SHA256_ALGORITHM, key, key_len, data, len, out, 32);
}

bool prf_sha1(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data,
              size_t data_len, uint8_t *out, size_t out_len) {
  const size_t label_len = std::strlen(label);
  std::vector<uint8_t> buf(label_len + 1 + data_len + 1);
  std::memcpy(buf.data(), label, label_len);
  buf[label_len] = 0;
  std::memcpy(buf.data() + label_len + 1, data, data_len);
  size_t pos = 0;
  for (uint8_t i = 0; pos < out_len; i++) {
    buf.back() = i;
    uint8_t digest[20];
    if (!hmac_sha1(key, key_len, buf.data(), buf.size(), digest)) return false;
    const size_t n = std::min<size_t>(20, out_len - pos);
    std::memcpy(out + pos, digest, n);
    pos += n;
  }
  return true;
}

bool aes_unwrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t in_len,
                std::vector<uint8_t> &out) {
  if (in_len < 24 || in_len % 8) return false;
  AesEcb aes(kek, kek_len);
  if (!aes.ok()) return false;
  const size_t n = in_len / 8 - 1;
  uint8_t a[8];
  std::memcpy(a, in, 8);
  out.assign(in + 8, in + in_len);
  for (int j = 5; j >= 0; j--) {
    for (size_t i = n; i >= 1; i--) {
      uint8_t b[16];
      std::memcpy(b, a, 8);
      const uint64_t t = static_cast<uint64_t>(n) * j + i;
      for (int k = 0; k < 8; k++) b[7 - k] ^= static_cast<uint8_t>(t >> (8 * k));
      std::memcpy(b + 8, out.data() + (i - 1) * 8, 8);
      uint8_t r[16];
      if (!aes.decrypt(b, r)) return false;
      std::memcpy(a, r, 8);
      std::memcpy(out.data() + (i - 1) * 8, r + 8, 8);
    }
  }
  for (int k = 0; k < 8; k++)
    if (a[k] != 0xa6) return false; /* integrity check failed */
  return true;
}

bool aes_wrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t in_len,
              std::vector<uint8_t> &out) {
  if (in_len < 16 || in_len % 8) return false;
  AesEcb aes(kek, kek_len);
  if (!aes.ok()) return false;
  const size_t n = in_len / 8;
  uint8_t a[8];
  std::memset(a, 0xa6, 8);
  std::vector<uint8_t> r(in, in + in_len);
  for (int j = 0; j <= 5; j++) {
    for (size_t i = 1; i <= n; i++) {
      uint8_t b[16], e[16];
      std::memcpy(b, a, 8);
      std::memcpy(b + 8, r.data() + (i - 1) * 8, 8);
      if (!aes.encrypt(b, e)) return false;
      const uint64_t t = static_cast<uint64_t>(n) * j + i;
      std::memcpy(a, e, 8);
      for (int k = 0; k < 8; k++) a[7 - k] ^= static_cast<uint8_t>(t >> (8 * k));
      std::memcpy(r.data() + (i - 1) * 8, e + 8, 8);
    }
  }
  out.assign(a, a + 8);
  out.insert(out.end(), r.begin(), r.end());
  return true;
}

bool sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  BCRYPT_ALG_HANDLE h = nullptr;
  if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
  BCRYPT_HASH_HANDLE hash = nullptr;
  const bool ok = BCryptCreateHash(h, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
                  BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0) == 0 &&
                  BCryptFinishHash(hash, out, 32, 0) == 0;
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(h, 0);
  return ok;
}

bool aes_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  AesEcb aes(key, 16);
  return aes.ok() && aes.encrypt(in, out);
}

bool random_bytes(uint8_t *out, size_t len) {
  return BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

} // namespace crypto
