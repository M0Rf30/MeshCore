# Network Simulator (net-sim)

A host-native, deterministic multi-node mesh simulator that runs the **real**
`mesh::Mesh` / `mesh::Dispatcher` firmware classes against a virtual RF medium
instead of hardware. It lives under `test/sim/` and `test/test_net_sim/`, and
builds as both a PlatformIO `googletest` native test env and a standalone CLI
binary.

## What it models

- **Topology** -- a per-link, directional reachability + loss-probability
  matrix (`SimMedium::setLink`, plus `connectChain()`/`connectAll()` helpers).
- **Airtime** -- the standard Semtech LoRa time-on-air formula (AN1200.13),
  the same formula `RadioLibWrapper::getEstAirtimeFor()` delegates to via
  RadioLib's `PhysicalLayer::getTimeOnAir()` on real hardware. See
  `test/sim/LoRaAirtime.h`. Defaults match `platformio.ini`'s `LORA_SF=8`,
  `LORA_BW=62.5`.
- **Half-duplex** -- a node cannot receive while one of its own transmissions
  is in flight.
- **Collisions** -- two transmissions that are both time-overlapping and both
  reach the same receiver corrupt each other; neither is delivered.
- **Per-link loss** -- an independent probability roll (separate from
  collisions) that a reachable transmission is not decoded.
- **CAD / channel-busy sensing** -- `Radio::isReceiving()` reflects whether
  *any* reachable transmitter is currently on-air, independent of whether that
  transmission would ultimately be decoded.
- **Real routing** -- every node is a real `mesh::Mesh` instance
  (`test/sim/SimMeshNode.h`, `#include <Mesh.h>`, `#include <Dispatcher.h>`),
  backed by a real `StaticPoolPacketManager` and `SimpleMeshTables`. Flood
  dedup, retransmit-delay jitter, RX score delay, CAD retry, and the
  duty-cycle budget are the genuine firmware code paths, not a
  reimplementation.

## What it deliberately does NOT model

- **No RF physical layer / demodulation.** Reachability and loss are
  configured probabilities, not a propagation/fading model.
- **No capture effect.** Two overlapping transmissions heard by the same
  receiver always both fail; a real receiver can sometimes capture the
  stronger signal.
- **No propagation delay.** Signals arrive instantly (a few km at radio speed
  is microseconds relative to LoRa airtimes of tens to thousands of ms).
- **No per-node clock drift.** All nodes share one `SimClock`; MeshCore's
  courier-clock skew handling is out of scope.
- **No real node addressing/identity.** `SimMeshNode` uses a fixed all-zero
  `LocalIdentity` and a fixed all-zero shared secret for its test message
  flow, so every node's destination-hash check trivially matches. Real
  Ed25519 signing (`LocalIdentity::sign()`, via the vendored `lib/ed25519`)
  still runs for real; ADVERT signature verification is stubbed to always
  pass (`test/mocks/Ed25519.h`), since the simulator is not a security test.
- **No hardware duty-cycle regulatory limits** beyond whatever
  `Dispatcher`'s own budget defaults already enforce.

## Running it

```sh
# gtest suite
pio test -e native_sim -vv

# standalone binary: seed, node count, duration_ms (all optional)
pio run -e native_sim_cli
.pio/build/native_sim_cli/program 42 5 20000
```

The CLI builds a linear chain (first/last node are clients, the rest are
repeaters), originates one message from node 0, runs the simulation, and
prints the same statistics the gtest suite asserts on: originated/delivered/
hop-limit-dropped counts, real transmission/duplicate counts (read straight
from `Dispatcher::getNumSent*()` and `SimpleMeshTables::getNum*Dups()`),
total airtime, and min/median/max delivery latency.

## Determinism

A run is bit-for-bit reproducible from a seed: `SimClock` only advances when
`SimMedium::run()` pops the next event off its virtual-time-ordered queue (no
wall-clock reads, no sleeps), and every random decision (per-node RNG, and the
medium's own link-loss rolls) is drawn from a `SimRNG` (xorshift128) seeded
deterministically from the run seed. `test/test_net_sim/test_net_sim.cpp`'s
`IdenticalSeedProducesIdenticalStatsAcrossReruns` test asserts this directly
by running the same scenario twice and comparing full stats snapshots.

## Adding a scenario

1. Construct a `netsim::SimNetwork(num_nodes, seed)`.
2. Call `addNode(idx, is_repeater, hop_limit)` for each node -- the first
   argument selects repeater (forwards flood traffic) vs. client (can
   originate/terminate messages) behaviour; both are the same
   `SimMeshNode` class (see `test/sim/SimMeshNode.h`), since forwarding is
   the only behavioural difference the simulator needs.
3. Wire the topology: `net.medium().setLink(a, b, reachable, loss_probability,
   snr_db)` per direction, or use `connectChain()` / `connectAll()`.
4. Call `net.node(idx).originate(msg_id)` to send a flood message from a
   client node.
5. Call `net.run(duration_ms)` to drain the event queue.
6. Call `net.collectNodeTotals()` to fold each node's real
   Dispatcher/SimpleMeshTables counters into `net.stats()`, then assert on
   `net.stats()`.

See `test/test_net_sim/test_net_sim.cpp` for worked examples (a 5-node chain
delivery test, a hop-limit drop test, a 10-node fully-connected dedup-bound
test, and the determinism test).
