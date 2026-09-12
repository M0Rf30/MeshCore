#pragma once

#include <Dispatcher.h>
#include <MeshCore.h>

namespace netsim {

// Deterministic millisecond/RTC clock for the simulator. Time only ever advances
// when SimNetwork explicitly moves it forward while draining the event queue --
// no wall-clock reads, no sleeps. Every node in a run shares ONE SimClock instance
// (a real deployment would have per-node clock drift; this simulator deliberately
// does not model that -- see docs/simulator.md).
class SimClock : public mesh::MillisecondClock, public mesh::RTCClock {
  unsigned long _millis = 0;
  uint32_t _epoch_base;

public:
  explicit SimClock(uint32_t epoch_base = 1700000000) : _epoch_base(epoch_base) { }

  unsigned long getMillis() override { return _millis; }

  uint32_t getCurrentTime() override { return _epoch_base + _millis / 1000; }
  void setCurrentTime(uint32_t time) override { _epoch_base = time - _millis / 1000; }

  // advances the shared clock; only called by SimNetwork's event loop.
  void advanceTo(unsigned long millis) {
    if (millis > _millis) _millis = millis;
  }
};

}
