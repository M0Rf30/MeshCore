// Standalone CLI entry point for the net-sim engine (see docs/simulator.md).
// Builds a simple linear-chain scenario and prints run statistics -- the same
// SimNetwork/SimMedium/SimMeshNode engine the gtest suite in test/test_net_sim/
// exercises, just driven from main() instead of from TEST() cases.
//
// usage: native_sim_cli [seed] [num_nodes] [duration_ms]

#include "SimNetwork.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
  uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], nullptr, 10) : 42;
  int num_nodes = argc > 2 ? atoi(argv[2]) : 5;
  unsigned long duration_ms = argc > 3 ? strtoul(argv[3], nullptr, 10) : 20000;

  if (num_nodes < 2) {
    fprintf(stderr, "num_nodes must be >= 2\n");
    return 1;
  }

  netsim::SimNetwork net(num_nodes, seed);
  net.addNode(0, /*is_repeater=*/false);
  for (int i = 1; i + 1 < num_nodes; i++) net.addNode(i, /*is_repeater=*/true);
  net.addNode(num_nodes - 1, /*is_repeater=*/false);
  net.medium().connectChain();

  net.node(0).originate(1);
  net.run(duration_ms);
  net.collectNodeTotals();

  const auto& s = net.stats();
  printf("seed=%u nodes=%d duration_ms=%lu\n", seed, num_nodes, duration_ms);
  printf("originated=%u delivered=%u hop_limit_dropped=%u\n", s.originated(), s.delivered(), s.hopLimitDropped());
  printf("transmissions=%u duplicates=%u total_airtime_ms=%lu\n", s.transmissions(), s.duplicates(), s.totalAirtimeMs());
  printf("latency_ms: min=%lu median=%lu max=%lu\n", s.minLatencyMs(), s.medianLatencyMs(), s.maxLatencyMs());
  printf("medium: transmissions=%u collided=%u lost=%u\n",
         net.medium().transmissionCount(), net.medium().collidedCount(), net.medium().lostCount());
  return s.delivered() > 0 ? 0 : 2;
}
