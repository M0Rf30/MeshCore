#include <gtest/gtest.h>
#include <tuple>
#include "helpers/RxDutyCycleCalc.h"

// symbol period in microseconds, mirrors production formula: Tsym = 2^SF / BW(kHz) * 1000
static double tSymUs(uint8_t sf, float bwKHz) {
  return (double)((uint32_t)1 << sf) * 1000.0 / bwKHz;
}

// ── unsupported radio families / inputs ─────────────────────────────────────

TEST(RxDutyCycleCalc, UnsupportedFamily_ReportsUnsupported) {
  RxDutyCycleWindow w = calcRxDutyCycleWindow(RadioFamily::UNSUPPORTED, 9, 125.0f, 32);
  EXPECT_FALSE(w.supported);
  EXPECT_EQ(w.rxPeriodUs, 0u);
  EXPECT_EQ(w.sleepPeriodUs, 0u);
}

TEST(RxDutyCycleCalc, InvalidSpreadingFactor_ReportsUnsupported) {
  EXPECT_FALSE(calcRxDutyCycleWindow(RadioFamily::SX126X, 4, 125.0f, 32).supported);
  EXPECT_FALSE(calcRxDutyCycleWindow(RadioFamily::SX126X, 13, 125.0f, 32).supported);
}

TEST(RxDutyCycleCalc, ZeroBandwidthOrPreamble_ReportsUnsupported) {
  EXPECT_FALSE(calcRxDutyCycleWindow(RadioFamily::SX126X, 9, 0.0f, 32).supported);
  EXPECT_FALSE(calcRxDutyCycleWindow(RadioFamily::SX126X, 9, 125.0f, 0).supported);
}

// Preamble too short to leave a safe sleep window (<= 2*minSymbols) must fall back to
// continuous RX rather than return a bogus window.
TEST(RxDutyCycleCalc, ShortPreamble_FallsBackToUnsupported) {
  // SF9-12: minSymbols=8, so preamble of 16 (MeshCore's own table for SF>8) is exactly the floor.
  RxDutyCycleWindow w = calcRxDutyCycleWindow(RadioFamily::SX126X, 9, 125.0f, 16);
  EXPECT_FALSE(w.supported);
}

// ── datasheet constraint: SF7-SF12 / 125-500kHz matrix, generous preamble ───
//
// With a preamble long enough to leave headroom (well above 2*minSymbols), the computed window
// must satisfy the SX126x datasheet rule: the chip's internal preamble-detect timeout
// (sleepPeriod + 2*wakePeriod) must be long enough to span the full configured preamble, and the
// wake period alone must be able to observe minSymbols.
class RxDutyCycleMatrix : public ::testing::TestWithParam<std::tuple<uint8_t, float>> {};

TEST_P(RxDutyCycleMatrix, WindowSatisfiesDatasheetConstraint) {
  uint8_t sf = std::get<0>(GetParam());
  float bw = std::get<1>(GetParam());
  const uint16_t preambleSymbols = 64;   // generous, well above any minSymbols floor

  RxDutyCycleWindow w = calcRxDutyCycleWindow(RadioFamily::SX126X, sf, bw, preambleSymbols);
  ASSERT_TRUE(w.supported);

  uint16_t minSymbols = (sf <= 6) ? 12 : 8;
  double tSym = tSymUs(sf, bw);
  double preambleDurationUs = tSym * preambleSymbols;

  // (B) wake period alone must be long enough to observe minSymbols
  EXPECT_GE((double)w.rxPeriodUs, tSym * minSymbols);

  // (A) internal preamble-detect timeout must cover the full preamble duration
  double timeoutUs = (double)w.sleepPeriodUs + 2.0 * (double)w.rxPeriodUs;
  EXPECT_GE(timeoutUs, preambleDurationUs);

  // sleep period must not exceed the datasheet's safe bound: preambleSymbols - 2*minSymbols
  double maxSleepUs = tSym * (preambleSymbols - 2 * minSymbols);
  EXPECT_LE((double)w.sleepPeriodUs, maxSleepUs + 1.0 /* rounding */);

  // duty cycling must actually save power: some real sleep time
  EXPECT_GT(w.sleepPeriodUs, 0u);
}

INSTANTIATE_TEST_SUITE_P(
  SF7to12_BW125to500,
  RxDutyCycleMatrix,
  ::testing::Combine(
    ::testing::Values((uint8_t)7, (uint8_t)8, (uint8_t)9, (uint8_t)10, (uint8_t)11, (uint8_t)12),
    ::testing::Values(125.0f, 250.0f, 500.0f)
  )
);

// ── MeshCore's own preamble table (RadioLibWrapper::preambleLengthForSF) ────
//
// MeshCore uses 32 symbols for SF<=8 and 16 symbols for SF>8. Documents the real-world result:
// duty cycling only has headroom at SF<=8 with this table (see docs/rx_duty_cycle.md).
static uint16_t meshCorePreambleForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }

TEST(RxDutyCycleCalc, MeshCorePreambleTable_LowSF_IsViable) {
  for (uint8_t sf = 5; sf <= 8; sf++) {
    RxDutyCycleWindow w = calcRxDutyCycleWindow(RadioFamily::SX126X, sf, 125.0f, meshCorePreambleForSF(sf));
    EXPECT_TRUE(w.supported) << "sf=" << (int)sf;
    EXPECT_GT(w.sleepPeriodUs, 0u) << "sf=" << (int)sf;
  }
}

TEST(RxDutyCycleCalc, MeshCorePreambleTable_HighSF_HasNoHeadroom) {
  // SF9-12 use a 16-symbol preamble, exactly 2*minSymbols(8) -- no safe sleep window exists.
  for (uint8_t sf = 9; sf <= 12; sf++) {
    RxDutyCycleWindow w = calcRxDutyCycleWindow(RadioFamily::SX126X, sf, 125.0f, meshCorePreambleForSF(sf));
    EXPECT_FALSE(w.supported) << "sf=" << (int)sf;
  }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
