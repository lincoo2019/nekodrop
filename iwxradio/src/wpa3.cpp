// SPDX-License-Identifier: GPL-2.0-only
/*
 * P-256 in Montgomery form with 32-bit limbs (CIOS multiplication), Jacobian
 * points; SAE and OWE on top. The steps follow hostap's sae.c and
 * wpa.c (owe_process_assoc_resp), which the self-test's vectors also check.
 */
#include "wpa3.h"

#include <cstring>

#include "crypto.h"

namespace wpa3 {

/* ------------------------------------------------------------ KDF, CMAC */

bool kdf_sha256(const uint8_t *key, size_t key_len, const char *label, const uint8_t *context,
                size_t context_len, uint8_t *out, size_t bits) {
  const size_t label_len = std::strlen(label), total = bits / 8;
  std::vector<uint8_t> buf(2 + label_len + context_len + 2);
  std::memcpy(buf.data() + 2, label, label_len);
  if (context_len) std::memcpy(buf.data() + 2 + label_len, context, context_len);
  buf[buf.size() - 2] = static_cast<uint8_t>(bits);
  buf[buf.size() - 1] = static_cast<uint8_t>(bits >> 8);
  size_t pos = 0;
  for (uint16_t counter = 1; pos < total; counter++) {
    buf[0] = static_cast<uint8_t>(counter);
    buf[1] = static_cast<uint8_t>(counter >> 8);
    uint8_t h[32];
    if (!crypto::hmac_sha256(key, key_len, buf.data(), buf.size(), h)) return false;
    const size_t n = total - pos < 32 ? total - pos : 32;
    std::memcpy(out + pos, h, n);
    pos += n;
  }
  return true;
}

namespace {

void cmac_shift(const uint8_t in[16], uint8_t out[16]) {
  const uint8_t msb = in[0] & 0x80;
  for (int i = 0; i < 15; i++) out[i] = static_cast<uint8_t>(in[i] << 1 | in[i + 1] >> 7);
  out[15] = static_cast<uint8_t>(in[15] << 1);
  if (msb) out[15] ^= 0x87;
}

} // namespace

bool aes_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t out[16]) {
  uint8_t zero[16] = {}, l[16], k1[16], k2[16];
  if (!crypto::aes_encrypt_block(key, zero, l)) return false;
  cmac_shift(l, k1);
  cmac_shift(k1, k2);
  size_t blocks = (len + 15) / 16;
  const bool whole = blocks > 0 && len % 16 == 0;
  if (blocks == 0) blocks = 1;
  uint8_t x[16] = {}, y[16];
  for (size_t b = 0; b + 1 < blocks; b++) {
    for (int i = 0; i < 16; i++) y[i] = x[i] ^ data[b * 16 + i];
    if (!crypto::aes_encrypt_block(key, y, x)) return false;
  }
  uint8_t last[16] = {};
  const size_t off = (blocks - 1) * 16, rest = len - off;
  if (whole) {
    for (int i = 0; i < 16; i++) last[i] = data[off + i] ^ k1[i];
  } else {
    if (rest) std::memcpy(last, data + off, rest);
    last[rest] = 0x80;
    for (int i = 0; i < 16; i++) last[i] ^= k2[i];
  }
  for (int i = 0; i < 16; i++) y[i] = x[i] ^ last[i];
  return crypto::aes_encrypt_block(key, y, out);
}

/* ---------------------------------------------------------------- P-256 */

