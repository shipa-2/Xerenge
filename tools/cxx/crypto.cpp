#include "crypto.hpp"

#include <cstring>

namespace {

// --- SHA-256 ---------------------------------------------------------------

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

constexpr uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// --- AES-128 ---------------------------------------------------------------

constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab,
    0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
    0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71,
    0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6,
    0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
    0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45,
    0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44,
    0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
    0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49,
    0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25,
    0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
    0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1,
    0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb,
    0x16};

struct InverseSbox {
  uint8_t table[256];
  constexpr InverseSbox() : table() {
    for (int i = 0; i < 256; ++i) {
      table[kSbox[i]] = uint8_t(i);
    }
  }
};
constexpr InverseSbox kInvSbox;

constexpr uint8_t Xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

uint8_t Mul(uint8_t x, uint8_t y) {
  uint8_t r = 0;
  while (y) {
    if (y & 1) {
      r ^= x;
    }
    x = Xtime(x);
    y >>= 1;
  }
  return r;
}

// The 11 round keys, 16 bytes each.
void ExpandKey(const uint8_t key[16], uint8_t round_keys[176]) {
  std::memcpy(round_keys, key, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4];
    std::memcpy(t, round_keys + i - 4, 4);
    if (i % 16 == 0) {
      const uint8_t first = t[0];
      t[0] = uint8_t(kSbox[t[1]] ^ rcon);
      t[1] = kSbox[t[2]];
      t[2] = kSbox[t[3]];
      t[3] = kSbox[first];
      rcon = Xtime(rcon);
    }
    for (int j = 0; j < 4; ++j) {
      round_keys[i + j] = uint8_t(round_keys[i - 16 + j] ^ t[j]);
    }
  }
}

// One 16-byte block, the state in column order as FIPS 197 lays it out.
void DecryptBlock(const uint8_t round_keys[176], uint8_t s[16]) {
  const auto add_round_key = [&](int round) {
    for (int i = 0; i < 16; ++i) {
      s[i] ^= round_keys[round * 16 + i];
    }
  };
  const auto inv_shift_sub = [&] {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        // Row r moves r columns right.
        t[((c + r) % 4) * 4 + r] = kInvSbox.table[s[c * 4 + r]];
      }
    }
    std::memcpy(s, t, 16);
  };
  add_round_key(10);
  for (int round = 9; round >= 1; --round) {
    inv_shift_sub();
    add_round_key(round);
    for (int c = 0; c < 4; ++c) {
      uint8_t* col = s + c * 4;
      const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
      col[0] = Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9);
      col[1] = Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13);
      col[2] = Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11);
      col[3] = Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14);
    }
  }
  inv_shift_sub();
  add_round_key(0);
}

}  // namespace

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
             0x5be0cd19} {}

void Sha256::Block(const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 |
           uint32_t(block[i * 4 + 2]) << 8 | uint32_t(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
    const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  length_ += size;
  if (buffered_) {
    const size_t take = size < 64 - buffered_ ? size : 64 - buffered_;
    std::memcpy(buffer_ + buffered_, bytes, take);
    buffered_ += take;
    bytes += take;
    size -= take;
    if (buffered_ < 64) {
      return;
    }
    Block(buffer_);
    buffered_ = 0;
  }
  for (; size >= 64; bytes += 64, size -= 64) {
    Block(bytes);
  }
  std::memcpy(buffer_, bytes, size);
  buffered_ = size;
}

void Sha256::Final(uint8_t digest[32]) {
  const uint64_t bits = length_ * 8;
  const uint8_t pad = 0x80;
  Update(&pad, 1);
  const uint8_t zero = 0;
  while (buffered_ != 56) {
    Update(&zero, 1);
  }
  uint8_t tail[8];
  for (int i = 0; i < 8; ++i) {
    tail[i] = uint8_t(bits >> (56 - i * 8));
  }
  Update(tail, 8);
  for (int i = 0; i < 8; ++i) {
    digest[i * 4] = uint8_t(state_[i] >> 24);
    digest[i * 4 + 1] = uint8_t(state_[i] >> 16);
    digest[i * 4 + 2] = uint8_t(state_[i] >> 8);
    digest[i * 4 + 3] = uint8_t(state_[i]);
  }
}

void Aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in,
                      uint8_t* out, size_t size) {
  uint8_t round_keys[176];
  ExpandKey(key, round_keys);
  uint8_t previous[16];
  std::memcpy(previous, iv, 16);
  for (size_t offset = 0; offset + 16 <= size; offset += 16) {
    uint8_t cipher[16];
    std::memcpy(cipher, in + offset, 16);
    uint8_t block[16];
    std::memcpy(block, cipher, 16);
    DecryptBlock(round_keys, block);
    for (int i = 0; i < 16; ++i) {
      out[offset + i] = uint8_t(block[i] ^ previous[i]);
    }
    std::memcpy(previous, cipher, 16);
  }
  // A short tail, as OpenSSL's AES_cbc_encrypt treats one: a whole block is
  // decrypted and only its first bytes are kept.
  if (const size_t tail = size % 16; tail != 0) {
    const size_t offset = size - tail;
    uint8_t block[16] = {};
    std::memcpy(block, in + offset, tail);
    DecryptBlock(round_keys, block);
    for (size_t i = 0; i < tail; ++i) {
      out[offset + i] = uint8_t(block[i] ^ previous[i]);
    }
  }
}
