#pragma once

#include <MeshCore.h>
#include <string.h>

namespace mesh {

#ifndef CONTENTION_TRACK_CAPACITY
  #define CONTENTION_TRACK_CAPACITY  12      // small, fixed-size ring of in-flight flood hashes
#endif

#define CONTENTION_WINDOW_MS   10000UL       // how long we keep listening for echoes of our own retransmit

/**
 * \brief  Tracks how many times this node hears its OWN flood retransmissions echoed back by
 *         neighbours, and turns that into a local contention estimate (an EMA of dupes-per-packet).
 *
 *         Two distinct behaviours share the one small ring buffer of recently-retransmitted hashes:
 *          - dupe counting: once a tracked packet has actually gone out (markTransmitted), any echo
 *            heard within CONTENTION_WINDOW_MS bumps that packet's dupe count. When the window
 *            closes, the count is folded into the EMA (see getFloodDelayFactorPermille()).
 *          - reactive backoff: while a tracked packet is still PENDING (queued, not yet transmitted),
 *            hearing an echo of it means a neighbour won the race first; noteDupeHeard() reports this
 *            so the caller can push its own scheduled send further out (bounded by a per-packet cap
 *            tracked here via recordBackoffApplied()/remainingBackoffBudget()).
 *
 *         Pure, deterministic, integer-only: no clock/RNG owned internally, no dynamic allocation,
 *         no dependency on Packet/Mesh -- caller supplies the packet hash and current millis().
 */
class ContentionTracker {
  struct Entry {
    uint8_t  hash[MAX_HASH_SIZE];
    uint32_t started_at;    // when notePendingRetransmit() registered this entry
    uint32_t sent_at;       // when markTransmitted() fired (only valid if 'sent')
    uint16_t dupe_count;    // echoes heard AFTER we transmitted (this window's EMA sample)
    uint16_t backoff_applied; // cumulative reactive-backoff already handed out, ms
    bool     active;
    bool     sent;
  };

  Entry _entries[CONTENTION_TRACK_CAPACITY];
  int _next_idx = 0;
  uint32_t _ema_c100 = 0;   // EMA of dupe-count, fixed point, scaled x100

  static bool elapsedPast(uint32_t now, uint32_t since, uint32_t window) {
    return (int32_t)(now - since) > (int32_t)window;
  }

  int findActive(const uint8_t* hash) const {
    for (int i = 0; i < CONTENTION_TRACK_CAPACITY; i++) {
      if (_entries[i].active && memcmp(_entries[i].hash, hash, MAX_HASH_SIZE) == 0) return i;
    }
    return -1;
  }

  void updateEma(uint16_t dupe_count) {
    int32_t sample_c100 = (int32_t)dupe_count * 100;
    int32_t diff = sample_c100 - (int32_t)_ema_c100;
    _ema_c100 = (uint32_t)((int32_t)_ema_c100 + diff * 3 / 10);   // alpha = 0.3
  }

  void finalize(Entry& e) {
    if (e.sent) updateEma(e.dupe_count);   // never-sent entries contribute no sample
    e.active = false;
  }

  void expireStale(uint32_t now) {
    for (int i = 0; i < CONTENTION_TRACK_CAPACITY; i++) {
      Entry& e = _entries[i];
      if (!e.active) continue;
      uint32_t since = e.sent ? e.sent_at : e.started_at;
      if (elapsedPast(now, since, CONTENTION_WINDOW_MS)) finalize(e);
    }
  }

  // floor(sqrt(n)), rounded to the nearest integer
  static uint32_t isqrtRound(uint32_t n) {
    uint32_t x = n, res = 0;
    uint32_t bit = 1UL << 30;
    while (bit > x) bit >>= 2;
    while (bit != 0) {
      if (x >= res + bit) {
        x -= res + bit;
        res = (res >> 1) + bit;
      } else {
        res >>= 1;
      }
      bit >>= 2;
    }
    if (n - res * res > res) res++;   // round to nearest rather than floor
    return res;
  }

public:
  ContentionTracker() { memset(_entries, 0, sizeof(_entries)); }

