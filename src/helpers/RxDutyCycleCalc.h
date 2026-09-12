#pragma once
#include <stdint.h>

// Chip-autonomous LoRa RX duty cycling ("sniff mode") window calculation.
//
// Only the SX126x family (SX1262, SX1268, LLCC68, STM32WLx all share RadioLib's SX126x
// SetRxDutyCycle command) can do this in hardware. SX127x has no equivalent command, and LR1110
// has a known erratum where a preamble detected mid-sleep can leave the receiver locked up, so
// both remain permanently unsupported here (see docs/rx_duty_cycle.md).
enum class RadioFamily : uint8_t {
  SX126X,       // SX1262 / SX1268 / LLCC68 / STM32WLx
  UNSUPPORTED   // everything else (SX127x, LR1110, LR2021, ...)
};

struct RxDutyCycleWindow {
  bool supported;          // false => caller must fall back to continuous startReceive()
  uint32_t rxPeriodUs;      // wake (listen) period, microseconds; valid only if supported
  uint32_t sleepPeriodUs;   // sleep period, microseconds; valid only if supported
};

// Computes the Rx/sleep window for chip-autonomous duty-cycled receive, given the radio's current
// spreading factor, bandwidth (kHz) and configured preamble length (symbols).
//
// Datasheet rule applied (Semtech SX126x datasheet rev 2.1, sections 6.1.1.1 and 13.1.7):
//  - Reliably latching a LoRa preamble needs the receiver to observe >= minSymbols consecutive
//    preamble symbols: 12 symbols for SF5/SF6, 8 symbols for SF7-SF12 (6.1.1.1).
//  - Worst case, the sender's preamble starts an instant after we've woken up and is still going
//    when we're about to fall back asleep; to still catch minSymbols before sleeping again, the
//    SLEEP portion (in symbol-periods) must be <= preambleSymbols - 2*minSymbols. If the
//    configured preamble isn't at least 2*minSymbols long there is no safe sleep window at all,
//    and duty cycling must not be used (13.1.7).
//  - The chip's own preamble-detect timeout runs for (sleepPeriod + 2*wakePeriod); this must
//    exceed the full preamble duration, AND the wake period alone must be long enough to observe
//    minSymbols. That gives:
//      wakePeriod = max( (preambleDurationUs - (sleepPeriodUs - 1 symbol overhead)) / 2,
//                         (minSymbols + 1) * symbolPeriodUs )
// This mirrors RadioLib's own PhysicalLayer::calculateRxDutyCycle(), reimplemented from scratch
// here (no RadioLib dependency) so the math is directly unit-testable.
RxDutyCycleWindow calcRxDutyCycleWindow(RadioFamily family, uint8_t sf, float bwKHz, uint16_t preambleSymbols);
