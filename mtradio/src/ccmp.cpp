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

#include "ccmp.h"

namespace ccmp {

namespace {

bool is_qos_data(const uint8_t *f) { return (f[0] & 0x0c) == 0x08 && (f[0] & 0x80); }

/* AAD (12.5.3.3.3) and nonce (12.5.3.3.4) of a frame whose header is `h` bytes. */
void aad_and_nonce(const uint8_t *f, size_t h, uint64_t pn, std::vector<uint8_t> &aad, uint8_t nonce[13]) {
  const bool data = (f[0] & 0x0c) == 0x08;
  const bool a4 = (f[1] & 0x03) == 0x03;
  aad.clear();
  /* Frame control: subtype bits 4-6 masked in data frames; retry, power
   * management and more data masked; protected set; order masked in QoS data. */
  uint8_t fc0 = f[0], fc1 = f[1];
  if (data) fc0 &= 0x8f;
  fc1 &= static_cast<uint8_t>(~(0x08 | 0x10 | 0x20));
  fc1 |= 0x40;
  if (is_qos_data(f)) fc1 &= 0x7f;
  aad.push_back(fc0);
  aad.push_back(fc1);
  aad.insert(aad.end(), f + 4, f + 22); /* A1, A2, A3 */
  aad.push_back(static_cast<uint8_t>(f[22] & 0x0f)); /* fragment number only */
  aad.push_back(0);
  size_t at = 24;
  if (a4) {
    aad.insert(aad.end(), f + 24, f + 30);
    at = 30;
  }
  uint8_t tid = 0;
  if (is_qos_data(f) && at + 2 <= h) {
    tid = f[at] & 0x0f;
    aad.push_back(tid);
    aad.push_back(0);
  }
  /* Nonce flags: priority, and the management bit for a protected
   * management frame (PMF). */
  nonce[0] = static_cast<uint8_t>(data ? tid : 0x10);
  std::memcpy(nonce + 1, f + 10, 6);
  for (int i = 0; i < 6; i++) nonce[7 + i] = static_cast<uint8_t>(pn >> (8 * (5 - i)));
}

} // namespace

size_t header_len(const uint8_t *f, size_t len) {
  if (len < 24) return 0;
  size_t h = 24;
  if ((f[0] & 0x0c) == 0x08) {
    if ((f[1] & 0x03) == 0x03) h += 6;
    if (f[0] & 0x80) {
      h += 2;
      if (f[1] & 0x80) h += 4;
    }
  }
  return h <= len ? h : 0;
}

uint64_t packet_number(const uint8_t hdr[8]) {
  return static_cast<uint64_t>(hdr[0]) | static_cast<uint64_t>(hdr[1]) << 8 | static_cast<uint64_t>(hdr[4]) << 16 |
         static_cast<uint64_t>(hdr[5]) << 24 | static_cast<uint64_t>(hdr[6]) << 32 |
         static_cast<uint64_t>(hdr[7]) << 40;
}

Key::~Key() { clear(); }

void Key::clear() {
  if (_key) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(_key));
  if (_alg) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(_alg), 0);
  _key = _alg = nullptr;
}

bool Key::set(const uint8_t tk[16]) {
  clear();
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
  _alg = alg;
  if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_CCM)),
                        sizeof(BCRYPT_CHAIN_MODE_CCM), 0) < 0) {
    clear();
    return false;
  }
  BCRYPT_KEY_HANDLE key = nullptr;
  if (BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, const_cast<PUCHAR>(tk), 16, 0) < 0) {
    clear();
    return false;
  }
  _key = key;
  return true;
}

bool Key::seal(const uint8_t *plain, size_t len, uint64_t pn, unsigned key_id, std::vector<uint8_t> &out) const {
  const size_t h = header_len(plain, len);
  if (!_key || !h) return false;
  out.assign(plain, plain + h);
  out[1] |= 0x40;
  const uint8_t ccmp_hdr[8] = {static_cast<uint8_t>(pn), static_cast<uint8_t>(pn >> 8), 0,
                               static_cast<uint8_t>(0x20 | (key_id & 3) << 6),
                               static_cast<uint8_t>(pn >> 16), static_cast<uint8_t>(pn >> 24),
                               static_cast<uint8_t>(pn >> 32), static_cast<uint8_t>(pn >> 40)};
  out.insert(out.end(), ccmp_hdr, ccmp_hdr + 8);
  std::vector<uint8_t> aad;
  uint8_t nonce[13];
  aad_and_nonce(out.data(), h, pn, aad, nonce);
  const size_t body = len - h;
  out.resize(h + 8 + body + kMicLen);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = nonce;
  info.cbNonce = sizeof(nonce);
  info.pbAuthData = aad.data();
  info.cbAuthData = static_cast<ULONG>(aad.size());
  info.pbTag = out.data() + h + 8 + body;
  info.cbTag = kMicLen;
  ULONG done = 0;
  return BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(_key), const_cast<PUCHAR>(plain + h), static_cast<ULONG>(body),
                       &info, nullptr, 0, out.data() + h + 8, static_cast<ULONG>(body), &done, 0) >= 0 &&
         done == body;
}

bool Key::open(const uint8_t *frame, size_t len, std::vector<uint8_t> &out) const {
  const size_t h = header_len(frame, len);
  if (!_key || !h || len < h + kHeaderLen + kMicLen || !(frame[h + 3] & 0x20)) return false;
  const uint64_t pn = packet_number(frame + h);
  std::vector<uint8_t> aad;
  uint8_t nonce[13];
  aad_and_nonce(frame, h, pn, aad, nonce);
  const size_t body = len - h - kHeaderLen - kMicLen;
  out.assign(frame, frame + h + kHeaderLen);
  out.resize(h + kHeaderLen + body);
  uint8_t tag[kMicLen];
  std::memcpy(tag, frame + len - kMicLen, kMicLen);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = nonce;
  info.cbNonce = sizeof(nonce);
  info.pbAuthData = aad.data();
  info.cbAuthData = static_cast<ULONG>(aad.size());
  info.pbTag = tag;
  info.cbTag = kMicLen;
  ULONG done = 0;
  return BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(_key), const_cast<PUCHAR>(frame + h + kHeaderLen),
                       static_cast<ULONG>(body), &info, nullptr, 0, out.data() + h + kHeaderLen,
                       static_cast<ULONG>(body), &done, 0) >= 0 &&
         done == body;
}

} // namespace ccmp
