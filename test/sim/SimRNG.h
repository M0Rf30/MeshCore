#pragma once

#include <Utils.h>
#include <cstdint>

namespace netsim {

// Deterministic seeded RNG (xorshift128) implementing mesh::RNG. Every node gets
// its own instance seeded from the run seed + node index, so a run is bit-for-bit
// reproducible given the same seed, independent of run-to-run heap/thread state.
class SimRNG : public mesh::RNG {
  uint32_t _s[4];

public:
  explicit SimRNG(uint32_t seed) {
    // splitmix32-style spread so small/adjacent seeds don't produce correlated streams.
    uint32_t x = seed ? seed : 0x9E3779B9u;
    for (int i = 0; i < 4; i++) {
      x += 0x9E3779B9u;
      uint32_t z = x;
      z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
      z = (z ^ (z >> 13)) * 0xC2B2AE35u;
      _s[i] = z ^ (z >> 16);
      if (_s[i] == 0) _s[i] = 1;
    }
  }

  uint32_t nextU32() {
    uint32_t t = _s[3];
    uint32_t s = _s[0];
    _s[3] = _s[2]; _s[2] = _s[1]; _s[1] = s;
    t ^= t << 11;
    t ^= t >> 8;
    _s[0] = t ^ s ^ (s >> 19);
    return _s[0];
  }

  void random(uint8_t* dest, size_t sz) override {
    size_t i = 0;
    while (i < sz) {
      uint32_t v = nextU32();
      for (int b = 0; b < 4 && i < sz; b++, i++) {
        dest[i] = (uint8_t)(v >> (b * 8));
      }
    }
  }
};

}
