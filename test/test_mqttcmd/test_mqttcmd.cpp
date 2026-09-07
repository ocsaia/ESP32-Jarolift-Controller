/*
 * Native unit tests for the MQTT command dispatch.
 *
 * mqttHandleCommand() is the surface Home Assistant actually drives, and it is
 * string parsing all the way down: a base topic the user chose, a suffix, a
 * channel number pulled out with strtol, and a payload that is a word in one
 * place and a percentage in another. Nothing here fails to compile when it is
 * wrong - it just sends the wrong shutter somewhere, or silently answers
 * "unknown topic" to a command that used to work.
 *
 * It also carries an invariant that is one careless edit from breaking, and its
 * own comment says so: addTopic() returns the same static buffer every call, so
 * shutterTopic and groupTopic alias, and the code is only correct because the
 * first is consumed before the second is built. The tests below fail if those
 * lines are reordered - see the aliasing section.
 *
 * What is NOT covered here: the broker, the AsyncTCP task the real callback
 * runs on, and the queue handoff between them. Those need hardware and a
 * broker; see IMPROVEMENT-PLAN.md for what is still unverified.
 */

#include <unity.h>

#include <LittleFS.h>
#include <basics.h>
#include <jarolift.h>
#include <shutterPos.h>

/* T E S T   D O U B L E S ****************************************************/

FakeLittleFS LittleFS;
uint32_t testMillis = 0;
bool testLogEcho = false;
int testPinMode[64] = {0};
s_logdata logData;
s_wifi wifi;
s_eth eth;
FakeEsp ESP;

s_config config;
bool setupMode = false;

// onMqttMessage() runs on the AsyncTCP task and only hands work to the queue.
// It is not what these tests exercise, but it has to link.
bool cmdQueuePushMqtt(const char *topic, const char *payload, size_t len) {
  (void)topic;
  (void)payload;
  (void)len;
  return true;
}
void cmdQueueCountDrop(const char *reason) { (void)reason; }

void setLogLevel(uint8_t level) { (void)level; }
void sendWiFiInfo() {}

// what the dispatch decided to do, in order
enum CallKind { CALL_SINGLE, CALL_GROUP, CALL_SERVICE, CALL_SETPOS, CALL_CALIB_START, CALL_CALIB_FINISH, CALL_CALIB_ABORT, CALL_DISCOVERY };

struct s_call {
  CallKind kind;
  int a; // command type, or channel for the position and calibration calls
  int b; // channel, group mask, percentage, or the direction flag
};

static s_call calls[16];
static int callCount = 0;

static void record(CallKind kind, int a, int b) {
  if (callCount < (int)(sizeof(calls) / sizeof(calls[0]))) {
    calls[callCount].kind = kind;
    calls[callCount].a = a;
    calls[callCount].b = b;
  }
  callCount++;
}

void jaroCmd(JaroCmdType type, uint8_t channel) { record(CALL_SINGLE, (int)type, channel); }
void jaroCmd(JaroCmdGrpType type, uint16_t group_mask) { record(CALL_GROUP, (int)type, group_mask); }
void jaroCmd(JaroCmdSrvType type, uint8_t channel) { record(CALL_SERVICE, (int)type, channel); }
void mqttDiscoverySetup(bool reset) { record(CALL_DISCOVERY, reset ? 1 : 0, 0); }

// return values the tests steer, so the failure branches can be reached
static bool setTargetResult = true;
static bool calibStartResult = true;
static uint32_t calibFinishResult = 12345;

bool shutterPosSetTarget(uint8_t channel, uint8_t targetPct) {
  record(CALL_SETPOS, channel, targetPct);
  return setTargetResult;
}
bool shutterCalibStart(uint8_t channel, bool downwards) {
  record(CALL_CALIB_START, channel, downwards ? 1 : 0);
  return calibStartResult;
}
uint32_t shutterCalibFinish(uint8_t channel) {
  record(CALL_CALIB_FINISH, channel, 0);
  return calibFinishResult;
}
void shutterCalibAbort(uint8_t channel) { record(CALL_CALIB_ABORT, channel, 0); }

// The module under test is compiled straight into this translation unit so the
// test binary does not have to link the rest of the firmware.
#include "../../lib/muTimer/src/muTimer.cpp"

#include "../../src/mqtt.cpp"

/* H E L P E R S **************************************************************/

#define BASE "test/jaro"

