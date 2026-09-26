/*
 * Native unit tests for remote matching and press detection.
 *
 * These two decisions stand between a wall remote and the position tracker.
 * The lookup decides whether a received frame belongs to a configured remote at
 * all; the repeat check decides whether it is a new press or the same button
 * still held. On the live device the lookup could never succeed - the table
 * held the serials exactly as the log prints them, the code compared them with
 * the lowest byte shifted away - so Home Assistant never heard about a single
 * remote press and kept showing wall-remote moves as if they had not happened.
 *
 * Radio decoding, the tracker itself and MQTT are not covered here.
 */

#include <unity.h>

#include <config.h>
#include <remoteMatch.h>

/* T E S T   D O U B L E S ****************************************************/

uint32_t testMillis = 0;
bool testLogEcho = false;
int testPinMode[64] = {0};

s_config config;

// The module under test is compiled straight into this translation unit so the
// test binary does not have to link the rest of the firmware.
#include "../../src/remoteMatch.cpp"

/* H E L P E R S **************************************************************/

static void setRemote(int index, uint32_t serial, bool enabled = true) {
  config.jaro.remote_serial[index] = serial;
  config.jaro.remote_enable[index] = enabled;
}

void setUp(void) {
  memset((void *)&config, 0, sizeof(config));
  remoteRepeatReset();
}

void tearDown(void) {}

/* M A T C H I N G ************************************************************/

// The case from the live device: the log said "serial: 0x001a4a06", the table
// said 1a4a06, and the old comparison (serial >> 8 == entry) could not match.
static void test_the_serial_as_the_log_prints_it_matches() {
  setRemote(6, 0x1a4a06);
  TEST_ASSERT_EQUAL_INT(6, remoteFind(0x001a4a06));
}

// A multi-channel handset with a serial per channel: every channel is its own
// entry, and each has to land on its own row - the prefix form would fold all
// of them onto one.
static void test_each_channel_of_a_handset_finds_its_own_entry() {
  for (int ch = 0; ch < 15; ch++) {
    setRemote(ch, 0x1a4a00u + (uint32_t)ch);
  }
  for (int ch = 0; ch < 15; ch++) {
    TEST_ASSERT_EQUAL_INT(ch, remoteFind(0x1a4a00u + (uint32_t)ch));
  }
}

// Upstream's form keeps working for anyone who configured against it.
static void test_the_upstream_prefix_form_still_matches() {
  setRemote(3, 0x1a4a);
  TEST_ASSERT_EQUAL_INT(3, remoteFind(0x1a4a37));
  TEST_ASSERT_EQUAL_INT(3, remoteFind(0x1a4a00));
}

// An exact entry must win over a prefix entry that happens to come first.
// Otherwise one prefix row would swallow every channel of the handset.
static void test_an_exact_entry_wins_over_an_earlier_prefix_entry() {
  setRemote(0, 0x1a4a);
  setRemote(5, 0x1a4a06);
  TEST_ASSERT_EQUAL_INT(5, remoteFind(0x1a4a06));
  TEST_ASSERT_EQUAL_INT(0, remoteFind(0x1a4a07)); // no exact row for this one
}

static void test_a_disabled_entry_is_ignored() {
  setRemote(6, 0x1a4a06, false);
  TEST_ASSERT_EQUAL_INT(-1, remoteFind(0x1a4a06));
}

static void test_an_unknown_serial_matches_nothing() {
  setRemote(6, 0x1a4a06);
  TEST_ASSERT_EQUAL_INT(-1, remoteFind(0x1a5000));
  TEST_ASSERT_EQUAL_INT(-1, remoteFind(0x0ABCDEF));
}

// A KeeLoq serial is 28 bits, so serial >> 8 never exceeds 20 bits. An entry
// above that - 0x1a4a06 is 21 bits - can only ever match exactly, which is why
// the prefix-only comparison was hopeless for this table.
static void test_an_entry_wider_than_twenty_bits_only_matches_exactly() {
  setRemote(6, 0x1a4a06);
  const uint32_t others[] = {0x0FFFFFFF, 0x1a4a0600 & 0x0FFFFFFF, 0x0a4a0612, 0x001a4a07};
  for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
    TEST_ASSERT_EQUAL_INT(-1, remoteFind(others[i]));
  }
}