namespace {

struct U {
  uint32_t w[8] = {};
};

U from_be(const uint8_t *b) {
  U r;
  for (int i = 0; i < 8; i++) {
    const uint8_t *p = b + 28 - 4 * i;
    r.w[i] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  return r;
}

void to_be(const U &a, uint8_t *b) {
  for (int i = 0; i < 8; i++) {
    uint8_t *p = b + 28 - 4 * i;
    p[0] = static_cast<uint8_t>(a.w[i] >> 24);
    p[1] = static_cast<uint8_t>(a.w[i] >> 16);
    p[2] = static_cast<uint8_t>(a.w[i] >> 8);
    p[3] = static_cast<uint8_t>(a.w[i]);
  }
}

int cmp(const U &a, const U &b) {
  for (int i = 7; i >= 0; i--)
    if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
  return 0;
}

bool is_zero(const U &a) {
  uint32_t v = 0;
  for (uint32_t x : a.w) v |= x;
  return v == 0;
}

uint32_t add_raw(U &r, const U &a, const U &b) {
  uint64_t c = 0;
  for (int i = 0; i < 8; i++) {
    c += uint64_t(a.w[i]) + b.w[i];
    r.w[i] = static_cast<uint32_t>(c);
    c >>= 32;
  }
  return static_cast<uint32_t>(c);
}

uint32_t sub_raw(U &r, const U &a, const U &b) {
  uint32_t borrow = 0;
  for (int i = 0; i < 8; i++) {
    const uint64_t t = uint64_t(a.w[i]) - b.w[i] - borrow;
    r.w[i] = static_cast<uint32_t>(t);
    borrow = (t >> 32) ? 1 : 0;
  }
  return borrow;
}

/* Arithmetic modulo m (top bit set), values in Montgomery form for mul/pow. */
struct Mod {
  U m, one, r2;
  uint32_t minv = 0;

  explicit Mod(const char *hex) {
    uint8_t b[32];
    for (int i = 0; i < 32; i++) {
      auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
      b[i] = static_cast<uint8_t>(nib(hex[2 * i]) << 4 | nib(hex[2 * i + 1]));
    }
    m = from_be(b);
    uint32_t x = 1;
    for (int i = 0; i < 5; i++) x *= 2 - m.w[0] * x; /* m.w[0]^-1 mod 2^32 */
    minv = 0u - x;
    U zero;
    sub_raw(one, zero, m); /* R mod m = 2^256 - m, as m > 2^255 */
    r2 = one;
    for (int i = 0; i < 256; i++) r2 = add(r2, r2); /* R * 2^256 = R^2 mod m */
  }
  U add(const U &a, const U &b) const {
    U r;
    const uint32_t c = add_raw(r, a, b);
    if (c || cmp(r, m) >= 0) sub_raw(r, r, m);
    return r;
  }
  U sub(const U &a, const U &b) const {
    U r;
    if (sub_raw(r, a, b)) add_raw(r, r, m);
    return r;
  }
  U mul(const U &a, const U &b) const {
    uint32_t t[10] = {};
    for (int i = 0; i < 8; i++) {
      uint64_t c = 0;
      for (int j = 0; j < 8; j++) {
        c += uint64_t(t[j]) + uint64_t(a.w[j]) * b.w[i];
        t[j] = static_cast<uint32_t>(c);
        c >>= 32;
      }
      c += t[8];
      t[8] = static_cast<uint32_t>(c);
      t[9] = static_cast<uint32_t>(c >> 32);
      const uint32_t q = t[0] * minv;
      c = (uint64_t(t[0]) + uint64_t(q) * m.w[0]) >> 32;
      for (int j = 1; j < 8; j++) {
        c += uint64_t(t[j]) + uint64_t(q) * m.w[j];
        t[j - 1] = static_cast<uint32_t>(c);
        c >>= 32;
      }
      c += t[8];
      t[7] = static_cast<uint32_t>(c);
      t[8] = t[9] + static_cast<uint32_t>(c >> 32);
    }
    U r;
    std::memcpy(r.w, t, sizeof(r.w));
    if (t[8] || cmp(r, m) >= 0) sub_raw(r, r, m);
    return r;
  }
  U to(const U &a) const { return mul(a, r2); }
  U from(const U &a) const {
    U unit;
    unit.w[0] = 1;
    return mul(a, unit);
  }
  /* a (Montgomery) to the plain exponent e. */
  U pow(const U &a, const U &e) const {
    U r = one;
    for (int i = 255; i >= 0; i--) {
      r = mul(r, r);
      if ((e.w[i / 32] >> (i % 32)) & 1) r = mul(r, a);
    }
    return r;
  }
  U inv(const U &a) const {
    U e;
    U two;
    two.w[0] = 2;
    sub_raw(e, m, two);
    return pow(a, e);
  }
};

const Mod &F() {
  static const Mod f("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
  return f;
}
const Mod &N() {
  static const Mod n("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
  return n;
}
const uint8_t kB[32] = {0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
                        0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b};
const uint8_t kGx[32] = {0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
                         0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96};
const uint8_t kGy[32] = {0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
                         0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5};

/* y^2 = x^3 - 3x + b, all in Montgomery form. */
U y_squared(const U &x) {
  const Mod &f = F();
  const U x3 = f.mul(f.mul(x, x), x);
  const U three_x = f.add(f.add(x, x), x);
  return f.add(f.sub(x3, three_x), f.to(from_be(kB)));
}

bool is_square(const U &a) {
  const Mod &f = F();
  U e = f.m; /* (p - 1) / 2 */
  U one;
  one.w[0] = 1;
  sub_raw(e, e, one);
  for (int i = 0; i < 8; i++) e.w[i] = e.w[i] >> 1 | (i < 7 ? e.w[i + 1] << 31 : 0);
  return cmp(f.pow(a, e), f.one) == 0;
}

/* p = 3 mod 4: sqrt(a) = a^((p+1)/4); false if a has none. */
bool sqrt_mod(const U &a, U &out) {
  const Mod &f = F();
  U e;
  U one;
  one.w[0] = 1;
  add_raw(e, f.m, one); /* p + 1 does not overflow: p < 2^256 - 1 */
  for (int i = 0; i < 8; i++) e.w[i] = e.w[i] >> 2 | (i < 7 ? e.w[i + 1] << 30 : 0);
  out = f.pow(a, e);
  return cmp(f.mul(out, out), a) == 0;
}

struct J {
  U x, y, z; /* Montgomery form; z = 0 is the point at infinity */
};

J dbl(const J &p) {
  const Mod &f = F();
  if (is_zero(p.z)) return p;
  const U delta = f.mul(p.z, p.z), gamma = f.mul(p.y, p.y), beta = f.mul(p.x, gamma);
  U alpha = f.mul(f.sub(p.x, delta), f.add(p.x, delta));
  alpha = f.add(f.add(alpha, alpha), alpha);
  U beta4 = f.add(beta, beta);
  beta4 = f.add(beta4, beta4);
  const U beta8 = f.add(beta4, beta4);
  J r;
  r.x = f.sub(f.mul(alpha, alpha), beta8);
  const U yz = f.add(p.y, p.z);
  r.z = f.sub(f.sub(f.mul(yz, yz), gamma), delta);
  U g8 = f.mul(gamma, gamma);
  g8 = f.add(g8, g8);
  g8 = f.add(g8, g8);
  g8 = f.add(g8, g8);
  r.y = f.sub(f.mul(alpha, f.sub(beta4, r.x)), g8);
  return r;
}

J add(const J &p, const J &q) {
  const Mod &f = F();
  if (is_zero(p.z)) return q;
  if (is_zero(q.z)) return p;
  const U z1z1 = f.mul(p.z, p.z), z2z2 = f.mul(q.z, q.z);
  const U u1 = f.mul(p.x, z2z2), u2 = f.mul(q.x, z1z1);
  const U s1 = f.mul(f.mul(p.y, q.z), z2z2), s2 = f.mul(f.mul(q.y, p.z), z1z1);
  if (cmp(u1, u2) == 0) {
    if (cmp(s1, s2) == 0) return dbl(p);
    return J{}; /* p = -q */
  }
  const U h = f.sub(u2, u1), r = f.sub(s2, s1);
  const U hh = f.mul(h, h), hhh = f.mul(h, hh), v = f.mul(u1, hh);
  J o;
  o.x = f.sub(f.sub(f.mul(r, r), hhh), f.add(v, v));
  o.y = f.sub(f.mul(r, f.sub(v, o.x)), f.mul(s1, hhh));
  o.z = f.mul(f.mul(p.z, q.z), h);
  return o;
}

J mul(const U &k, const J &p) {
  J r;
  for (int i = 255; i >= 0; i--) {
    r = dbl(r);
    if ((k.w[i / 32] >> (i % 32)) & 1) r = add(r, p);
  }
  return r;
}

/* Affine big-endian x || y in, Jacobian out; false if not on the curve. */
bool load(const uint8_t xy[64], J &out) {
  const Mod &f = F();
  const U x = from_be(xy), y = from_be(xy + 32);
  if (cmp(x, f.m) >= 0 || cmp(y, f.m) >= 0) return false;
  out.x = f.to(x);
  out.y = f.to(y);
  out.z = f.one;
  return cmp(f.mul(out.y, out.y), y_squared(out.x)) == 0;
}

bool store(const J &p, uint8_t xy[64]) {
  const Mod &f = F();
  if (is_zero(p.z)) return false;
  const U zi = f.inv(p.z), zi2 = f.mul(zi, zi), zi3 = f.mul(zi2, zi);
  to_be(f.from(f.mul(p.x, zi2)), xy);
  to_be(f.from(f.mul(p.y, zi3)), xy + 32);
  return true;
}

/* A random scalar in [2, n - 1]. */
bool random_scalar(uint8_t out[32]) {
  for (int tries = 0; tries < 100; tries++) {
    if (!crypto::random_bytes(out, 32)) return false;
    const U v = from_be(out);
    U two;
    two.w[0] = 2;
    if (cmp(v, N().m) < 0 && cmp(v, two) >= 0) return true;
  }
  return false;
}

void negate_y(uint8_t xy[64]) {
  U y = from_be(xy + 32);
  if (is_zero(y)) return;
  sub_raw(y, F().m, y);
  to_be(y, xy + 32);
}

J base() {
  uint8_t g[64];
  std::memcpy(g, kGx, 32);
  std::memcpy(g + 32, kGy, 32);
  J p;
  load(g, p);
  return p;
}

} // namespace

bool p256_mul_base(const uint8_t scalar[32], uint8_t out[64]) { return store(mul(from_be(scalar), base()), out); }

/* ------------------------------------------------------------------ SAE */

bool Sae::prepare(const uint8_t own[6], const uint8_t peer[6], const std::string &password) {
  const Mod &f = F();
  uint8_t addrs[12];
  const bool own_high = std::memcmp(own, peer, 6) > 0;
  std::memcpy(addrs, own_high ? own : peer, 6);
  std::memcpy(addrs + 6, own_high ? peer : own, 6);
  uint8_t prime[32];
  to_be(f.m, prime);
  std::vector<uint8_t> base(password.begin(), password.end());
  base.push_back(0);
  bool found = false;
  bool odd = false;
  U x;
  /* At least 40 rounds whatever the outcome, as hostap does (dragonfly_min_pwe_loop_iter). */
  for (unsigned counter = 1; counter <= 200 && (counter <= 40 || !found); counter++) {
    base.back() = static_cast<uint8_t>(counter);
    uint8_t seed[32], value[32];
    if (!crypto::hmac_sha256(addrs, 12, base.data(), base.size(), seed) ||
        !kdf_sha256(seed, 32, "SAE Hunting and Pecking", prime, 32, value, 256))
      return false;
    const U v = from_be(value);
    if (cmp(v, f.m) >= 0) continue;
    if (!found && is_square(y_squared(f.to(v)))) {
      found = true;
      x = v;
      odd = seed[31] & 1;
    }
  }
  if (!found) return false;
  U y;
  if (!sqrt_mod(y_squared(f.to(x)), y)) return false;
  U yp = f.from(y);
  if (bool(yp.w[0] & 1) != odd) sub_raw(yp, f.m, yp);
  to_be(x, _pwe);
  to_be(yp, _pwe + 32);
  _have_pwe = true;
  return true;
}

bool Sae::commit(const uint8_t *rand, const uint8_t *mask) {
  if (!_have_pwe) return false;
  J pwe;
  if (!load(_pwe, pwe)) return false;
  for (int tries = 0; tries < 10; tries++) {
    uint8_t m[32];
    if (rand) std::memcpy(_rand, rand, 32);
    else if (!random_scalar(_rand)) return false;
    if (mask) std::memcpy(m, mask, 32);
    else if (!random_scalar(m)) return false;
    const U s = N().add(from_be(_rand), from_be(m));
    U one;
    one.w[0] = 1;
    if (cmp(s, one) <= 0) {
      if (rand || mask) return false;
      continue;
    }
    to_be(s, _scalar);
    if (!store(mul(from_be(m), pwe), _element)) return false;
    negate_y(_element);
    std::memset(m, 0, sizeof(m));
    return true;
  }
  return false;
}

std::vector<uint8_t> Sae::commit_body(const std::vector<uint8_t> &token) const {
  std::vector<uint8_t> b = {19, 0};
  b.insert(b.end(), token.begin(), token.end());
  b.insert(b.end(), _scalar, _scalar + 32);
  b.insert(b.end(), _element, _element + 64);
  return b;
}

bool Sae::process_commit(const uint8_t *body, size_t len) {
  if (len < 2 + 32 + 64 || (body[0] | body[1] << 8) != 19) return false;
  const uint8_t *scalar = body + 2, *element = body + 2 + 32;
  const U ps = from_be(scalar);
  U one;
  one.w[0] = 1;
  if (cmp(ps, one) <= 0 || cmp(ps, N().m) >= 0) return false;
  J pe, pwe;
  if (!load(element, pe) || !load(_pwe, pwe)) return false;
  /* A reflection of our own commit is an attack (hostap: "reflection attack"). */
  if (std::memcmp(scalar, _scalar, 32) == 0 && std::memcmp(element, _element, 64) == 0) return false;
  std::memcpy(_peer_scalar, scalar, 32);
  std::memcpy(_peer_element, element, 64);
  uint8_t k[64];
  if (!store(mul(from_be(_rand), add(mul(ps, pwe), pe)), k)) return false;
  uint8_t zero[32] = {}, keyseed[32];
  if (!crypto::hmac_sha256(zero, 32, k, 32, keyseed)) return false;
  uint8_t value[32];
  to_be(N().add(from_be(_scalar), ps), value);
  uint8_t keys[64];
  if (!kdf_sha256(keyseed, 32, "SAE KCK and PMK", value, 32, keys, 512)) return false;
  std::memcpy(_kck, keys, 32);
  std::memcpy(_pmk, keys + 32, 32);
  std::memcpy(_pmkid, value, 16);
  std::memset(keys, 0, sizeof(keys));
  std::memset(k, 0, sizeof(k));
  return true;
}

namespace {

bool confirm_hash(const uint8_t kck[32], uint16_t sc, const uint8_t *s1, const uint8_t *e1, const uint8_t *s2,
                  const uint8_t *e2, uint8_t out[32]) {
  uint8_t buf[2 + 32 + 64 + 32 + 64];
  buf[0] = static_cast<uint8_t>(sc);
  buf[1] = static_cast<uint8_t>(sc >> 8);
  std::memcpy(buf + 2, s1, 32);
  std::memcpy(buf + 34, e1, 64);
  std::memcpy(buf + 98, s2, 32);
  std::memcpy(buf + 130, e2, 64);
  return crypto::hmac_sha256(kck, 32, buf, sizeof(buf), out);
}

} // namespace

bool Sae::confirm(uint16_t sc, uint8_t out[32]) const {
  return confirm_hash(_kck, sc, _scalar, _element, _peer_scalar, _peer_element, out);
}

bool Sae::verify(uint16_t peer_sc, const uint8_t peer_confirm[32]) const {
  uint8_t want[32];
  if (!confirm_hash(_kck, peer_sc, _peer_scalar, _peer_element, _scalar, _element, want)) return false;
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= want[i] ^ peer_confirm[i];
  return diff == 0;
}

/* ------------------------------------------------------------------ OWE */

bool Owe::generate(const uint8_t *priv) {
  if (priv) std::memcpy(_priv, priv, 32);
  else if (!random_scalar(_priv)) return false;
  uint8_t xy[64];
  if (!p256_mul_base(_priv, xy)) return false;
  std::memcpy(_pub, xy, 32);
  return true;
}

bool Owe::derive(const uint8_t peer_x[32]) {
  const Mod &f = F();
  const U x = from_be(peer_x);
  if (cmp(x, f.m) >= 0) return false;
  U y;
  if (!sqrt_mod(y_squared(f.to(x)), y)) return false;
  /* Either root will do: k(-P) and kP share their x. */
  J peer{f.to(x), y, f.one};
  uint8_t z[64];
  if (!store(mul(from_be(_priv), peer), z)) return false;
  uint8_t salt[32 + 32 + 2];
  std::memcpy(salt, _pub, 32);
  std::memcpy(salt + 32, peer_x, 32);
  salt[64] = 19;
  salt[65] = 0;
  uint8_t prk[32];
  if (!crypto::hmac_sha256(salt, sizeof(salt), z, 32, prk)) return false;
  static const char kInfo[] = "OWE Key Generation";
  uint8_t info[sizeof(kInfo)];
  std::memcpy(info, kInfo, sizeof(kInfo) - 1);
  info[sizeof(kInfo) - 1] = 1; /* HKDF-Expand, first block */
  const bool ok = crypto::hmac_sha256(prk, 32, info, sizeof(info), _pmk);
  std::memset(z, 0, sizeof(z));
  std::memset(prk, 0, sizeof(prk));
  return ok;
}

} // namespace wpa3
