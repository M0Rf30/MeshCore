#pragma once

#include <stdint.h>
#include <stddef.h>

// Mock for rweather/Crypto's Ed25519 class -- provides only the static verify()
// entry point src/Identity.cpp calls. Same philosophy as test/mocks/AES.h and
// test/mocks/SHA256.h: a minimal, deliberately-not-cryptographic stand-in so
// Identity.cpp (and anything that links it, like the net-sim's real Mesh/Dispatcher
// classes) compiles for native without pulling in the full Arduino Crypto library.
// Real Ed25519 signing (LocalIdentity::sign(), via the vendored lib/ed25519) still
// runs for real; only verification of ADVERT signatures is stubbed to always pass,
// since the simulator does not model cryptographic attacks.
class Ed25519 {
public:
  static bool verify(const uint8_t* signature, const uint8_t* publicKey, const void* message, size_t len) {
    return true;
  }
};
