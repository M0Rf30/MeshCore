#include <gtest/gtest.h>
#include "helpers/ContentionTracker.h"

using namespace mesh;

static void makeHash(uint8_t out[MAX_HASH_SIZE], uint8_t seed) {
  for (int i = 0; i < MAX_HASH_SIZE; i++) out[i] = seed + i;
}

// drives one full retransmit cycle for 'hash': pending -> sent -> 'dupes' echoes heard,
// then advances past the window so the sample is folded into the EMA.
static void runWindow(ContentionTracker& t, uint8_t seed, int dupes, uint32_t& now) {
  uint8_t hash[MAX_HASH_SIZE];
  makeHash(hash, seed);
  t.notePendingRetransmit(hash, now);
  t.markTransmitted(hash, now);
  for (int i = 0; i < dupes; i++) {
    t.noteDupeHeard(hash, now);
  }
  now += CONTENTION_WINDOW_MS + 1;   // push the window closed
  t.noteDupeHeard(hash, now);        // any call after now touches expireStale() and folds the sample
}

// ── EMA convergence ─────────────────────────────────────────────────────────

TEST(ContentionTracker, EmaStartsAtZero) {
  ContentionTracker t;
  EXPECT_FLOAT_EQ(0.0f, t.getEma());
}

TEST(ContentionTracker, EmaConvergesTowardsSteadyDupeRate) {
  ContentionTracker t;
  uint32_t now = 1000;
  for (int i = 0; i < 40; i++) {
    runWindow(t, (uint8_t)(i * 16), 15, now);
  }
  // alpha=0.3 EMA of a constant-15 sequence converges close to 15, never overshoots it
  EXPECT_NEAR(15.0f, t.getEma(), 0.5f);
}

TEST(ContentionTracker, EmaRisesMonotonicallyFromQuietToNoisy) {
  ContentionTracker t;
  uint32_t now = 1000;
  runWindow(t, 1, 0, now);
  float after_quiet = t.getEma();
  runWindow(t, 2, 20, now);
  float after_noisy = t.getEma();
  EXPECT_LT(after_quiet, after_noisy);
}

// ── sqrt-shaped flood delay factor ──────────────────────────────────────────

TEST(ContentionTracker, FactorIsZeroInQuietChain) {
  ContentionTracker t;
  EXPECT_EQ(0, t.getFloodDelayFactorPermille());
}

TEST(ContentionTracker, FactorHitsHalfAtAroundFifteenDupes) {
  ContentionTracker t;
  uint32_t now = 1000;
  // converge the EMA to ~15 dupes/window
  for (int i = 0; i < 60; i++) {
    runWindow(t, (uint8_t)(i * 4), 15, now);
  }
  uint16_t factor_pm = t.getFloodDelayFactorPermille();
  EXPECT_NEAR(500, factor_pm, 60);   // calibrated so EMA==15 -> ~0.5; allow integer-sqrt rounding slack
}

TEST(ContentionTracker, FactorGrowsWithHigherContention) {
  ContentionTracker t;
  uint32_t now = 1000;
  for (int i = 0; i < 60; i++) runWindow(t, (uint8_t)(i * 4), 15, now);
  uint16_t factor_at_15 = t.getFloodDelayFactorPermille();

  ContentionTracker t2;
  now = 1000;
  for (int i = 0; i < 60; i++) runWindow(t2, (uint8_t)(i * 4), 40, now);
  uint16_t factor_at_40 = t2.getFloodDelayFactorPermille();

  EXPECT_GT(factor_at_40, factor_at_15);
}

// ── caps ─────────────────────────────────────────────────────────────────────

TEST(ContentionTracker, FloodJitterCapBindsAtTwoThousandMillis) {
  // airtime large enough that 6*airtime alone would exceed 2000ms
  EXPECT_EQ(2000u, ContentionTracker::floodJitterCap(1000));
}

TEST(ContentionTracker, FloodJitterCapBindsAtSixTimesAirtimeWhenSmaller) {
  // 6*100 == 600, well under the 2000ms ceiling
  EXPECT_EQ(600u, ContentionTracker::floodJitterCap(100));
}

TEST(ContentionTracker, ReactiveBackoffCapBindsAtTwoThousandMillis) {
  EXPECT_EQ(2000u, ContentionTracker::reactiveBackoffCap(1000));
}

TEST(ContentionTracker, ReactiveBackoffCapBindsAtTwelveTimesAirtimeWhenSmaller) {
  EXPECT_EQ(600u, ContentionTracker::reactiveBackoffCap(50));
}

