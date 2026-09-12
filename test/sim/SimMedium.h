#pragma once

#include <Dispatcher.h>
#include "LoRaAirtime.h"
#include "SimClock.h"
#include "SimRNG.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <queue>
#include <vector>
namespace netsim {

class SimRadio;

// Shared virtual RF medium for one simulation run. Owns:
//  - the per-node/per-link topology (who can hear whom, with what loss probability)
//  - the discrete-event queue (ordered by virtual time) that drives the whole run
//  - collision + half-duplex resolution for in-flight transmissions
//
// Time only moves forward as events are popped off the queue; SimMedium never
// reads the wall clock. Every random decision (link-loss roll) is drawn from a
// medium-owned SimRNG seeded from the run seed, so two runs built with the same
// seed produce byte-identical transmissions/collisions/drops.
class SimMedium {
public:
  struct Link {
    bool reachable = false;
    double loss_probability = 0.0;   // probability a reachable transmission is NOT decoded
    float snr_db = 8.0f;
  };

  SimMedium(int num_nodes, SimClock& clock, LoRaParams params, uint32_t seed)
    : _num_nodes(num_nodes), _clock(clock), _params(params), _rng(seed ^ 0xA5A5A5A5u),
      _links(num_nodes, std::vector<Link>(num_nodes)), _inbox(num_nodes),
      _nodes(num_nodes, nullptr)
  { }

  void attachNode(int idx, mesh::Dispatcher* node) { _nodes[idx] = node; }

  uint32_t airtimeForLen(int len_bytes) const { return loRaTimeOnAirMillis(len_bytes, _params); }

  // links are directional (A hears B independently of B hears A); call twice for symmetric links.
  void setLink(int from, int to, bool reachable, double loss_probability = 0.0, float snr_db = 8.0f) {
    _links[from][to] = { reachable, loss_probability, snr_db };
  }

  void connectChain(double loss_probability = 0.0, float snr_db = 8.0f) {
    for (int i = 0; i + 1 < _num_nodes; i++) {
      setLink(i, i + 1, true, loss_probability, snr_db);
      setLink(i + 1, i, true, loss_probability, snr_db);
    }
  }

  void connectAll(double loss_probability = 0.0, float snr_db = 8.0f) {
    for (int i = 0; i < _num_nodes; i++) {
      for (int j = 0; j < _num_nodes; j++) {
        if (i == j) continue;
        setLink(i, j, true, loss_probability, snr_db);
      }
    }
  }

  // ---- called by SimRadio ----

  int beginTransmit(int node_idx, const uint8_t* data, int len) {
    unsigned long start = _clock.getMillis();
    unsigned long end = start + airtimeForLen(len);

    Transmission tx;
    tx.start = start; tx.end = end; tx.node_idx = node_idx; tx.len = len;
    memcpy(tx.data, data, len);
    for (int j = 0; j < _num_nodes; j++) {
      if (j != node_idx && _links[node_idx][j].reachable) tx.reached.push_back(j);
    }

    int tx_id = (int)_transmissions.size();
    _transmissions.push_back(std::move(tx));
    _tx_count++;
    pushEvent(end, EventType::TX_END, tx_id);
    return tx_id;
  }

  bool isChannelBusyFor(int node_idx, unsigned long now) const {
    for (const auto& tx : _transmissions) {
      if (tx.node_idx == node_idx) continue;
      if (!_links[tx.node_idx][node_idx].reachable) continue;
      if (tx.start <= now && now < tx.end) return true;
    }
    return false;
  }

  struct Delivered { uint8_t data[256]; int len; float snr_db; };

  bool popReceived(int node_idx, Delivered& out) {
    auto& q = _inbox[node_idx];
    if (q.empty()) return false;
    out = q.front();
    q.pop_front();
    return true;
  }

  // ---- run loop ----

  // Drains the event queue up to 'duration_ms' of virtual time, calling loop() on
  // every attached node every 'tick_ms', in node-index order (deterministic).
  void run(unsigned long duration_ms, unsigned long tick_ms) {
    unsigned long end = _clock.getMillis() + duration_ms;
    pushEvent(_clock.getMillis(), EventType::TICK, 0);

    while (!_queue.empty() && _queue.top().time <= end) {
      Event ev = _queue.top();
      _queue.pop();
      _clock.advanceTo(ev.time);

      if (ev.type == EventType::TICK) {
        for (auto* node : _nodes) {
          if (node) node->loop();
        }
        unsigned long next = ev.time + tick_ms;
        if (next <= end) pushEvent(next, EventType::TICK, 0);
      } else {
        processTxEnd(ev.ref);
      }
    }
    _clock.advanceTo(end);
  }

  uint32_t collidedCount() const { return _collided; }
  uint32_t lostCount() const { return _lost; }
  uint32_t deliveredFrameCount() const { return _delivered_frames; }
  uint32_t transmissionCount() const { return _tx_count; }

private:
  enum class EventType { TICK, TX_END };
  struct Event {
    unsigned long time;
    uint64_t seq;
    EventType type;
    int ref;
  };
  struct EventOrder {
    bool operator()(const Event& a, const Event& b) const {
      if (a.time != b.time) return a.time > b.time;
      return a.seq > b.seq;
    }
  };

  struct Transmission {
    unsigned long start, end;
    int node_idx, len;
    uint8_t data[256];
    std::vector<int> reached;
  };

  static bool overlaps(const Transmission& a, const Transmission& b) {
    return a.start < b.end && b.start < a.end;
  }

  void pushEvent(unsigned long time, EventType type, int ref) {
    _queue.push(Event{ time, _seq++, type, ref });
  }

  void processTxEnd(int tx_id) {
    const Transmission& a = _transmissions[tx_id];
    for (int r : a.reached) {
      bool half_duplex_blocked = false;
      for (const auto& b : _transmissions) {
        if (b.node_idx == r && overlaps(a, b)) { half_duplex_blocked = true; break; }
      }
      if (half_duplex_blocked) { _lost++; continue; }

      bool collided = false;
      for (size_t bi = 0; bi < _transmissions.size(); bi++) {
        if ((int)bi == tx_id) continue;
        const auto& b = _transmissions[bi];
        if (!overlaps(a, b)) continue;
        if (std::find(b.reached.begin(), b.reached.end(), r) != b.reached.end()) { collided = true; break; }
      }
      if (collided) { _collided++; continue; }

      double loss_p = _links[a.node_idx][r].loss_probability;
      if (loss_p > 0.0 && rollUnit() < loss_p) { _lost++; continue; }

      Delivered d;
      d.len = a.len;
      memcpy(d.data, a.data, a.len);
      d.snr_db = _links[a.node_idx][r].snr_db;
      _inbox[r].push_back(d);
      _delivered_frames++;
    }
  }

  double rollUnit() { return (double)(_rng.nextU32() >> 8) / (double)(1u << 24); }

  int _num_nodes;
  SimClock& _clock;
  LoRaParams _params;
  SimRNG _rng;
  std::vector<std::vector<Link>> _links;
  std::vector<Transmission> _transmissions;
  std::vector<std::deque<Delivered>> _inbox;
  std::vector<mesh::Dispatcher*> _nodes;

  std::priority_queue<Event, std::vector<Event>, EventOrder> _queue;
  uint64_t _seq = 0;
  uint32_t _tx_count = 0, _collided = 0, _lost = 0, _delivered_frames = 0;
};

}
