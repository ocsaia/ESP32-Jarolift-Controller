/*
 * Native unit tests for long-press (SHADE) detection.
 *
 * A Jarolift remote has no SHADE button: holding STOP sends STOP over and over,
 * and the receiver drives to its shade position. ShadeDetector turns that run
 * into one SHADE. It used to count frames - the eleventh STOP - which only
 * meant "about three seconds" because loop() stalled for 280 ms after every
 * decoded frame. With the stall gone, the same count would fire in about a
 * second and a half, so it is now measured as time held. These tests pin the
 * behaviour that has to survive that change, and the one property the frame
 * count never had: it does not depend on how fast frames are decoded.
 */

#include <unity.h>

#include <ShadeDetector.h>

/* H E L P E R S **************************************************************/

static const uint8_t STOP = ShadeDetector::kFnStop;
static const uint8_t SHADE = ShadeDetector::kFnShade;
static const uint8_t UP = 0x8;

// Feed a STOP run of the given length at the given frame interval and return
// the hold time at which SHADE was reported, or -1 if it never was.
static long holdUntilShade(ShadeDetector &d, uint32_t serial, uint32_t startMs, uint32_t intervalMs, uint32_t durationMs,
                           int *shadeCount = nullptr) {
  long firedAt = -1;
  int count = 0;
  for (uint32_t t = 0; t <= durationMs; t += intervalMs) {
    if (d.update(serial, 0x40, STOP, startMs + t) == SHADE) {
      count++;
      if (firedAt < 0) {
        firedAt = (long)t;
      }
    }
  }
  if (shadeCount != nullptr) {
    *shadeCount = count;
  }
  return firedAt;
}

void setUp(void) {}
void tearDown(void) {}

/* T E S T S ******************************************************************/

static void test_a_short_stop_press_stays_stop() {
  ShadeDetector d;
  TEST_ASSERT_EQUAL_INT(-1, holdUntilShade(d, 0x1a4a06, 1000, 150, 1500));
}

static void test_holding_stop_long_enough_reports_one_shade() {
  ShadeDetector d;
  int count = 0;
  long at = holdUntilShade(d, 0x1a4a06, 1000, 150, 3600, &count);
  TEST_ASSERT_EQUAL_INT(1, count);
  TEST_ASSERT_TRUE_MESSAGE(at >= (long)ShadeDetector::kHoldMs, "SHADE reported before the hold time");
  TEST_ASSERT_TRUE_MESSAGE(at < (long)ShadeDetector::kHoldMs + 150, "SHADE reported later than the first frame past the hold time");
}

// The property the frame count lacked. At 150 ms per frame, eleven frames were
// 1.5 s; at 400 ms they were 4 s. Time held is the same either way.
static void test_the_hold_time_does_not_depend_on_the_frame_rate() {
  ShadeDetector fast, slow;
  long atFast = holdUntilShade(fast, 0x1a4a06, 1000, 150, 5000);
  long atSlow = holdUntilShade(slow, 0x1a4a06, 1000, 400, 5000);
  TEST_ASSERT_TRUE(atFast >= (long)ShadeDetector::kHoldMs && atFast < (long)ShadeDetector::kHoldMs + 150);
  TEST_ASSERT_TRUE(atSlow >= (long)ShadeDetector::kHoldMs && atSlow < (long)ShadeDetector::kHoldMs + 400);
}

// Holding on after the SHADE must not produce a second one; the rest of the run
// is still reported as STOP.
static void test_holding_on_after_the_shade_reports_stop() {
  ShadeDetector d;
  int count = 0;
  holdUntilShade(d, 0x1a4a06, 1000, 200, 10000, &count);
  TEST_ASSERT_EQUAL_INT(1, count);
  TEST_ASSERT_EQUAL_UINT8(STOP, d.update(0x1a4a06, 0x40, STOP, 1000 + 10200));
}

