/*
 * Native unit tests for the time-based position tracker.
 *
 * The tracker is the one piece of new firmware whose correctness is not
 * observable by compiling: it is a state machine over a clock, and the ways it
 * can be wrong - a retarget that restarts the motor, a span that goes negative,
 * a deadline that a millis() rollover pushes 49 days into the future - all look
 * fine to the compiler. These tests drive it on the host with a clock they
 * control, so the shutter never has to move for the logic to be checked.
 *
 * What is NOT covered here, and still needs hardware: whether the measured
 * travel time matches the real shutter, whether the RF STOP is actually
 * received, and how much the loop() jitter costs in practice.
 */

#include <unity.h>

#include <config.h>
#include <shutterPos.h>

// The module under test is compiled straight into this translation unit so the
// test binary does not have to link the rest of the firmware.
#include "../../src/shutterPos.cpp"

/* T E S T   D O U B L E S ****************************************************/

uint32_t testMillis = 0;
bool testLogEcho = false;

s_config config;

// what the tracker asked the radio to do, in order
struct s_radioCall {
  enum Kind { CMD_UP_CALL, CMD_DOWN_CALL, STOP_NOW } kind;
  uint8_t channel;
};

static s_radioCall radioCalls[32];
static int radioCallCount = 0;

static uint8_t lastPublishedChannel = 0xFF;
static int lastPublishedPosition = -1;
static int publishCount = 0;

void jaroCmd(JaroCmdType type, uint8_t channel) {
  if (radioCallCount < 32) {
    radioCalls[radioCallCount].kind = (type == CMD_DOWN) ? s_radioCall::CMD_DOWN_CALL : s_radioCall::CMD_UP_CALL;
    radioCalls[radioCallCount].channel = channel;
    radioCallCount++;
  }
}

void jaroStopNow(uint8_t channel) {
  if (radioCallCount < 32) {
    radioCalls[radioCallCount].kind = s_radioCall::STOP_NOW;
    radioCalls[radioCallCount].channel = channel;
    radioCallCount++;
  }
}

void mqttSendPosition(uint8_t channel, uint8_t position) {
  lastPublishedChannel = channel;
  lastPublishedPosition = position;
  publishCount++;
}

/* H E L P E R S **************************************************************/

#define CH 0
#define TRAVEL_MS 20000u

static int stopNowCount() {
  int n = 0;
  for (int i = 0; i < radioCallCount; i++) {
    if (radioCalls[i].kind == s_radioCall::STOP_NOW) {
      n++;
    }
  }
  return n;
}

static int moveCmdCount() {
  int n = 0;
  for (int i = 0; i < radioCallCount; i++) {
    if (radioCalls[i].kind != s_radioCall::STOP_NOW) {
      n++;
    }
  }
  return n;
}

// Advance the clock in small steps, running the cyclic function each time, the
// way loop() would. A single jump would let a deadline pass unnoticed.
static void advance(uint32_t ms) {
  const uint32_t step = 25;
  for (uint32_t elapsed = 0; elapsed < ms; elapsed += step) {
    testMillis += step;
    shutterPosCyclic();
  }
}

// Drive the channel to a known position by running it to an end-stop.
static void anchorAt(int8_t endStop) {
  if (endStop == 0) {
    shutterPosNotifyDown(CH);
  } else {
    shutterPosNotifyUp(CH);
  }
  advance(TRAVEL_MS + 500);
}

void setUp() {
  testMillis = 100000; // not zero: startMs == 0 must not be special
  radioCallCount = 0;
  publishCount = 0;
  lastPublishedPosition = -1;
  lastPublishedChannel = 0xFF;

  config = s_config{};
  config.jaro.ch_travel_down[CH] = TRAVEL_MS;
  config.jaro.ch_travel_up[CH] = TRAVEL_MS;
  std::snprintf(config.jaro.ch_name[CH], sizeof(config.jaro.ch_name[CH]), "test channel");

  shutterPosSetup();
}

void tearDown() {}

/* T E S T S ******************************************************************/

// An uncalibrated channel has to keep working exactly as the firmware did
// before position tracking existed: report the end position at once.
static void test_uncalibrated_channel_settles_at_once() {
  config.jaro.ch_travel_down[CH] = 0;
  config.jaro.ch_travel_up[CH] = 0;

  shutterPosNotifyDown(CH);

  TEST_ASSERT_FALSE(shutterPosIsCalibrated(CH));
  TEST_ASSERT_FALSE(shutterPosIsMoving(CH));
  TEST_ASSERT_EQUAL_INT(0, shutterPosGet(CH));
  TEST_ASSERT_EQUAL_INT(0, lastPublishedPosition);
}

// Position is unknown until a shutter has been to an end-stop, because a
// remote may have moved it while the controller was off.
static void test_position_starts_unknown() { TEST_ASSERT_EQUAL_INT(SHUTTER_POS_UNKNOWN, shutterPosGet(CH)); }

