// SPDX-License-Identifier: GPL-2.0-only
/* Host CCMP against IEEE 802.11-2016 Annex J.6.4's test MPDU, then a QoS
 * round trip, a tampered frame and a management (PMF) frame. */
#include <cstdio>
#include <cstring>
#include <vector>

#include "ccmp.h"

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  failures += !ok;
}

} // namespace

int main() {
  const uint8_t tk[16] = {0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85,
                          0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd5, 0x2f};
  const uint64_t pn = 0xB5039776E70Cull;
  /* The header with the retry bit, before the protected bit is set. */
  const uint8_t plain[] = {0x08, 0x08, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84,
                           0x44, 0x08, 0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33, 0xf8, 0xba, 0x1a, 0x55,
                           0xd0, 0x2f, 0x85, 0xae, 0x96, 0x7b, 0xb6, 0x2f, 0xb6, 0xcd, 0xa8, 0xeb, 0x7e, 0x78,
                           0xa0, 0x50};
  const uint8_t sealed[] = {0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84,
                            0x44, 0x08, 0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33, 0x0c, 0xe7, 0x00, 0x20,
                            0x76, 0x97, 0x03, 0xb5, 0xf3, 0xd0, 0xa2, 0xfe, 0x9a, 0x3d, 0xbf, 0x23, 0x42, 0xa6,
                            0x43, 0xe4, 0x32, 0x46, 0xe8, 0x0c, 0x3c, 0x04, 0xd0, 0x19, 0x78, 0x45, 0xce, 0x0b,
                            0x16, 0xf9, 0x76, 0x23};
  ccmp::Key key;
  check(key.set(tk), "key accepted by CNG");
  std::vector<uint8_t> out;
  check(key.seal(plain, sizeof(plain), pn, 0, out) && out.size() == sizeof(sealed) &&
            std::memcmp(out.data(), sealed, sizeof(sealed)) == 0,
        "J.6.4 test MPDU sealed byte for byte");
  check(ccmp::packet_number(sealed + 24) == pn, "packet number read from the CCMP header");
  std::vector<uint8_t> opened;
  check(key.open(sealed, sizeof(sealed), opened) && opened.size() == sizeof(plain) + 8 &&
            std::memcmp(opened.data() + 32, plain + 24, sizeof(plain) - 24) == 0,
        "J.6.4 test MPDU opened");
  std::vector<uint8_t> bad(sealed, sealed + sizeof(sealed));
  bad[40] ^= 1;
  check(!key.open(bad.data(), bad.size(), opened), "a flipped bit fails the MIC");

  /* QoS data, TID 5, from DS, 1500 bytes: round trip. */
  std::vector<uint8_t> qos = {0x88, 0x02, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                              0x10, 0x00, 0x05, 0x00};
  for (int i = 0; i < 1500; i++) qos.push_back(static_cast<uint8_t>(i * 7));
  check(key.seal(qos.data(), qos.size(), 42, 0, out) && key.open(out.data(), out.size(), opened) &&
            opened.size() == qos.size() + 8 && std::memcmp(opened.data() + 34, qos.data() + 26, 1500) == 0,
        "QoS data round trip");
  bad = out;
  bad[24] ^= 0x01; /* the TID is in the nonce and the AAD */
  check(!key.open(bad.data(), bad.size(), opened), "a changed TID fails the MIC");

  /* A protected deauthentication (PMF): the nonce's management bit. */
  std::vector<uint8_t> deauth = {0xc0, 0x00, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                                 0x20, 0x00, 0x07, 0x00};
  check(key.seal(deauth.data(), deauth.size(), 7, 0, out) && key.open(out.data(), out.size(), opened) &&
            opened.size() == deauth.size() + 8 && opened[32] == 0x07,
        "protected management frame round trip");

  std::printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
