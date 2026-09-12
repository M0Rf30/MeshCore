#pragma once

#include <Mesh.h>
#include <helpers/RoutingPolicy.h>
#include "SimStats.h"

#include <cstring>

namespace netsim {

// The one concrete mesh::Mesh subclass used by the simulator. Exercises the
// REAL flood-routing/dedup/retransmit-delay code in src/Mesh.cpp and
// src/Dispatcher.cpp; role (repeater vs. originating/terminating client) is
// just a constructor flag, since that's the only behavioural difference
// needed (allowPacketForward() + the hop-limit filter).
class SimMeshNode : public mesh::Mesh {
  SimStats& _stats;
  bool _is_repeater;
  uint8_t _hop_limit;

public:
  SimMeshNode(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
              mesh::PacketManager& mgr, mesh::MeshTables& tables, SimStats& stats,
              bool is_repeater, uint8_t hop_limit = 32)
    : mesh::Mesh(radio, ms, rng, rtc, mgr, tables), _stats(stats), _is_repeater(is_repeater), _hop_limit(hop_limit)
  { }

  // Sends a flood TXT_MSG carrying [msg_id:u32][origin send time ms:u32], and
  // records it as 'originated' for the run stats.
  //
  // TXT_MSG (not RAW_CUSTOM) is used deliberately: PAYLOAD_TYPE_RAW_CUSTOM is
  // only ever processed for DIRECT routes in the real Mesh::onRecvPacket() (see
  // src/Mesh.cpp, PAYLOAD_TYPE_RAW_CUSTOM case) and is never flood-forwarded.
  // TXT_MSG is real flood-routable traffic. The encryption round-trip is real
  // (Utils::encryptThenMAC/MACThenDecrypt); only the underlying AES/SHA256
  // primitives are the project's existing native-test mocks (test/mocks/AES.h,
  // SHA256.h), so a fixed all-zero shared secret round-trips deterministically
  // without needing real key exchange for this simulator.
  void originate(uint32_t msg_id) {
    uint8_t payload[8];
    memcpy(payload, &msg_id, 4);
    uint32_t t = (uint32_t)_ms->getMillis();
    memcpy(payload + 4, &t, 4);

    static const uint8_t secret[PUB_KEY_SIZE] = { 0 };
    mesh::Identity dest;   // zero pub_key: hash-match is gated below via searchPeersByHash()
    mesh::Packet* pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, dest, secret, payload, sizeof(payload));
    if (!pkt) return;
    sendFlood(pkt);
    _stats.recordOriginated();
  }

protected:
  bool allowPacketForward(const mesh::Packet* packet) override { return _is_repeater; }

  bool filterRecvFloodPacket(mesh::Packet* packet) override {
    if (mesh::isFloodHopLimitExceeded(packet, _hop_limit, _hop_limit, _hop_limit)) {
      _stats.recordHopLimitDropped();
      return true;
    }
    return false;
  }

  // Only client (non-repeater) nodes claim a 'peer' match, so only they decrypt
  // and terminate messages; repeaters must never match here or they'd stop
  // forwarding (Mesh::onRecvPacket marks the packet do-not-retransmit on match).
  int searchPeersByHash(const uint8_t* hash) override { return _is_repeater ? 0 : 1; }

  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override {
    memset(dest_secret, 0, PUB_KEY_SIZE);
  }

  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override {
    if (type != PAYLOAD_TYPE_TXT_MSG || len < 8) return;
    uint32_t origin_time;
    memcpy(&origin_time, data + 4, 4);
    _stats.recordDelivered(_ms->getMillis() - origin_time);
  }
};

}