static void send(const char *topic, const char *payload) { mqttHandleCommand(topic, payload); }

// topic under the configured base, as Home Assistant would address it
static const char *t(const char *suffix) {
  static char buf[256];
  snprintf(buf, sizeof(buf), "%s%s", BASE, suffix);
  return buf;
}

static void assertOneCall(CallKind kind, int a, int b, const char *what) {
  char msg[160];
  snprintf(msg, sizeof(msg), "%s: expected exactly one call, got %d", what, callCount);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, callCount, msg);
  snprintf(msg, sizeof(msg), "%s: wrong kind of call", what);
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)kind, (int)calls[0].kind, msg);
  snprintf(msg, sizeof(msg), "%s: wrong command/channel", what);
  TEST_ASSERT_EQUAL_INT_MESSAGE(a, calls[0].a, msg);
  snprintf(msg, sizeof(msg), "%s: wrong channel/mask/value", what);
  TEST_ASSERT_EQUAL_INT_MESSAGE(b, calls[0].b, msg);
}

static void assertNoCall(const char *what) {
  char msg[160];
  snprintf(msg, sizeof(msg), "%s: expected no command to be issued, got %d", what, callCount);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, callCount, msg);
}

// the last thing published on <base>/message, or "" if nothing was
static std::string lastMessage() {
  std::string want = std::string(BASE) + "/message";
  for (int i = (int)fakeMqttPublished().size() - 1; i >= 0; i--) {
    if (fakeMqttPublished()[i].topic == want) {
      return fakeMqttPublished()[i].payload;
    }
  }
  return "";
}

void setUp(void) {
  memset((void *)&config, 0, sizeof(config));
  snprintf(config.mqtt.topic, sizeof(config.mqtt.topic), "%s", BASE);
  config.mqtt.enable = true;
  config.mqtt.ha_enable = true;
  for (int i = 0; i < 6; i++) {
    config.jaro.grp_mask[i] = (uint16_t)(0x0011u << i);
  }
  callCount = 0;
  setTargetResult = true;
  calibStartResult = true;
  calibFinishResult = 12345;
  fakeMqttReset();
  ESP.restarts = 0;
}

void tearDown(void) {}

/* S H U T T E R   C O M M A N D S ********************************************/

static void test_every_shutter_payload_maps_to_its_command() {
  struct {
    const char *payload;
    JaroCmdType expected;
  } cases[] = {
      {"UP", CMD_UP},       {"OPEN", CMD_UP},         {"0", CMD_UP},        {"DOWN", CMD_DOWN}, {"CLOSE", CMD_DOWN},
      {"1", CMD_DOWN},      {"STOP", CMD_STOP},       {"2", CMD_STOP},      {"SHADE", CMD_SHADE}, {"3", CMD_SHADE},
      {"SETSHADE", CMD_SET_SHADE}, {"4", CMD_SET_SHADE},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    setUp();
    send(t("/cmd/shutter/3"), cases[i].payload);
    assertOneCall(CALL_SINGLE, (int)cases[i].expected, 2, cases[i].payload);
  }
}

// Home Assistant sends "close"; a person typing by hand sends "CLOSE". Only the
// words are case-insensitive - the numeric forms go through strcmp.
static void test_payload_words_are_case_insensitive() {
  send(t("/cmd/shutter/1"), "close");
  assertOneCall(CALL_SINGLE, CMD_DOWN, 0, "lowercase close");

  setUp();
  send(t("/cmd/shutter/1"), "sToP");
  assertOneCall(CALL_SINGLE, CMD_STOP, 0, "mixed case stop");
}

// One-based on the wire, zero-based in the firmware. Getting this wrong moves
// the shutter next door, which is the sort of bug that gets blamed on the radio.
static void test_the_channel_number_is_one_based_on_the_wire() {
  send(t("/cmd/shutter/1"), "UP");
  assertOneCall(CALL_SINGLE, CMD_UP, 0, "channel 1");

  setUp();
  send(t("/cmd/shutter/16"), "UP");
  assertOneCall(CALL_SINGLE, CMD_UP, 15, "channel 16");
}

/*
 * A channel outside 1..16 issues nothing, and answers "unknown topic".
 *
 * Not "invalid channel", which is what the source looks like it should say:
 * checkJaroCmd() already range-checks and returns -1, so mqttHandleCommand()'s
 * own `if (channel >= 1 && channel <= 16)` is always true and its else branch
 * is unreachable. The message is less helpful than the code implies, and this
 * test says which of the two is real. Making that branch reachable would be a
 * behaviour change, not a test fix.
 */
