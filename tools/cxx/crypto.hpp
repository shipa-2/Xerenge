#pragma once

// The two primitives the tools need - SHA-256 for checking the disc image and
// AES-128-CBC for opening default.xex - written out here so the tools depend
// on no crypto library (FIPS 180-4 and FIPS 197).

#include <cstddef>
#include <cstdint>

class Sha256 {
 public:
  Sha256();
  void Update(const void* data, size_t size);
  void Final(uint8_t digest[32]);

 private:
  void Block(const uint8_t block[64]);

  uint32_t state_[8];
  uint64_t length_ = 0;  // bytes hashed
  uint8_t buffer_[64];
  size_t buffered_ = 0;
};

// Decrypts `size` bytes of AES-128-CBC; `in` and `out` may be the same buffer.
void Aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in,
                      uint8_t* out, size_t size);
