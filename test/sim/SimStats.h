#pragma once

#include <Dispatcher.h>
#include <helpers/SimpleMeshTables.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace netsim {

// Per-run statistics collector. Message-level counters (originated/delivered/
// hop-limit-dropped) are recorded directly by SimMeshNode as events happen;
// network-level totals (retransmissions, duplicate suppressions, airtime) are
// NOT reinvented here -- they are read straight from the real Dispatcher and
// SimpleMeshTables counters via aggregate(), since those are exactly the
// counters the firmware itself already maintains.
class SimStats {
public:
  void recordOriginated() { _originated++; }
  void recordHopLimitDropped() { _hop_limit_dropped++; }
  void recordDelivered(unsigned long latency_ms) {
    _delivered++;
    _latencies.push_back(latency_ms);
  }

  // Reads real per-node counters (Dispatcher::getNumSent*, SimpleMeshTables::getNum*Dups,
  // Dispatcher::getTotalAirTime) and folds them into the run totals.
  void aggregate(mesh::Dispatcher& dispatcher, SimpleMeshTables& tables) {
    _transmissions += dispatcher.getNumSentFlood() + dispatcher.getNumSentDirect();
    _duplicates += tables.getNumFloodDups() + tables.getNumDirectDups();
    _total_airtime_ms += dispatcher.getTotalAirTime();
  }

  uint32_t originated() const { return _originated; }
  uint32_t delivered() const { return _delivered; }
  uint32_t hopLimitDropped() const { return _hop_limit_dropped; }
  uint32_t transmissions() const { return _transmissions; }
  uint32_t duplicates() const { return _duplicates; }
  unsigned long totalAirtimeMs() const { return _total_airtime_ms; }

  unsigned long minLatencyMs() const { return _latencies.empty() ? 0 : *std::min_element(_latencies.begin(), _latencies.end()); }
  unsigned long maxLatencyMs() const { return _latencies.empty() ? 0 : *std::max_element(_latencies.begin(), _latencies.end()); }
  unsigned long medianLatencyMs() const {
    if (_latencies.empty()) return 0;
    std::vector<unsigned long> sorted(_latencies);
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
  }

  bool operator==(const SimStats& other) const {
    return _originated == other._originated && _delivered == other._delivered
        && _hop_limit_dropped == other._hop_limit_dropped && _transmissions == other._transmissions
        && _duplicates == other._duplicates && _total_airtime_ms == other._total_airtime_ms
        && _latencies == other._latencies;
  }

private:
  uint32_t _originated = 0, _delivered = 0, _hop_limit_dropped = 0;
  uint32_t _transmissions = 0, _duplicates = 0;
  unsigned long _total_airtime_ms = 0;
  std::vector<unsigned long> _latencies;
};

}