static void test_a_channel_outside_the_range_is_refused() {
  send(t("/cmd/shutter/0"), "UP");
  assertNoCall("channel 0");
  TEST_ASSERT_EQUAL_STRING("unknown topic", lastMessage().c_str());

  setUp();
  send(t("/cmd/shutter/17"), "UP");
  assertNoCall("channel 17");
  TEST_ASSERT_EQUAL_STRING("unknown topic", lastMessage().c_str());
}

static void test_an_unknown_shutter_payload_is_reported_not_ignored() {
  send(t("/cmd/shutter/3"), "SIDEWAYS");
  assertNoCall("nonsense payload");
  TEST_ASSERT_EQUAL_STRING("invalid shutter cmd", lastMessage().c_str());
}

/* T H E   A L I A S I N G   I N V A R I A N T ********************************/

/*
 * addTopic() hands back the same static buffer on every call, so the shutter
 * prefix is destroyed the moment the group prefix is built. The dispatch is
 * only correct because checkJaroCmd() consumes the first before the second
 * exists, and a comment in the source says "do not reorder these four lines".
 *
 * These two tests are what makes that comment enforceable. Hoisting both
 * addTopic() calls above both checkJaroCmd() calls - the natural tidy-up -
 * leaves the shutter comparison looking at "/cmd/group/", so a shutter topic
 * stops matching and falls through to "unknown topic".
 */
static void test_a_shutter_topic_is_not_confused_with_a_group_topic() {
  send(t("/cmd/shutter/4"), "DOWN");
  assertOneCall(CALL_SINGLE, CMD_DOWN, 3, "shutter topic after the group prefix is built");
}

static void test_a_group_topic_still_resolves_after_the_shutter_prefix() {
  send(t("/cmd/group/2"), "UP");
  assertOneCall(CALL_GROUP, CMD_GRP_UP, config.jaro.grp_mask[1], "group topic");
}

/* G R O U P   C O M M A N D S ************************************************/

static void test_a_numbered_group_sends_its_configured_mask() {
  send(t("/cmd/group/1"), "DOWN");
  assertOneCall(CALL_GROUP, CMD_GRP_DOWN, config.jaro.grp_mask[0], "group 1");

  setUp();
  send(t("/cmd/group/6"), "STOP");
  assertOneCall(CALL_GROUP, CMD_GRP_STOP, config.jaro.grp_mask[5], "group 6");
}

// Same unreachable-else story as the shutter channel above: checkJaroCmd()
// filters the range, so "invalid group" is never sent.
static void test_a_group_outside_the_range_is_refused() {
  send(t("/cmd/group/7"), "UP");
  assertNoCall("group 7");
  TEST_ASSERT_EQUAL_STRING("unknown topic", lastMessage().c_str());

  setUp();
  send(t("/cmd/group/0"), "UP");
  assertNoCall("group 0");
}

// The other way to address a group: name the channels directly in the payload.
static void test_the_bitmask_topics_take_the_mask_from_the_payload() {
  send(t("/cmd/group/up"), "5");
  assertOneCall(CALL_GROUP, CMD_GRP_UP, 5, "bitmask up");

  setUp();
  send(t("/cmd/group/shade"), "65535");
  assertOneCall(CALL_GROUP, CMD_GRP_SHADE, 65535, "bitmask shade, all channels");
}

/* S E T   P O S I T I O N ****************************************************/

static void test_a_position_command_passes_the_percentage_through() {
  send(t("/cmd/shutter/7/set_position"), "45");
  assertOneCall(CALL_SETPOS, 6, 45, "set_position 45");
}

static void test_both_ends_of_the_percentage_range_are_accepted() {
  send(t("/cmd/shutter/2/set_position"), "0");
  assertOneCall(CALL_SETPOS, 1, 0, "set_position 0");

  setUp();
  send(t("/cmd/shutter/2/set_position"), "100");
  assertOneCall(CALL_SETPOS, 1, 100, "set_position 100");
}

static void test_a_percentage_outside_the_range_is_refused() {
  send(t("/cmd/shutter/2/set_position"), "101");
  assertNoCall("101 percent");
  TEST_ASSERT_EQUAL_STRING("invalid position", lastMessage().c_str());

  setUp();
  send(t("/cmd/shutter/2/set_position"), "-1");
  assertNoCall("negative percent");
}