static void test_full_travel_down_anchors_at_closed() {
  shutterPosNotifyDown(CH);
  TEST_ASSERT_TRUE(shutterPosIsMoving(CH));

  advance(TRAVEL_MS + 500);

  TEST_ASSERT_FALSE(shutterPosIsMoving(CH));
  TEST_ASSERT_EQUAL_INT(0, shutterPosGet(CH));
  // an end-stop run needs no timed stop - the motor's own limit switch ends it
  TEST_ASSERT_EQUAL_INT(0, stopNowCount());
}

static void test_full_travel_up_anchors_at_open() {
  anchorAt(0);
  shutterPosNotifyUp(CH);
  advance(TRAVEL_MS + 500);

  TEST_ASSERT_EQUAL_INT(100, shutterPosGet(CH));
  TEST_ASSERT_EQUAL_INT(0, stopNowCount());
}

// An intermediate target cannot be honoured without knowing where we are.
static void test_intermediate_target_refused_while_position_unknown() {
  TEST_ASSERT_FALSE(shutterPosSetTarget(CH, 50));
  TEST_ASSERT_EQUAL_INT(0, moveCmdCount());
}

static void test_endstop_target_accepted_while_position_unknown() {
  TEST_ASSERT_TRUE(shutterPosSetTarget(CH, 0));
  TEST_ASSERT_EQUAL_INT(1, moveCmdCount());
  TEST_ASSERT_EQUAL_INT(s_radioCall::CMD_DOWN_CALL, radioCalls[0].kind);
}

static void test_uncalibrated_channel_refuses_position_command() {
  config.jaro.ch_travel_down[CH] = 0;
  TEST_ASSERT_FALSE(shutterPosSetTarget(CH, 50));
  TEST_ASSERT_EQUAL_INT(0, moveCmdCount());
}

// The core of the feature: from fully open, a 60 % target must run for 40 % of
// the travel time and then stop.
static void test_timed_stop_lands_on_target() {
  anchorAt(100);
  radioCallCount = 0;

  TEST_ASSERT_TRUE(shutterPosSetTarget(CH, 60));
  TEST_ASSERT_EQUAL_INT(1, moveCmdCount());
  TEST_ASSERT_EQUAL_INT(s_radioCall::CMD_DOWN_CALL, radioCalls[0].kind);

  shutterPosNotifyDown(CH); // the telegram goes out - the stopwatch starts here

  advance(TRAVEL_MS * 40 / 100 - 1000);
  TEST_ASSERT_TRUE_MESSAGE(shutterPosIsMoving(CH), "stopped too early");
  TEST_ASSERT_EQUAL_INT(0, stopNowCount());

  advance(2000);
  TEST_ASSERT_FALSE_MESSAGE(shutterPosIsMoving(CH), "did not stop at the target");
  TEST_ASSERT_EQUAL_INT(1, stopNowCount());
  TEST_ASSERT_EQUAL_INT(60, shutterPosGet(CH));
  TEST_ASSERT_EQUAL_INT(60, lastPublishedPosition);
}

// Retargeting the same way must move the deadline, not re-issue the command:
// a fresh DOWN would restart the motor and throw away the elapsed travel the
// current estimate is built on.
static void test_retarget_same_direction_does_not_restart_the_motor() {
  anchorAt(100);
  shutterPosSetTarget(CH, 40);
  shutterPosNotifyDown(CH);
  advance(TRAVEL_MS * 20 / 100); // roughly 80 % left
  radioCallCount = 0;

  TEST_ASSERT_TRUE(shutterPosSetTarget(CH, 20));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, moveCmdCount(), "motor was restarted");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, stopNowCount(), "motor was stopped");
  TEST_ASSERT_TRUE(shutterPosIsMoving(CH));

  advance(TRAVEL_MS);
  TEST_ASSERT_FALSE(shutterPosIsMoving(CH));
  TEST_ASSERT_EQUAL_INT(20, shutterPosGet(CH));
}

// Reversing needs an explicit stop first, otherwise the receiver ignores it.
static void test_direction_change_stops_before_reversing() {
  anchorAt(100);
  shutterPosSetTarget(CH, 20);
  shutterPosNotifyDown(CH);
  advance(TRAVEL_MS * 30 / 100);
  radioCallCount = 0;

  TEST_ASSERT_TRUE(shutterPosSetTarget(CH, 90));

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, stopNowCount(), "reversed without stopping");
  TEST_ASSERT_EQUAL_INT(s_radioCall::STOP_NOW, radioCalls[0].kind);
  TEST_ASSERT_EQUAL_INT(s_radioCall::CMD_UP_CALL, radioCalls[1].kind);
}

