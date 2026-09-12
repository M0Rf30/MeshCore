#include "RxDutyCycleCalc.h"

RxDutyCycleWindow calcRxDutyCycleWindow(RadioFamily family, uint8_t sf, float bwKHz, uint16_t preambleSymbols) {
  RxDutyCycleWindow w = {false, 0, 0};
  if (family != RadioFamily::SX126X) return w;   // no chip-side duty-cycle command available
  if (sf < 5 || sf > 12 || bwKHz <= 0.0f || preambleSymbols == 0) return w;

  uint16_t minSymbols = (sf <= 6) ? 12 : 8;   // datasheet 6.1.1.1
  if (preambleSymbols <= 2 * minSymbols) return w;   // no headroom to sleep safely -> use continuous RX

  double tSymUs = (double)((uint32_t)1 << sf) * 1000.0 / bwKHz;   // symbol period, microseconds

  uint32_t sleepSymbols = preambleSymbols - 2 * minSymbols;
  uint32_t sleepPeriodUs = (uint32_t)(tSymUs * sleepSymbols);

  double preambleUs = tSymUs * (preambleSymbols + 1);
  double wakeA = (preambleUs - ((double)sleepPeriodUs - 1000.0)) / 2.0;   // datasheet 13.1.7 (A)
  double wakeB = tSymUs * (minSymbols + 1);                               // datasheet 13.1.7 (B)
  double wake = wakeA > wakeB ? wakeA : wakeB;

  w.supported = true;
  w.rxPeriodUs = (uint32_t)(wake > 0 ? wake : 0);
  w.sleepPeriodUs = sleepPeriodUs;
  return w;
}