/*
 * strtol stops at the first character it cannot use and reports success for
 * what it did read, so "45abc" would arrive as 45 unless the whole string is
 * checked. A thermostat card sending "45.0" must not be read as 45 either -
 * silently accepting half a payload is how a position ends up somewhere nobody
 * asked for.
 */
static void test_a_payload_that_is_not_purely_numeric_is_refused() {
  const char *bad[] = {"45abc", "45.0", "", "abc", "45 "};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    setUp();
    send(t("/cmd/shutter/2/set_position"), bad[i]);
    assertNoCall(bad[i]);
    TEST_ASSERT_EQUAL_STRING("invalid position", lastMessage().c_str());
  }
}

/*
 * Leading whitespace is accepted, trailing whitespace is not.
 *
 * strtol() skips leading blanks itself, so " 45" arrives as 45 and passes the
 * `*posEnd != 0` check; "45 " leaves posEnd on the space and is rejected. The
 * asymmetry is inherited from strtol rather than intended, and it is harmless -
 * but it is behaviour, so it is pinned here rather than left to be rediscovered
 * by whoever next tightens the parser.
 */
static void test_leading_whitespace_in_a_position_is_tolerated() {
  send(t("/cmd/shutter/2/set_position"), " 45");
  assertOneCall(CALL_SETPOS, 1, 45, "leading space");

  setUp();
  send(t("/cmd/shutter/2/set_position"), "45 ");
  assertNoCall("trailing space");
  TEST_ASSERT_EQUAL_STRING("invalid position", lastMessage().c_str());
}

static void test_a_position_for_an_impossible_channel_is_refused() {
  send(t("/cmd/shutter/17/set_position"), "50");
  assertNoCall("channel 17 position");
  TEST_ASSERT_EQUAL_STRING("invalid channel", lastMessage().c_str());
}

// An uncalibrated channel cannot be positioned, and the user has to be told
// rather than left watching a shutter that never moves.
static void test_a_refused_target_is_reported_back() {
  setTargetResult = false;
  send(t("/cmd/shutter/3/set_position"), "50");
  TEST_ASSERT_EQUAL_STRING("position not available", lastMessage().c_str());
}

/* C A L I B R A T I O N ******************************************************/

static void test_calibration_starts_in_the_requested_direction() {
  send(t("/cmd/shutter/5/calibrate"), "down");
  assertOneCall(CALL_CALIB_START, 4, 1, "calibrate down");

  setUp();
  send(t("/cmd/shutter/5/calibrate"), "UP");
  assertOneCall(CALL_CALIB_START, 4, 0, "calibrate up, uppercase");
}

static void test_finish_reports_the_measured_time() {
  calibFinishResult = 26356;
  send(t("/cmd/shutter/7/calibrate"), "finish");
  assertOneCall(CALL_CALIB_FINISH, 6, 0, "calibrate finish");
  TEST_ASSERT_EQUAL_STRING("channel 7 travel: 26356 ms", lastMessage().c_str());
}

// A measurement outside the accepted window returns 0, and saying so matters:
// otherwise a discarded run looks exactly like a stored one.
static void test_a_discarded_measurement_says_so() {
  calibFinishResult = 0;
  send(t("/cmd/shutter/7/calibrate"), "finish");
  TEST_ASSERT_EQUAL_STRING("channel 7 calibration discarded", lastMessage().c_str());
}

static void test_abort_is_dispatched() {
  send(t("/cmd/shutter/2/calibrate"), "abort");
  assertOneCall(CALL_CALIB_ABORT, 1, 0, "calibrate abort");
}

static void test_an_unknown_calibration_payload_lists_the_options() {
  send(t("/cmd/shutter/2/calibrate"), "sideways");
  assertNoCall("nonsense calibration payload");
  TEST_ASSERT_EQUAL_STRING("use down, up, finish or abort", lastMessage().c_str());
}

static void test_calibrating_an_impossible_channel_is_refused() {
  send(t("/cmd/shutter/0/calibrate"), "down");
  assertNoCall("calibrate channel 0");
  TEST_ASSERT_EQUAL_STRING("invalid channel", lastMessage().c_str());
}

/* D E V I C E   C O M M A N D S **********************************************/

