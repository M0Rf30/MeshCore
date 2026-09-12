# Adaptive Contention

Flood-retransmit timing used to be a fixed, prefs-controlled random window (`txdelay` /
`direct.txdelay`): every node picked a delay uniformly at random from a range scaled by a
configured factor, regardless of how busy the local radio neighbourhood actually was. That range
had to be guessed once, at flash time, for every deployment.

`ContentionTracker` (`src/helpers/ContentionTracker.h`) replaces that guess with a measurement:
each node watches how often its own flood retransmissions get echoed back by neighbours, and uses
that to size its own jitter window.

## Mechanism

1. **Dupe counting.** When this node retransmits a flood packet, its hash is tracked in a small
   fixed-size ring (12 entries). Any echo of that same packet heard from a neighbour within the
   following 10 seconds increments that packet's dupe count. When the window closes, the count is
   folded into an exponential moving average (EMA, alpha 0.3) -- the node's running estimate of
   local contention.
2. **Flood delay factor.** The EMA is passed through a sqrt-shaped curve (`0` when quiet, rising
   with density, calibrated so an EMA of ~15 dupes/packet yields a factor of ~0.5) to produce the
   spread used for `getRetransmitDelay()`. The resulting jitter window is double-capped at
   `min(2000ms, 6x airtime)`.
3. **Reactive per-packet backoff.** While a specific flood packet is still queued for this node's
   own retransmit (not yet sent), hearing a neighbour retransmit that exact packet first means this
   node lost the race. Its own send is pushed back by a random amount up to
   `backoff.multiplier x airtime`, cumulative extension capped at `min(2000ms, 12x airtime)` per
   packet. Setting `backoff.multiplier` to `0` disables this push-back only -- the EMA above keeps
   updating regardless.
4. **Locally-originated floods** (adverts, replies the node itself sends via `sendFlood()`) have no
   dupe history to adapt from, since nothing has echoed them yet. They get a small, non-adaptive
   anti-collision spread instead: up to `min(1000ms, 3x airtime)`.
5. **Direct (routed) retransmits** only ever have one node retransmitting a given packet, so
   adaptive delay buys nothing. They keep a small fixed (non-adaptive) jitter window.

## CLI

- `get txdelay` reports the live state: the current contention EMA and the resulting flood delay
  factor, in place of the old fixed number.
- `set txdelay` / `set direct.txdelay` still parse, validate and persist their values (for
  pref-file and binary compatibility with older tools and GUIs), but the stored values are no
  longer read by the retransmit path -- they are inert.
- `get/set backoff.multiplier` (default `0.2`, range `0.0`-`2.0`) controls the reactive backoff
  described above.

## Interoperability

This is purely local, receive-side behaviour: it only changes *when* this node chooses to
transmit its own copy of a packet it already decided to forward. It does not change the packet
format, the flood/direct routing logic, or anything observable over the air beyond timing. A node
running this scheme interoperates transparently with existing deployed nodes still using static
`txdelay`/`direct.txdelay` jitter -- from a neighbour's point of view it is just another node that
happens to pick its retransmit delay differently.
