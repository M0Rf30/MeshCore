#include <gtest/gtest.h>

#include "SimNetwork.h"

using namespace netsim;

// ── 5-node linear chain: originator -> repeater x3 -> terminator ──────────────

TEST(NetSim, LinearChainDeliversWithinHopLimit) {
  SimNetwork net(5, /*seed=*/12345);
  net.addNode(0, /*is_repeater=*/false);
  net.addNode(1, /*is_repeater=*/true);
  net.addNode(2, /*is_repeater=*/true);
  net.addNode(3, /*is_repeater=*/true);
  net.addNode(4, /*is_repeater=*/false);
  net.medium().connectChain();

  net.node(0).originate(1);
  net.run(20000);

  EXPECT_EQ(1u, net.stats().originated());
  ASSERT_EQ(1u, net.stats().delivered());
  EXPECT_LT(net.stats().maxLatencyMs(), 20000u);
}

// ── hop-limit: a packet that would need more hops than allowed is dropped ────

TEST(NetSim, ExceedingHopLimitIsDroppedNotCirculated) {
  SimNetwork net(5, /*seed=*/777);
  net.addNode(0, false, /*hop_limit=*/2);
  net.addNode(1, true, /*hop_limit=*/2);
  net.addNode(2, true, /*hop_limit=*/2);
  net.addNode(3, true, /*hop_limit=*/2);
  net.addNode(4, false, /*hop_limit=*/2);
  net.medium().connectChain();

  net.node(0).originate(42);
  net.run(20000);

  // node 4 is 4 hops from node 0; a limit of 2 must never let it arrive, and
  // the packet must not keep circulating (bounded retransmissions, not zero,
  // not runaway).
  EXPECT_EQ(0u, net.stats().delivered());
  EXPECT_GT(net.stats().hopLimitDropped(), 0u);

  net.collectNodeTotals();
  EXPECT_LT(net.stats().transmissions(), 20u);
}

// ── fully-connected 10-node cluster: dedup bounds flood amplification ────────

TEST(NetSim, DuplicateSuppressionBoundsFloodInFullyConnectedCluster) {
  const int N = 10;
  SimNetwork net(N, /*seed=*/99);
  for (int i = 0; i < N; i++) {
    net.addNode(i, /*is_repeater=*/true);
  }
  net.medium().connectAll();

  net.node(0).originate(7);
  net.run(30000);

  net.collectNodeTotals();
  // Without dedup, N broadcast-capable repeaters would keep re-flooding each
  // other's rebroadcasts forever. With dedup, each node retransmits the
  // message at most once, so total transmissions must stay within a small
  // constant multiple of N (not an exact count -- retransmit-delay jitter and
  // scheduling can make exact numbers flaky).
  EXPECT_LE(net.stats().transmissions(), (uint32_t)(N * 2));
  EXPECT_GT(net.stats().duplicates(), 0u);
}

// ── determinism: identical seed -> identical stats, rerun from scratch ───────

static SimStats runChainScenario(uint32_t seed) {
  SimNetwork net(5, seed);
  net.addNode(0, false);
  net.addNode(1, true);
  net.addNode(2, true);
  net.addNode(3, true);
  net.addNode(4, false);
  net.medium().connectChain(/*loss_probability=*/0.1);

  for (uint32_t i = 0; i < 5; i++) {
    net.node(0).originate(i);
    net.run(3000);
  }
  net.collectNodeTotals();
  return net.stats();
}

TEST(NetSim, IdenticalSeedProducesIdenticalStatsAcrossReruns) {
  SimStats a = runChainScenario(2024);
  SimStats b = runChainScenario(2024);

  EXPECT_EQ(a, b);
  EXPECT_EQ(a.delivered(), b.delivered());
  EXPECT_EQ(a.transmissions(), b.transmissions());
  EXPECT_EQ(a.totalAirtimeMs(), b.totalAirtimeMs());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
