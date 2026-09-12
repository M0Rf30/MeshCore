#pragma once

#include <Dispatcher.h>
#include "SimClock.h"
#include "SimMedium.h"

#include <cstring>
namespace netsim {

// mesh::Radio implementation backed by a shared SimMedium instead of hardware.
// Airtime comes from the medium (the real LoRa time-on-air formula); the medium
// also decides reachability, loss, half-duplex, and collisions -- SimRadio is
// just the per-node view of that shared channel.
class SimRadio : public mesh::Radio {
  SimMedium& _medium;
  SimClock& _clock;
  int _idx;
  unsigned long _tx_end_at = 0;
  bool _tx_pending = false;
  float _last_snr = 0, _last_rssi = -100;

public:
  SimRadio(SimMedium& medium, SimClock& clock, int idx) : _medium(medium), _clock(clock), _idx(idx) { }

  int recvRaw(uint8_t* bytes, int sz) override {
    if (_tx_pending) return 0;   // half-duplex: can't receive while transmitting

    SimMedium::Delivered d;
    if (!_medium.popReceived(_idx, d)) return 0;
    int len = d.len < sz ? d.len : sz;
    memcpy(bytes, d.data, len);
    _last_snr = d.snr_db;
    _last_rssi = -60.0f + d.snr_db;   // simplistic but monotonic in SNR; not used by any invariant we test
    return len;
  }

  uint32_t getEstAirtimeFor(int len_bytes) override { return _medium.airtimeForLen(len_bytes); }

  float packetScore(float snr, int packet_len) override {
    float score = (snr + 20.0f) / 30.0f;
    if (score < 0.05f) score = 0.05f;
    if (score > 0.95f) score = 0.95f;
    return score;
  }

  bool startSendRaw(const uint8_t* bytes, int len) override {
    if (_tx_pending) return false;
    _medium.beginTransmit(_idx, bytes, len);
    _tx_end_at = _clock.getMillis() + _medium.airtimeForLen(len);
    _tx_pending = true;
    return true;
  }

  bool isSendComplete() override {
    return _tx_pending && _clock.getMillis() >= _tx_end_at;
  }

  void onSendFinished() override { _tx_pending = false; }

  bool isInRecvMode() const override { return !_tx_pending; }

  bool isReceiving() override {
    return !_tx_pending && _medium.isChannelBusyFor(_idx, _clock.getMillis());
  }

  float getLastRSSI() const override { return _last_rssi; }
  float getLastSNR() const override { return _last_snr; }
};

}