// Asking for the position the shutter is already at, while it is running, must
// stop it. The direction test reads "is the target below me", which is false at
// equality - so the naive path sends it upwards and relies on the span guard to
// pull it back, after a wasted telegram and a small overshoot.
static void test_target_equal_to_current_position_stops_instead_of_reversing() {
  anchorAt(100);
  shutterPosSetTarget(CH, 20);
  shutterPosNotifyDown(CH);
  advance(TRAVEL_MS * 30 / 100);

  int8_t here = shutterPosGet(CH);
  TEST_ASSERT_TRUE_MESSAGE(here > 20 && here < 100, "test setup did not reach an intermediate position");
  radioCallCount = 0;

  TEST_ASSERT_TRUE(shutterPosSetTarget(CH, (uint8_t)here));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, stopNowCount(), "did not stop");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, moveCmdCount(), "reversed instead of stopping");
  TEST_ASSERT_FALSE(shutterPosIsMoving(CH));
  TEST_ASSERT_EQUAL_INT(here, shutterPosGet(CH));
}

// millis() wraps every 49 days. An unsigned comparison would read the deadline
// as being far in the future and the shutter would run to its end-stop.
static void test_deadline_survives_a_millis_rollover() {
  testMillis = 0xFFFFF000u; // about 4 s before the wrap
  shutterPosSetup();
  radioCallCount = 0;

  // anchor without letting the clock wrap yet
  shutterPosNotifyUp(CH);
  advance(TRAVEL_MS + 500); // this crosses the wrap
  TEST_ASSERT_EQUAL_INT(100, shutterPosGet(CH));

  radioCallCount = 0;
  TEST_ASSERT_TRUE(shutterPosSetTarget(CH, 50));
  shutterPosNotifyDown(CH);
  advance(TRAVEL_MS * 50 / 100 + 1000);

  TEST_ASSERT_FALSE_MESSAGE(shutterPosIsMoving(CH), "deadline was lost across the rollover");
  TEST_ASSERT_EQUAL_INT(1, stopNowCount());
  TEST_ASSERT_EQUAL_INT(50, shutterPosGet(CH));
}

// With only the DOWN direction measured, UP is derived with a margin rather
// than assumed symmetric.
static void test_up_travel_falls_back_to_a_factor_of_down() {
  config.jaro.ch_travel_up[CH] = 0;
  anchorAt(0);
  radioCallCount = 0;

  shutterPosSetTarget(CH, 50);
  shutterPosNotifyUp(CH);

  // symmetric would stop at half of TRAVEL_MS; the fallback must run longer
  advance(TRAVEL_MS * 50 / 100 + 200);
  TEST_ASSERT_TRUE_MESSAGE(shutterPosIsMoving(CH), "used the DOWN time for an UP move");

  advance(TRAVEL_MS);
  TEST_ASSERT_FALSE(shutterPosIsMoving(CH));
  TEST_ASSERT_EQUAL_INT(50, shutterPosGet(CH));
}

// A physical remote moves the shutter too. The tracker follows those, otherwise
// one press of a wall remote invalidates every estimate until the next
// end-stop.
static void test_remote_driven_movement_is_tracked() {
  anchorAt(100);
  radioCallCount = 0;

  shutterPosNotifyDown(CH); // remote DOWN observed on the air
  advance(TRAVEL_MS * 25 / 100);
  shutterPosNotifyStop(CH); // remote STOP observed

  TEST_ASSERT_FALSE(shutterPosIsMoving(CH));
  int8_t pos = shutterPosGet(CH);
  TEST_ASSERT_INT_WITHIN_MESSAGE(3, 75, pos, "interpolated position is off");
  TEST_ASSERT_EQUAL_INT(pos, lastPublishedPosition);
  // the controller did not transmit anything - it only watched
  TEST_ASSERT_EQUAL_INT(0, radioCallCount);
}

static void test_out_of_range_channel_is_rejected() {
  TEST_ASSERT_FALSE(shutterPosSetTarget(99, 50));
  TEST_ASSERT_FALSE(shutterPosIsCalibrated(99));
  TEST_ASSERT_FALSE(shutterPosIsMoving(99));
  TEST_ASSERT_EQUAL_INT(SHUTTER_POS_UNKNOWN, shutterPosGet(99));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_position_starts_unknown);
  RUN_TEST(test_uncalibrated_channel_settles_at_once);
  RUN_TEST(test_uncalibrated_channel_refuses_position_command);
  RUN_TEST(test_full_travel_down_anchors_at_closed);
  RUN_TEST(test_full_travel_up_anchors_at_open);
  RUN_TEST(test_intermediate_target_refused_while_position_unknown);
  RUN_TEST(test_endstop_target_accepted_while_position_unknown);
  RUN_TEST(test_timed_stop_lands_on_target);
  RUN_TEST(test_retarget_same_direction_does_not_restart_the_motor);
  RUN_TEST(test_direction_change_stops_before_reversing);
  RUN_TEST(test_target_equal_to_current_position_stops_instead_of_reversing);
  RUN_TEST(test_deadline_survives_a_millis_rollover);
  RUN_TEST(test_up_travel_falls_back_to_a_factor_of_down);
  RUN_TEST(test_remote_driven_movement_is_tracked);
  RUN_TEST(test_out_of_range_channel_is_rejected);
  return UNITY_END();
}
