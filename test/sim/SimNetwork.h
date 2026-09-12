#pragma once

#include "SimMedium.h"
#include "SimMeshNode.h"
#include "SimRadio.h"
#include "SimRNG.h"
#include "SimStats.h"
#include <helpers/StaticPoolPacketManager.h>
#include <memory>
#include <vector>

namespace netsim {

// Owns everything for one simulation run: the shared clock, the N nodes (each a
// real mesh::Mesh instance, backed by a real StaticPoolPacketManager and
// SimpleMeshTables), and the shared SimMedium. This is the entry point used by
// both the gtest suites and the standalone CLI binary.
class SimNetwork {
public:
  SimNetwork(int num_nodes, uint32_t seed, LoRaParams params = LoRaParams(), int pool_size = 24)
    : _medium(num_nodes, _clock, params, seed), _num_nodes(num_nodes)
  {
    _nodes.resize(num_nodes);
    for (int i = 0; i < num_nodes; i++) {
      _radios.push_back(std::make_unique<SimRadio>(_medium, _clock, i));
      _rngs.push_back(std::make_unique<SimRNG>(seed * 2654435761u + (uint32_t)i * 0x9E3779B1u + 1));
      _tables.push_back(std::make_unique<SimpleMeshTables>());
      _pkt_mgrs.push_back(std::make_unique<StaticPoolPacketManager>(pool_size));
    }
  }

  SimMedium& medium() { return _medium; }
  SimClock& clock() { return _clock; }
  SimStats& stats() { return _stats; }
  int size() const { return _num_nodes; }

  SimMeshNode& addNode(int idx, bool is_repeater, uint8_t hop_limit = 32) {
    auto node = std::make_unique<SimMeshNode>(*_radios[idx], _clock, *_rngs[idx], _clock,
                                               *_pkt_mgrs[idx], *_tables[idx], _stats, is_repeater, hop_limit);
    node->begin();
    _medium.attachNode(idx, node.get());
    _nodes[idx] = std::move(node);
    return *_nodes[idx];
  }

  SimMeshNode& node(int idx) { return *_nodes[idx]; }

  void run(unsigned long duration_ms, unsigned long tick_ms = 2) {
    _medium.run(duration_ms, tick_ms);
  }

  // Folds each node's real Dispatcher/SimpleMeshTables counters into stats().
  // Call after run() (may be called more than once; totals are re-summed each time).
  void collectNodeTotals() {
    for (int i = 0; i < _num_nodes; i++) {
      if (_nodes[i]) _stats.aggregate(*_nodes[i], *_tables[i]);
    }
  }

private:
  SimClock _clock;
  SimMedium _medium;
  SimStats _stats;
  int _num_nodes;
  std::vector<std::unique_ptr<SimRadio>> _radios;
  std::vector<std::unique_ptr<SimRNG>> _rngs;
  std::vector<std::unique_ptr<SimpleMeshTables>> _tables;
  std::vector<std::unique_ptr<StaticPoolPacketManager>> _pkt_mgrs;
  std::vector<std::unique_ptr<SimMeshNode>> _nodes;
};

}
