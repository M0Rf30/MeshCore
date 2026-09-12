# RX Duty Cycling ("Sniff Mode")

## What it is

SX126x-family radios (SX1262, SX1268, LLCC68, STM32WLx) can be told to sleep between short
preamble-detect windows instead of listening continuously, via the chip's `SetRxDutyCycle`
command. If an incoming preamble is detected during a listen window, the chip automatically stays
awake and receives the rest of the packet as normal -- this is entirely handled by the radio
hardware, invisibly to the firmware's receive path.

This reduces RX current draw at the cost of a small chance of missing the very start of a
transmission that happens to fall in a sleep window (bounded by the window calculation below, so a
correctly-configured preamble is never actually missed).

MeshCore enables this per-node with the `rxduty` setting (default **off**):

```
get rxduty
set rxduty on
set rxduty off
```

See [docs/cli_commands.md](cli_commands.md) for full command details.

## Supported radios

| Radio family                        | Supported | Why |
|--------------------------------------|-----------|-----|
| SX1262, SX1268, LLCC68, STM32WLx     | Yes       | All share RadioLib's SX126x `SetRxDutyCycle` command |
| LR1110                                | No        | Known erratum: a preamble detected mid-sleep can leave the receiver locked up |
| SX1276 (SX127x family)                | No        | No equivalent radio command exists |
| LR2021                                | No        | Out of scope for this feature; falls back to continuous RX like any other unsupported radio |

`get rxduty` reports `unsupported` on any radio that isn't in the first row above. `set rxduty on`
on an unsupported radio is accepted (so switching hardware later doesn't require a config change)
but has no effect -- the radio keeps using continuous RX.

## Window calculation

The Rx (wake) and sleep periods are computed from the node's current spreading factor,
bandwidth, and preamble length, following the Semtech SX126x datasheet (rev 2.1, sections 6.1.1.1
and 13.1.7):

- Reliably latching a LoRa preamble needs the receiver to observe at least `minSymbols` consecutive
  preamble symbols: 12 symbols for SF5/SF6, 8 symbols for SF7-SF12.
- Worst case, the sender's preamble begins just as the receiver is about to go back to sleep. To
  still catch `minSymbols` before sleeping, the sleep period (in symbol-periods) must be at most
  `preambleSymbols - 2*minSymbols`.
- The chip's own preamble-detect timeout runs for `sleepPeriod + 2*wakePeriod`; this must exceed the
  full preamble duration, and the wake period alone must be long enough to observe `minSymbols`.

This is implemented as a pure function in
[`src/helpers/RxDutyCycleCalc.h`](../src/helpers/RxDutyCycleCalc.h) /
`src/helpers/RxDutyCycleCalc.cpp`, unit tested in `test/test_rx_duty_cycle/`, and recomputed by
`RadioLibWrapper::updateDutyCycleWindow()` every time `setParams()` runs (i.e. whenever SF/BW/CR
changes, including `set radio`, `tempradio` and boot-time configuration).

### Real-world limitation with MeshCore's current preamble table

`RadioLibWrapper::preambleLengthForSF()` uses a 32-symbol preamble for SF5-SF8 and a 16-symbol
preamble for SF9-SF12 (chosen to keep airtime down at high SF). Since `minSymbols` is 8 for
SF7-SF12, the SF9-SF12 preamble (16 symbols) is exactly `2*minSymbols` -- there is no headroom left
to sleep at all. In practice:

- **SF5-SF8:** duty cycling is viable; the sleep window is `preambleSymbols - 2*minSymbols` symbol
  periods (8 symbols at SF5/SF6, 16 symbols at SF7/SF8).
- **SF9-SF12:** `set rxduty on` is accepted, but `updateDutyCycleWindow()` computes an unsupported
  (zero) window and the radio keeps using continuous RX, because there is no way to sleep without
  risking missing a preamble under this preamble table.

Expected current saving, computed from `calcRxDutyCycleWindow()` for two common SF5-SF8
configurations (rx = wake window, sleep = sleep window per cycle):

| Config                          | rx window | sleep window | awake fraction |
|----------------------------------|-----------|--------------|----------------|
| SF8 / BW62.5 (build default)     | 36.9 ms   | 65.5 ms      | ~36%           |
| SF7 / BW250                       | 4.9 ms    | 8.2 ms       | ~37%           |

i.e. the radio sleeps roughly 63-64% of the time it would otherwise have spent in continuous RX, at
these SF/BW combinations. Actual current saving also depends on the radio's own sleep-vs-RX current
draw and how much of the node's power budget is RX-dominated versus MCU/peripherals.

## Continuous-RX assumptions that were checked

- **`isInRecvMode()`** only checks the internal `state == STATE_RX` flag, which is set the same way
  regardless of whether continuous or duty-cycled receive was armed. No change needed.
- **CAD / `isReceiving()`** (used by `Dispatcher::checkSend()` to avoid transmitting over an
  in-flight packet) relies on `isReceivingPacket()` reading the SX126x `PREAMBLE_DETECTED` /
  `HEADER_VALID` IRQ bits. RadioLib's `startReceiveDutyCycle()` defaults to a *different* IRQ mask
  than `Custom*::startReceive()` (the latter explicitly ORs in `PREAMBLE_DETECTED`). Duty-cycle
  receive is therefore armed with that same explicit IRQ mask
  (`SX126xDutyCycleWrapper::startReceiveDutyCycleRaw()`) -- without this fix, CAD would never see a
  mid-cycle preamble detection and collision avoidance would silently stop working while duty
  cycling was enabled.
- **TX -> RX restoration:** `onSendFinished()` resets to `STATE_IDLE`; the next `recvRaw()`/
  `startRecv()` call re-arms receive via the shared `doStartReceive()` helper, which picks
  duty-cycle or continuous mode the same way in both call sites. No separate handling needed.
- **Failure fallback:** if the duty-cycle command itself fails (invalid rx/sleep period,
  chip busy, etc.), `doStartReceive()` falls back to continuous `startReceive()` in the same call,
  so the node never gets stuck without RX.
