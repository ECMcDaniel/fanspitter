#pragma once
// Pure frame-comparison logic, kept free of Arduino dependencies so it can be
// unit-tested on the host (see test/test_logic).
#include <stdint.h>

struct PulseMatchParams {
  uint16_t minPulses;   // frames shorter than this never match
  uint8_t  maxLenDiff;  // allowed difference in pulse count (preamble jitter)
  uint8_t  tolPct;      // per-pulse tolerance, % of the longer pulse
  uint16_t tolMinUs;    // per-pulse tolerance floor in µs (ISR latency jitter)
};

// Compare two captured frames pulse-by-pulse.
// On match, writes the element-wise average of the common prefix into `out`
// (which must hold at least min(lenA, lenB) entries) and returns its length.
// Returns 0 if the frames don't match. If `mismatchAt` is non-null it receives
// the index of the first mismatching pulse (or 0xFFFF for a length mismatch).
inline uint16_t matchFrames(const uint16_t* a, uint16_t lenA,
                            const uint16_t* b, uint16_t lenB,
                            const PulseMatchParams& p,
                            uint16_t* out,
                            uint16_t* mismatchAt = nullptr) {
  if (mismatchAt) *mismatchAt = 0xFFFF;
  if (lenA < p.minPulses || lenB < p.minPulses) return 0;
  uint16_t diffLen = (lenA > lenB) ? (lenA - lenB) : (lenB - lenA);
  if (diffLen > p.maxLenDiff) return 0;

  uint16_t len = (lenA < lenB) ? lenA : lenB;
  for (uint16_t i = 0; i < len; i++) {
    uint32_t x = a[i], y = b[i];
    uint32_t ref  = (x > y) ? x : y;
    uint32_t diff = (x > y) ? (x - y) : (y - x);
    uint32_t tol  = ref * p.tolPct / 100;
    if (tol < p.tolMinUs) tol = p.tolMinUs;
    if (diff > tol) {
      if (mismatchAt) *mismatchAt = i;
      return 0;
    }
  }
  for (uint16_t i = 0; i < len; i++) {
    out[i] = (uint16_t)(((uint32_t)a[i] + (uint32_t)b[i]) / 2);
  }
  return len;
}

// Sum of all pulse widths in µs (used to bound how long TX runs with
// interrupts masked).
inline uint32_t frameDurationUs(const uint16_t* pulses, uint16_t count) {
  uint32_t total = 0;
  for (uint16_t i = 0; i < count; i++) total += pulses[i];
  return total;
}