static void test_restart_is_honoured_and_its_reason_recorded() {
  send(t("/cmd/restart"), "");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, ESP.restarts, "the restart command did not restart");
  TEST_ASSERT_EQUAL_STRING("mqtt command", EspSysUtil::RestartReason::lastSaved().c_str());
}

// Reset then re-send: a stale discovery config has to be cleared before the
// current one is published, or Home Assistant keeps both.
static void test_reconfigure_clears_the_discovery_before_resending_it() {
  send(t("/cmd/reconfigure"), "");
  TEST_ASSERT_EQUAL_INT(2, callCount);
  TEST_ASSERT_EQUAL_INT(CALL_DISCOVERY, (int)calls[0].kind);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, calls[0].a, "the first call should be the reset");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, calls[1].a, "the second call should re-publish");
}

// Home Assistant announces itself after a restart; that is when discovery has
// to be re-sent, because a restart clears what it knew.
static void test_home_assistant_coming_online_triggers_discovery() {
  send("homeassistant/status", "online");
  assertOneCall(CALL_DISCOVERY, 0, 0, "ha online");
}

static void test_home_assistant_going_offline_does_nothing() {
  send("homeassistant/status", "offline");
  assertNoCall("ha offline");
}

static void test_nothing_is_sent_when_home_assistant_support_is_off() {
  config.mqtt.ha_enable = false;
  send("homeassistant/status", "online");
  assertNoCall("ha online with discovery disabled");
}

/* U N K N O W N   T O P I C S ************************************************/

static void test_an_unknown_topic_is_answered_rather_than_dropped() {
  send(t("/cmd/nonsense"), "UP");
  assertNoCall("unknown topic");
  TEST_ASSERT_EQUAL_STRING("unknown topic", lastMessage().c_str());
}

// A topic under somebody else's base must not be acted on. The controller only
// subscribes to its own tree, but a wildcard subscription or a shared broker
// makes this reachable.
static void test_a_topic_under_a_different_base_is_not_acted_on() {
  send("someone/else/cmd/shutter/3", "UP");
  assertNoCall("foreign base topic");
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_every_shutter_payload_maps_to_its_command);
  RUN_TEST(test_payload_words_are_case_insensitive);
  RUN_TEST(test_the_channel_number_is_one_based_on_the_wire);
  RUN_TEST(test_a_channel_outside_the_range_is_refused);
  RUN_TEST(test_an_unknown_shutter_payload_is_reported_not_ignored);

  RUN_TEST(test_a_shutter_topic_is_not_confused_with_a_group_topic);
  RUN_TEST(test_a_group_topic_still_resolves_after_the_shutter_prefix);

  RUN_TEST(test_a_numbered_group_sends_its_configured_mask);
  RUN_TEST(test_a_group_outside_the_range_is_refused);
  RUN_TEST(test_the_bitmask_topics_take_the_mask_from_the_payload);

  RUN_TEST(test_a_position_command_passes_the_percentage_through);
  RUN_TEST(test_both_ends_of_the_percentage_range_are_accepted);
  RUN_TEST(test_a_percentage_outside_the_range_is_refused);
  RUN_TEST(test_a_payload_that_is_not_purely_numeric_is_refused);
  RUN_TEST(test_leading_whitespace_in_a_position_is_tolerated);
  RUN_TEST(test_a_position_for_an_impossible_channel_is_refused);
  RUN_TEST(test_a_refused_target_is_reported_back);

  RUN_TEST(test_calibration_starts_in_the_requested_direction);
  RUN_TEST(test_finish_reports_the_measured_time);
  RUN_TEST(test_a_discarded_measurement_says_so);
  RUN_TEST(test_abort_is_dispatched);
  RUN_TEST(test_an_unknown_calibration_payload_lists_the_options);
  RUN_TEST(test_calibrating_an_impossible_channel_is_refused);

  RUN_TEST(test_restart_is_honoured_and_its_reason_recorded);
  RUN_TEST(test_reconfigure_clears_the_discovery_before_resending_it);
  RUN_TEST(test_home_assistant_coming_online_triggers_discovery);
  RUN_TEST(test_home_assistant_going_offline_does_nothing);
  RUN_TEST(test_nothing_is_sent_when_home_assistant_support_is_off);

  RUN_TEST(test_an_unknown_topic_is_answered_rather_than_dropped);
  RUN_TEST(test_a_topic_under_a_different_base_is_not_acted_on);

  return UNITY_END();
}