TEST(ContentionTracker, FloodSpreadNeverExceedsItsCap) {
  ContentionTracker t;
  uint32_t now = 1000;
  // drive the EMA very high so the raw factor would blow well past any sane spread
  for (int i = 0; i < 60; i++) runWindow(t, (uint8_t)(i * 4), 250, now);

  uint32_t airtime = 500;
  uint32_t spread = t.getFloodSpreadMs(airtime);
  EXPECT_LE(spread, ContentionTracker::floodJitterCap(airtime));
}

// ── reactive backoff budget ──────────────────────────────────────────────────

TEST(ContentionTracker, ReactiveBackoffAppliesUpToItsCap) {
  ContentionTracker t;
  uint8_t hash[MAX_HASH_SIZE];
  makeHash(hash, 9);
  uint32_t now = 1000;
  t.notePendingRetransmit(hash, now);

  uint32_t cap = 300;
  ASSERT_TRUE(t.noteDupeHeard(hash, now));   // still pending -> reactive backoff applies
  uint32_t room = t.remainingBackoffBudget(hash, cap);
  EXPECT_EQ(cap, room);

  t.recordBackoffApplied(hash, 250);
  EXPECT_EQ(50u, t.remainingBackoffBudget(hash, cap));

  t.recordBackoffApplied(hash, 100);   // would overshoot the cap
  EXPECT_EQ(0u, t.remainingBackoffBudget(hash, cap));
}

TEST(ContentionTracker, BackoffMultiplierZeroDisablesReactiveBackoffOnlyLeavesEmaWindowActive) {
  // simulates Mesh's caller-side "multiplier == 0" path: it never calls recordBackoffApplied()
  // with anything but 0, so the budget stays untouched, while dupe-counting for the EMA
  // continues normally once the packet is actually transmitted.
  ContentionTracker t;
  uint8_t hash[MAX_HASH_SIZE];
  makeHash(hash, 3);
  uint32_t now = 1000;

  t.notePendingRetransmit(hash, now);
  ASSERT_TRUE(t.noteDupeHeard(hash, now));   // reactive-applicable
  t.recordBackoffApplied(hash, 0);           // multiplier == 0 -> nothing ever applied
  EXPECT_EQ(500u, t.remainingBackoffBudget(hash, 500));   // budget untouched

  t.markTransmitted(hash, now);
  for (int i = 0; i < 15; i++) t.noteDupeHeard(hash, now);   // EMA-side counting still works
  now += CONTENTION_WINDOW_MS + 1;
  t.noteDupeHeard(hash, now);   // fold the window

  EXPECT_GT(t.getEma(), 0.0f);
}

// ── window expiry ────────────────────────────────────────────────────────────

TEST(ContentionTracker, StalePendingEntryIsEvictedWithoutContributingASample) {
  ContentionTracker t;
  uint8_t hash[MAX_HASH_SIZE];
  makeHash(hash, 5);
  uint32_t now = 1000;

  t.notePendingRetransmit(hash, now);   // never actually transmitted
  now += CONTENTION_WINDOW_MS + 1;

  EXPECT_FALSE(t.noteDupeHeard(hash, now));   // entry has expired: unknown hash now
  EXPECT_FLOAT_EQ(0.0f, t.getEma());          // no sample was ever folded in
}

TEST(ContentionTracker, SentEntryExpiresAfterWindowAndFoldsIntoEma) {
  ContentionTracker t;
  uint8_t hash[MAX_HASH_SIZE];
  makeHash(hash, 7);
  uint32_t now = 1000;

  t.notePendingRetransmit(hash, now);
  t.markTransmitted(hash, now);
  t.noteDupeHeard(hash, now + 100);
  t.noteDupeHeard(hash, now + 200);

  // an echo just past the window should no longer be attributed to this (now-expired) entry
  uint32_t later = now + CONTENTION_WINDOW_MS + 1;
  EXPECT_FALSE(t.noteDupeHeard(hash, later));
  EXPECT_GT(t.getEma(), 0.0f);   // the 2-dupe sample was folded in when the window closed
}

TEST(ContentionTracker, RingWrapEvictsOldestEntry) {
  ContentionTracker t;
  uint32_t now = 1000;
  uint8_t first[MAX_HASH_SIZE];
  makeHash(first, 0);
  t.notePendingRetransmit(first, now);

  // wrap the whole ring without ever letting the window expire naturally
  for (int i = 1; i <= CONTENTION_TRACK_CAPACITY; i++) {
    uint8_t h[MAX_HASH_SIZE];
    makeHash(h, (uint8_t)(i * 8));
    t.notePendingRetransmit(h, now);
  }

  // the oldest slot ('first') has been recycled: no longer a known pending/sent hash
  EXPECT_FALSE(t.noteDupeHeard(first, now));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
