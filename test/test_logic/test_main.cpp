// Host-side tests: pio test -e native
#include <unity.h>
#include <string.h>
#include "slot_name.h"
#include "pulse_match.h"

static const PulseMatchParams P = { /*minPulses*/ 10, /*maxLenDiff*/ 2,
                                    /*tolPct*/ 20, /*tolMinUs*/ 50 };

void setUp() {}
void tearDown() {}

static bool valid(const char* s) { return isValidSlotName(s, strlen(s)); }

// ── slot names ──────────────────────────────────────────────────────────────
void test_slot_name_accepts_safe_names() {
  TEST_ASSERT_TRUE(valid("fan_high"));
  TEST_ASSERT_TRUE(valid("light-toggle"));
  TEST_ASSERT_TRUE(valid("A"));
  TEST_ASSERT_TRUE(valid("01234567890123456789012345678901"));  // 32 chars
}

void test_slot_name_rejects_unsafe_names() {
  TEST_ASSERT_FALSE(valid(""));
  TEST_ASSERT_FALSE(valid("012345678901234567890123456789012"));  // 33 chars
  TEST_ASSERT_FALSE(valid("fan high"));
  TEST_ASSERT_FALSE(valid("x');alert(1);//"));
  TEST_ASSERT_FALSE(valid("<img>"));
  TEST_ASSERT_FALSE(valid("a/b"));
  TEST_ASSERT_FALSE(valid("caf\xc3\xa9"));
  TEST_ASSERT_FALSE(isValidSlotName(nullptr, 3));
}

// ── frame matching ──────────────────────────────────────────────────────────
static void fill(uint16_t* buf, uint16_t n, uint16_t shortUs, uint16_t longUs) {
  for (uint16_t i = 0; i < n; i++) buf[i] = (i % 3 == 0) ? longUs : shortUs;
}

void test_identical_frames_match_and_average() {
  uint16_t a[24], b[24], out[24];
  fill(a, 24, 400, 1200);
  fill(b, 24, 400, 1200);
  TEST_ASSERT_EQUAL_UINT16(24, matchFrames(a, 24, b, 24, P, out));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(a, out, 24);
}

void test_jitter_within_tolerance_matches() {
  uint16_t a[20], b[20], out[20];
  fill(a, 20, 400, 1200);
  fill(b, 20, 440, 1100);  // +10% / -8%
  TEST_ASSERT_EQUAL_UINT16(20, matchFrames(a, 20, b, 20, P, out));
  TEST_ASSERT_EQUAL_UINT16(1150, out[0]);  // (1200 + 1100) / 2
  TEST_ASSERT_EQUAL_UINT16(420, out[1]);   // (400 + 440) / 2
}

void test_different_codes_do_not_match() {
  uint16_t a[20], b[20], out[20];
  uint16_t at = 0;
  fill(a, 20, 400, 1200);
  fill(b, 20, 400, 1200);
  b[7] = 1200;  // a short pulse became a long one → different button
  TEST_ASSERT_EQUAL_UINT16(0, matchFrames(a, 20, b, 20, P, out, &at));
  TEST_ASSERT_EQUAL_UINT16(7, at);
}

void test_tolerance_floor_absorbs_isr_jitter_on_short_pulses() {
  uint16_t a[12], b[12], out[12];
  for (int i = 0; i < 12; i++) { a[i] = 100; b[i] = 140; }  // 40 µs > 20% but < 50 µs floor
  TEST_ASSERT_EQUAL_UINT16(12, matchFrames(a, 12, b, 12, P, out));
  b[5] = 160;  // 60 µs off → beyond the floor
  TEST_ASSERT_EQUAL_UINT16(0, matchFrames(a, 12, b, 12, P, out));
}

void test_small_length_difference_uses_common_prefix() {
  uint16_t a[22], b[20], out[22];
  fill(a, 22, 400, 1200);
  fill(b, 20, 400, 1200);
  TEST_ASSERT_EQUAL_UINT16(20, matchFrames(a, 22, b, 20, P, out));
}

void test_large_length_difference_rejected() {
  uint16_t a[30], b[20], out[30];
  uint16_t at = 0;
  fill(a, 30, 400, 1200);
  fill(b, 20, 400, 1200);
  TEST_ASSERT_EQUAL_UINT16(0, matchFrames(a, 30, b, 20, P, out, &at));
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, at);
}

void test_short_frames_rejected() {
  uint16_t a[9], b[9], out[9];
  fill(a, 9, 400, 1200);
  fill(b, 9, 400, 1200);
  TEST_ASSERT_EQUAL_UINT16(0, matchFrames(a, 9, b, 9, P, out));
}

void test_no_overflow_on_max_width_pulses() {
  uint16_t a[10], b[10], out[10];
  for (int i = 0; i < 10; i++) { a[i] = 65535; b[i] = 65000; }
  TEST_ASSERT_EQUAL_UINT16(10, matchFrames(a, 10, b, 10, P, out));
  TEST_ASSERT_EQUAL_UINT16(65267, out[0]);
}

void test_frame_duration() {
  uint16_t a[4] = { 1000, 2000, 3000, 65535 };
  TEST_ASSERT_EQUAL_UINT32(71535, frameDurationUs(a, 4));
  TEST_ASSERT_EQUAL_UINT32(0, frameDurationUs(a, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_slot_name_accepts_safe_names);
  RUN_TEST(test_slot_name_rejects_unsafe_names);
  RUN_TEST(test_identical_frames_match_and_average);
  RUN_TEST(test_jitter_within_tolerance_matches);
  RUN_TEST(test_different_codes_do_not_match);
  RUN_TEST(test_tolerance_floor_absorbs_isr_jitter_on_short_pulses);
  RUN_TEST(test_small_length_difference_uses_common_prefix);
  RUN_TEST(test_large_length_difference_rejected);
  RUN_TEST(test_short_frames_rejected);
  RUN_TEST(test_no_overflow_on_max_width_pulses);
  RUN_TEST(test_frame_duration);
  return UNITY_END();
}
