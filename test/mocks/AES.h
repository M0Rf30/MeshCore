#pragma once

#include <stdint.h>
#include <stddef.h>

// Mock AES128 class for testing.
// Provides minimal interface to allow Utils.cpp to compile. Deterministic but
// not cryptographic: each block is XORed with the key (repeated/truncated to
// 16 bytes), which is trivially self-inverse so encrypt(decrypt(x)) == x --
// needed for Utils::encrypt()/decrypt() to round-trip correctly under test.
class AES128 {
  uint8_t _key[16] = { 0 };
public:
  void setKey(const uint8_t* key, size_t keySize) {
    for (size_t i = 0; i < 16; i++) _key[i] = (i < keySize) ? key[i] : 0;
  }
  void encryptBlock(uint8_t* output, const uint8_t* input) {
    for (int i = 0; i < 16; i++) output[i] = input[i] ^ _key[i];
  }
  void decryptBlock(uint8_t* output, const uint8_t* input) {
    for (int i = 0; i < 16; i++) output[i] = input[i] ^ _key[i];
  }
};