// Two separate STOP presses must not add up to a long one.
static void test_a_pause_ends_the_run() {
  ShadeDetector d;
  TEST_ASSERT_EQUAL_INT(-1, holdUntilShade(d, 0x1a4a06, 1000, 200, 2000));
  uint32_t second = 1000 + 2000 + ShadeDetector::kGapMs + 1;
  TEST_ASSERT_EQUAL_INT(-1, holdUntilShade(d, 0x1a4a06, second, 200, 2000));
}

static void test_another_button_ends_the_run() {
  ShadeDetector d;
  holdUntilShade(d, 0x1a4a06, 1000, 200, 2000);
  TEST_ASSERT_EQUAL_UINT8(UP, d.update(0x1a4a06, 0x40, UP, 3100));
  TEST_ASSERT_EQUAL_INT(-1, holdUntilShade(d, 0x1a4a06, 3200, 200, 2000));
}

static void test_another_remote_starts_its_own_run() {
  ShadeDetector d;
  holdUntilShade(d, 0x1a4a06, 1000, 200, 2000);
  // a different transmitter carries on at once: it must not inherit two seconds
  TEST_ASSERT_EQUAL_INT(-1, holdUntilShade(d, 0x1a4a07, 3200, 200, 2000));
}

// The caller passes only the low byte of the channel mask. A change in it is a
// different button on a multi-channel handset and starts a new run.
static void test_a_different_channel_byte_starts_a_new_run() {
  ShadeDetector d;
  for (uint32_t t = 0; t <= 2000; t += 200) {
    d.update(0x1a4a00, 0x40, STOP, 1000 + t);
  }
  for (uint32_t t = 0; t <= 2000; t += 200) {
    TEST_ASSERT_EQUAL_UINT8(STOP, d.update(0x1a4a00, 0x80, STOP, 3200 + t));
  }
}

// Channels 9-16 of a handset with a single serial differ only in the high byte
// of the mask. With frames now taken whole that byte is reliable, and two such
// buttons are two runs.
static void test_channels_differing_only_in_the_high_byte_are_separate_runs() {
  ShadeDetector d;
  for (uint32_t t = 0; t <= 2000; t += 200) {
    d.update(0x1a4a00, 0x0100, STOP, 1000 + t);
  }
  for (uint32_t t = 0; t <= 2000; t += 200) {
    TEST_ASSERT_EQUAL_UINT8(STOP, d.update(0x1a4a00, 0x0200, STOP, 3200 + t));
  }
}

static void test_a_new_long_press_after_the_first_gives_a_new_shade() {
  ShadeDetector d;
  holdUntilShade(d, 0x1a4a06, 1000, 200, 3600);
  uint32_t again = 1000 + 3600 + ShadeDetector::kGapMs + 1;
  int count = 0;
  holdUntilShade(d, 0x1a4a06, again, 200, 3600, &count);
  TEST_ASSERT_EQUAL_INT(1, count);
}

static void test_a_press_held_across_the_millis_wrap_still_counts() {
  ShadeDetector d;
  int count = 0;
  holdUntilShade(d, 0x1a4a06, 0xFFFFF000u, 200, 3600, &count);
  TEST_ASSERT_EQUAL_INT(1, count);
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_a_short_stop_press_stays_stop);
  RUN_TEST(test_holding_stop_long_enough_reports_one_shade);
  RUN_TEST(test_the_hold_time_does_not_depend_on_the_frame_rate);
  RUN_TEST(test_holding_on_after_the_shade_reports_stop);
  RUN_TEST(test_a_pause_ends_the_run);
  RUN_TEST(test_another_button_ends_the_run);
  RUN_TEST(test_another_remote_starts_its_own_run);
  RUN_TEST(test_a_different_channel_byte_starts_a_new_run);
  RUN_TEST(test_channels_differing_only_in_the_high_byte_are_separate_runs);
  RUN_TEST(test_a_new_long_press_after_the_first_gives_a_new_shade);
  RUN_TEST(test_a_press_held_across_the_millis_wrap_still_counts);

  return UNITY_END();
}