  /**
   * \brief  Call when this node has just decided to retransmit a flood packet it received
   *         (ie. right before scheduling it), so its hash can be recognised in wasSeen() echoes.
   */
  void notePendingRetransmit(const uint8_t hash[MAX_HASH_SIZE], uint32_t now) {
    expireStale(now);

    Entry& e = _entries[_next_idx];
    if (e.active) finalize(e);   // ring wrapped onto a still-live slot: evict oldest

    memcpy(e.hash, hash, MAX_HASH_SIZE);
    e.started_at = now;
    e.sent_at = 0;
    e.dupe_count = 0;
    e.backoff_applied = 0;
    e.active = true;
    e.sent = false;
    _next_idx = (_next_idx + 1) % CONTENTION_TRACK_CAPACITY;
  }

  /**
   * \brief  Call (eg. from a logTx() hook) once a tracked packet has actually been transmitted.
   *         Starts the dupe-counting window used to feed the contention EMA.
   */
  void markTransmitted(const uint8_t hash[MAX_HASH_SIZE], uint32_t now) {
    int idx = findActive(hash);
    if (idx >= 0) {
      _entries[idx].sent = true;
      _entries[idx].sent_at = now;
    }
  }

  /**
   * \brief  Call whenever the existing dup-detection (wasSeen()) reports a duplicate flood packet.
   * \returns  true if this hash is still PENDING our own transmit (ie. reactive backoff applies);
   *           false otherwise (unknown hash, or already-sent -- counted toward the EMA instead).
   */
  bool noteDupeHeard(const uint8_t hash[MAX_HASH_SIZE], uint32_t now) {
    expireStale(now);
    int idx = findActive(hash);
    if (idx < 0) return false;

    Entry& e = _entries[idx];
    if (e.sent) {
      if (e.dupe_count < 0xFFFF) e.dupe_count++;
      return false;
    }
    return true;
  }

  /**
   * \returns  remaining reactive-backoff budget (ms) for a still-pending hash, bounded by cap_ms;
   *           0 if the hash isn't a pending entry, or the cap has already been used up.
   */
  uint32_t remainingBackoffBudget(const uint8_t hash[MAX_HASH_SIZE], uint32_t cap_ms) const {
    int idx = findActive(hash);
    if (idx < 0 || _entries[idx].sent) return 0;
    uint32_t applied = _entries[idx].backoff_applied;
    return applied >= cap_ms ? 0 : cap_ms - applied;
  }

  /// Record that 'applied_ms' of reactive backoff has now been handed out for a pending hash.
  void recordBackoffApplied(const uint8_t hash[MAX_HASH_SIZE], uint32_t applied_ms) {
    int idx = findActive(hash);
    if (idx < 0) return;
    uint32_t total = (uint32_t)_entries[idx].backoff_applied + applied_ms;
    _entries[idx].backoff_applied = (uint16_t)(total > 0xFFFF ? 0xFFFF : total);
  }

  /// Current contention estimate: exponential moving average of dupes heard per retransmitted packet.
  float getEma() const { return _ema_c100 / 100.0f; }

  /**
   * \returns  sqrt-shaped flood delay factor, in permille (ie. /1000). ~0 in a quiet linear chain,
   *           calibrated so an EMA of ~15 dupes yields a factor of ~500 (0.5).
   */
  uint16_t getFloodDelayFactorPermille() const {
    uint32_t root = isqrtRound(_ema_c100);       // sqrt(ema*100) == sqrt(ema)*10
    uint32_t fp = (root * 12910UL) / 1000UL;     // 12.910 == 100 * k, k = 0.5/sqrt(15)
    return (uint16_t)(fp > 0xFFFFU ? 0xFFFFU : fp);
  }

  /// double-cap for adaptive flood jitter: min(2000ms, 6*airtime)
  static uint32_t floodJitterCap(uint32_t airtime_ms) {
    uint32_t cap = airtime_ms * 6;
    return cap > 2000 ? 2000 : cap;
  }

  /// double-cap for cumulative reactive backoff: min(2000ms, 12*airtime)
  static uint32_t reactiveBackoffCap(uint32_t airtime_ms) {
    uint32_t cap = airtime_ms * 12;
    return cap > 2000 ? 2000 : cap;
  }

  /// pre-random-draw upper bound (ms) for adaptive flood jitter, already capped
  uint32_t getFloodSpreadMs(uint32_t airtime_ms) const {
    uint32_t spread = (uint32_t)(((uint64_t)airtime_ms * getFloodDelayFactorPermille()) / 1000);
    uint32_t cap = floodJitterCap(airtime_ms);
    return spread > cap ? cap : spread;
  }
};

}