/* P R E S S   D E T E C T I O N **********************************************/

static void test_the_first_frame_is_a_new_press() { TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 1000)); }

// What the live device showed for a five second hold: two to three decoded
// frames a second. The tracker must hear about it once.
static void test_a_held_button_is_one_press() {
  int newPresses = 0;
  for (uint32_t t = 0; t <= 5000; t += 400) {
    if (!remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 10000 + t)) {
      newPresses++;
    }
  }
  TEST_ASSERT_EQUAL_INT(1, newPresses);
}

static void test_pressing_again_after_a_pause_is_a_new_press() {
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 1000));
  TEST_ASSERT_TRUE(remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 1400));
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 1400 + REMOTE_REPEAT_GAP_MS));
}

// UP then DOWN in quick succession is two decisions, not one.
static void test_a_different_button_is_a_new_press() {
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 1000));
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_DOWN, 1200));
}

static void test_a_different_remote_is_a_new_press() {
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 1000));
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a07, REMOTE_FN_UP, 1100));
}

/*
 * A long STOP press, as the radio library reports it: STOP frames, then the
 * eleventh reported once as SHADE, then STOP again for as long as the button is
 * held. Exactly two decisions: the STOP, and the SHADE. A third one - a "new"
 * STOP after the SHADE - would settle the tracker at its old position and
 * publish that over the shade position just reported.
 */
static void test_a_long_stop_press_reports_stop_then_shade_and_nothing_after() {
  int reported = 0;
  int8_t lastReported = 0;
  uint32_t t = 5000;
  for (int frame = 1; frame <= 20; frame++, t += 250) {
    int8_t fn = (frame == 11) ? REMOTE_FN_SHADE : REMOTE_FN_STOP;
    if (!remoteIsRepeat(0x1a4a06, fn, t)) {
      reported++;
      lastReported = fn;
    }
  }
  TEST_ASSERT_EQUAL_INT(2, reported);
  TEST_ASSERT_EQUAL_INT(REMOTE_FN_SHADE, lastReported);
}

static void test_a_stop_after_the_shade_press_has_ended_is_new() {
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_SHADE, 1000));
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_STOP, 1000 + REMOTE_REPEAT_GAP_MS + 1));
}

// millis() wraps after 49 days; a press held across the wrap is still one press.
static void test_a_press_held_across_the_millis_wrap_stays_one_press() {
  TEST_ASSERT_FALSE(remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 0xFFFFFF00u));
  TEST_ASSERT_TRUE(remoteIsRepeat(0x1a4a06, REMOTE_FN_UP, 0x00000100u));
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_the_serial_as_the_log_prints_it_matches);
  RUN_TEST(test_each_channel_of_a_handset_finds_its_own_entry);
  RUN_TEST(test_the_upstream_prefix_form_still_matches);
  RUN_TEST(test_an_exact_entry_wins_over_an_earlier_prefix_entry);
  RUN_TEST(test_a_disabled_entry_is_ignored);
  RUN_TEST(test_an_unknown_serial_matches_nothing);
  RUN_TEST(test_an_entry_wider_than_twenty_bits_only_matches_exactly);

  RUN_TEST(test_the_first_frame_is_a_new_press);
  RUN_TEST(test_a_held_button_is_one_press);
  RUN_TEST(test_pressing_again_after_a_pause_is_a_new_press);
  RUN_TEST(test_a_different_button_is_a_new_press);
  RUN_TEST(test_a_different_remote_is_a_new_press);
  RUN_TEST(test_a_long_stop_press_reports_stop_then_shade_and_nothing_after);
  RUN_TEST(test_a_stop_after_the_shade_press_has_ended_is_new);
  RUN_TEST(test_a_press_held_across_the_millis_wrap_stays_one_press);

  return UNITY_END();
}
