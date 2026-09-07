/*
 * Native unit tests for the timer logic.
 *
 * The interesting parts here are arithmetic that is easy to get subtly wrong
 * and impossible to observe by compiling: parsing "HH:MM", clamping an astro
 * event into a min/max window, and the polar cases where the sun does not cross
 * the horizon at all and the library reports that out of band.
 *
 * The astro results are never hard-coded. Each test asks the function what it
 * computed for the given date and place, then asserts how the surrounding logic
 * must behave relative to that - so the tests check this firmware's reasoning
 * rather than re-implementing Dusk2Dawn's.
 */

#include <unity.h>

#include <config.h>
#include <timer.h>

#include "../../lib/Dusk2Dawn/Dusk2Dawn.cpp"
#include "../../src/timer.cpp"

/* T E S T   D O U B L E S ****************************************************/

uint32_t testMillis = 0;
bool testLogEcho = false;

s_config config;

static int groupCmdCount = 0;
static uint16_t lastGroupMask = 0;

void jaroCmd(JaroCmdGrpType type, uint16_t group_mask) {
  (void)type;
  groupCmdCount++;
  lastGroupMask = group_mask;
}

/* H E L P E R S **************************************************************/

// Budapest, where this controller actually runs.
#define LAT 47.4979f
#define LON 19.0402f

// Svalbard - far enough north that midsummer has no sunset and midwinter no
// sunrise, which is the case the "no event" path exists for.
#define POLAR_LAT 78.22f
#define POLAR_LON 15.65f

static time_t localMoment(int year, int mon, int day, int hour, int minute) {
  struct tm t;
  std::memset(&t, 0, sizeof(t));
  t.tm_year = year - 1900;
  t.tm_mon = mon - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_isdst = -1; // let the library work out whether DST applies
  return mktime(&t);
}

static void setTime(s_cfg_timer &t, const char *value) { std::snprintf(t.time_value, sizeof(t.time_value), "%s", value); }
static void setMin(s_cfg_timer &t, const char *value) { std::snprintf(t.min_time_value, sizeof(t.min_time_value), "%s", value); }
static void setMax(s_cfg_timer &t, const char *value) { std::snprintf(t.max_time_value, sizeof(t.max_time_value), "%s", value); }

void setUp() {
  config = s_config{};
  config.geo.latitude = LAT;
  config.geo.longitude = LON;
  groupCmdCount = 0;
  lastGroupMask = 0;

  // The astro maths derives the UTC offset from the configured zone, so the
  // tests have to pin one down rather than inherit the build machine's.
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
}

void tearDown() {}

/* T E S T S ******************************************************************/

static void test_time_parsing_accepts_a_valid_value() {
  TEST_ASSERT_EQUAL_INT(0, timeToMinutes("00:00"));
  TEST_ASSERT_EQUAL_INT(435, timeToMinutes("07:15"));
  TEST_ASSERT_EQUAL_INT(1439, timeToMinutes("23:59"));
}

// getHour()/getMinute() return -1 for anything malformed. They used to be
// assigned to uint8_t locals, where -1 became 255 and a "minHour >= 0" guard was
// always true, so a bad value clamped an event to 255:255.
static void test_time_parsing_rejects_malformed_values() {
  TEST_ASSERT_EQUAL_INT(-1, timeToMinutes(""));
  TEST_ASSERT_EQUAL_INT(-1, timeToMinutes("7:15"));
  TEST_ASSERT_EQUAL_INT(-1, timeToMinutes("07:5"));
  TEST_ASSERT_EQUAL_INT(-1, timeToMinutes("25:00"));
  TEST_ASSERT_EQUAL_INT(-1, timeToMinutes("12:60"));
}

static void test_weekday_flags_map_to_the_right_days() {
  s_cfg_timer t{};
  t.sunday = true;
  t.wednesday = true;

  TEST_ASSERT_TRUE(isDayEnabled(t, 0));  // tm_wday 0 is Sunday
  TEST_ASSERT_FALSE(isDayEnabled(t, 1)); // Monday
  TEST_ASSERT_TRUE(isDayEnabled(t, 3));  // Wednesday
  TEST_ASSERT_FALSE(isDayEnabled(t, 6)); // Saturday
  TEST_ASSERT_FALSE(isDayEnabled(t, 9)); // out of range
}

static void test_fixed_time_fires_on_the_minute_and_only_then() {
  s_cfg_timer t{};
  t.type = TYPE_FIXED_TIME;
  setTime(t, "07:30");
  time_t now = localMoment(2026, 3, 10, 7, 30);

  TEST_ASSERT_TRUE(checkTimerTrigger(t, now, 7, 30));
  TEST_ASSERT_FALSE(checkTimerTrigger(t, now, 7, 29));
  TEST_ASSERT_FALSE(checkTimerTrigger(t, now, 7, 31));
  TEST_ASSERT_FALSE(checkTimerTrigger(t, now, 19, 30));
}

static void test_a_malformed_fixed_time_never_fires() {
  s_cfg_timer t{};
  t.type = TYPE_FIXED_TIME;
  setTime(t, "oops");
  time_t now = localMoment(2026, 3, 10, 7, 30);

  for (int h = 0; h < 24; h++) {
    for (int m = 0; m < 60; m += 7) {
      TEST_ASSERT_FALSE(checkTimerTrigger(t, now, (uint8_t)h, (uint8_t)m));
    }
  }
}

static void test_sunrise_is_plausible_for_the_configured_place() {
  uint8_t h = 0, m = 0;
  time_t midsummer = localMoment(2026, 6, 21, 12, 0);

  TEST_ASSERT_TRUE(getSunriseOrSunset(midsummer, TYPE_SUNRISE, 0, LAT, LON, h, m));
  int minutes = h * 60 + m;
  // Budapest at the solstice: sunrise is a little before 05:00 local. A wide
  // band still catches a wrong time zone, a wrong sign or a missing DST hour.
  TEST_ASSERT_INT_WITHIN_MESSAGE(60, 4 * 60 + 45, minutes, "midsummer sunrise is not where it should be");

  TEST_ASSERT_TRUE(getSunriseOrSunset(midsummer, TYPE_SUNDOWN, 0, LAT, LON, h, m));
  minutes = h * 60 + m;
  TEST_ASSERT_INT_WITHIN_MESSAGE(60, 20 * 60 + 45, minutes, "midsummer sunset is not where it should be");
}

// Winter and summer differ by more than the DST hour, so this catches the
// classic mistake of adding the DST offset twice or not at all.
static void test_summer_and_winter_sunrise_differ_as_expected() {
  uint8_t h = 0, m = 0;
  TEST_ASSERT_TRUE(getSunriseOrSunset(localMoment(2026, 6, 21, 12, 0), TYPE_SUNRISE, 0, LAT, LON, h, m));
  int summer = h * 60 + m;
  TEST_ASSERT_TRUE(getSunriseOrSunset(localMoment(2026, 12, 21, 12, 0), TYPE_SUNRISE, 0, LAT, LON, h, m));
  int winter = h * 60 + m;

  TEST_ASSERT_TRUE_MESSAGE(winter > summer, "winter sunrise should be later than summer sunrise");
  TEST_ASSERT_INT_WITHIN_MESSAGE(60, 165, winter - summer, "the summer/winter difference is wrong");
}

static void test_offset_shifts_the_event() {
  uint8_t h = 0, m = 0;
  time_t now = localMoment(2026, 4, 15, 12, 0);

  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, h, m));
  int base = h * 60 + m;

  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 30, LAT, LON, h, m));
  TEST_ASSERT_EQUAL_INT(base + 30, h * 60 + m);

  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, -45, LAT, LON, h, m));
  TEST_ASSERT_EQUAL_INT(base - 45, h * 60 + m);
}

// Polar day: the sun never sets, so there is no event to fire on. This used to
// come back as 23:59 - the library's -1 sentinel run through the offset and the
// modulo wrap - and the timer went off just before midnight, every day.
static void test_polar_day_reports_no_event() {
  uint8_t h = 0, m = 0;
  time_t midsummer = localMoment(2026, 6, 21, 12, 0);

  TEST_ASSERT_FALSE_MESSAGE(getSunriseOrSunset(midsummer, TYPE_SUNDOWN, 0, POLAR_LAT, POLAR_LON, h, m), "reported a sunset above the arctic circle in June");
  TEST_ASSERT_EQUAL_INT(0, h);
  TEST_ASSERT_EQUAL_INT(0, m);
}

static void test_polar_night_reports_no_event() {
  uint8_t h = 0, m = 0;
  time_t midwinter = localMoment(2026, 12, 21, 12, 0);

  TEST_ASSERT_FALSE_MESSAGE(getSunriseOrSunset(midwinter, TYPE_SUNRISE, 0, POLAR_LAT, POLAR_LON, h, m), "reported a sunrise above the arctic circle in December");
}

static void test_a_timer_does_not_fire_when_there_is_no_event() {
  config.geo.latitude = POLAR_LAT;
  config.geo.longitude = POLAR_LON;

  s_cfg_timer t{};
  t.type = TYPE_SUNDOWN;
  time_t midsummer = localMoment(2026, 6, 21, 12, 0);

  for (int h = 0; h < 24; h++) {
    for (int m = 0; m < 60; m += 11) {
      TEST_ASSERT_FALSE(checkTimerTrigger(t, midsummer, (uint8_t)h, (uint8_t)m));
    }
  }
}

// The clamp used to be applied to the hour and the minute independently, which
// landed on a time that was neither the event nor the limit: sunrise 05:30 with
// a 07:15 minimum fired at 07:30.
static void test_minimum_time_clamps_to_the_limit_exactly() {
  time_t now = localMoment(2026, 6, 21, 12, 0);
  uint8_t eh = 0, em = 0;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, eh, em));
  int event = eh * 60 + em;

  s_cfg_timer t{};
  t.type = TYPE_SUNRISE;
  t.use_min_time = true;
  setMin(t, "07:15"); // later than a midsummer sunrise in Budapest
  TEST_ASSERT_TRUE_MESSAGE(event < 7 * 60 + 15, "test setup: the event is not before the minimum");

  TEST_ASSERT_TRUE_MESSAGE(checkTimerTrigger(t, now, 7, 15), "did not fire at the minimum");
  TEST_ASSERT_FALSE_MESSAGE(checkTimerTrigger(t, now, 7, 30), "fired at the hour of the limit but the minute of the event");
  TEST_ASSERT_FALSE_MESSAGE(checkTimerTrigger(t, now, (uint8_t)(event / 60), (uint8_t)(event % 60)), "fired at the unclamped event time");
}

static void test_maximum_time_clamps_to_the_limit_exactly() {
  time_t now = localMoment(2026, 6, 21, 12, 0);
  uint8_t eh = 0, em = 0;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNDOWN, 0, LAT, LON, eh, em));
  int event = eh * 60 + em;

  s_cfg_timer t{};
  t.type = TYPE_SUNDOWN;
  t.use_max_time = true;
  setMax(t, "20:00"); // earlier than a midsummer sunset in Budapest
  TEST_ASSERT_TRUE_MESSAGE(event > 20 * 60, "test setup: the event is not after the maximum");

  TEST_ASSERT_TRUE_MESSAGE(checkTimerTrigger(t, now, 20, 0), "did not fire at the maximum");
  TEST_ASSERT_FALSE_MESSAGE(checkTimerTrigger(t, now, (uint8_t)(event / 60), (uint8_t)(event % 60)), "fired at the unclamped event time");
}

// A limit that is not a valid time must be ignored, not applied. Before, the
// -1 from the parser became 255 and clamped the event to 255:255.
static void test_a_malformed_limit_is_ignored() {
  time_t now = localMoment(2026, 6, 21, 12, 0);
  uint8_t eh = 0, em = 0;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, eh, em));

  s_cfg_timer t{};
  t.type = TYPE_SUNRISE;
  t.use_min_time = true;
  setMin(t, "");

  TEST_ASSERT_TRUE_MESSAGE(checkTimerTrigger(t, now, eh, em), "an unusable limit suppressed the event");
}

static void test_a_limit_that_does_not_bind_leaves_the_event_alone() {
  time_t now = localMoment(2026, 6, 21, 12, 0);
  uint8_t eh = 0, em = 0;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, eh, em));

  s_cfg_timer t{};
  t.type = TYPE_SUNRISE;
  t.use_min_time = true;
  setMin(t, "01:00"); // long before any sunrise
  t.use_max_time = true;
  setMax(t, "23:00"); // long after

  TEST_ASSERT_TRUE(checkTimerTrigger(t, now, eh, em));
}

static void test_group_command_carries_the_configured_mask() {
  s_cfg_timer t{};
  t.cmd = 1; // DOWN
  t.grp_mask = 0x0A5A;

  executeCommand(t, 0);

  TEST_ASSERT_EQUAL_INT(1, groupCmdCount);
  TEST_ASSERT_EQUAL_HEX16(0x0A5A, lastGroupMask);
}


/* T W I L I G H T   M O D E S ************************************************/

// Far enough north that the sun never gets 18 deg below the horizon at
// midsummer, so astronomical twilight simply does not happen.
#define NORDIC_LAT 55.68f
#define NORDIC_LON 12.57f

static void test_zenith_angles_match_the_standard_definitions() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 90.833f, astroZenith(ASTRO_REAL, 0));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 96.0f, astroZenith(ASTRO_CIVIL, 0));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 102.0f, astroZenith(ASTRO_NAUTICAL, 0));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 108.0f, astroZenith(ASTRO_ASTRONOMICAL, 0));
  // an unknown mode must fall back to the ordinary definition rather than to 0
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 90.833f, astroZenith(99, 0));
}

// An obstruction hides the sun while it is still above the true horizon, which
// is a SMALLER zenith angle. Getting this backwards would move every horizon
// timer the wrong way, and the result would still look plausible.
static void test_an_obstruction_reduces_the_zenith_angle() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 90.833f - 5.0f, astroZenith(ASTRO_HORIZON, 5));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 90.833f + 3.0f, astroZenith(ASTRO_HORIZON, -3));
}

static void test_horizon_value_is_clamped() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 90.833f - ASTRO_HORIZON_MAX, astroZenith(ASTRO_HORIZON, 120));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 90.833f - ASTRO_HORIZON_MIN, astroZenith(ASTRO_HORIZON, -120));
}

// Dusk gets later the further below the horizon the definition reaches.
static void test_dusk_is_progressively_later_for_darker_definitions() {
  time_t now = localMoment(2026, 3, 20, 12, 0); // equinox: all four exist
  uint8_t h = 0, m = 0;
  int official = 0, civil = 0, nautical = 0, astronomical = 0;

  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNDOWN, 0, LAT, LON, h, m, ASTRO_REAL, 0));
  official = h * 60 + m;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNDOWN, 0, LAT, LON, h, m, ASTRO_CIVIL, 0));
  civil = h * 60 + m;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNDOWN, 0, LAT, LON, h, m, ASTRO_NAUTICAL, 0));
  nautical = h * 60 + m;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNDOWN, 0, LAT, LON, h, m, ASTRO_ASTRONOMICAL, 0));
  astronomical = h * 60 + m;

  TEST_ASSERT_TRUE_MESSAGE(civil > official, "civil dusk should be after sunset");
  TEST_ASSERT_TRUE_MESSAGE(nautical > civil, "nautical dusk should be after civil");
  TEST_ASSERT_TRUE_MESSAGE(astronomical > nautical, "astronomical dusk should be after nautical");
  // roughly half an hour per step at this latitude - a sanity bound on the maths
  TEST_ASSERT_INT_WITHIN_MESSAGE(20, 30, civil - official, "civil dusk is an implausible distance from sunset");
}

static void test_dawn_is_progressively_earlier_for_darker_definitions() {
  time_t now = localMoment(2026, 3, 20, 12, 0);
  uint8_t h = 0, m = 0;

  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, h, m, ASTRO_REAL, 0));
  int official = h * 60 + m;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, h, m, ASTRO_CIVIL, 0));
  int civil = h * 60 + m;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, h, m, ASTRO_ASTRONOMICAL, 0));
  int astronomical = h * 60 + m;

  TEST_ASSERT_TRUE_MESSAGE(civil < official, "civil dawn should be before sunrise");
  TEST_ASSERT_TRUE_MESSAGE(astronomical < civil, "astronomical dawn should be before civil");
}

static void test_a_hill_to_the_west_brings_sunset_forward() {
  time_t now = localMoment(2026, 3, 20, 12, 0);
  uint8_t h = 0, m = 0;

  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNDOWN, 0, LAT, LON, h, m, ASTRO_REAL, 0));
  int flat = h * 60 + m;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNDOWN, 0, LAT, LON, h, m, ASTRO_HORIZON, 8));
  int blocked = h * 60 + m;

  TEST_ASSERT_TRUE_MESSAGE(blocked < flat, "an obstruction should make the sun disappear earlier");

  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, h, m, ASTRO_REAL, 0));
  flat = h * 60 + m;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNRISE, 0, LAT, LON, h, m, ASTRO_HORIZON, 8));
  blocked = h * 60 + m;

  TEST_ASSERT_TRUE_MESSAGE(blocked > flat, "an obstruction should make the sun appear later");
}

// The darker the definition, the more often it is never reached. This is the
// case that makes twilight modes different from an offset: the event genuinely
// does not exist on some days, and the timer has to not fire rather than
// substitute something.
static void test_astronomical_twilight_does_not_occur_at_midsummer_up_north() {
  time_t midsummer = localMoment(2026, 6, 21, 12, 0);
  uint8_t h = 0, m = 0;

  TEST_ASSERT_TRUE_MESSAGE(getSunriseOrSunset(midsummer, TYPE_SUNDOWN, 0, NORDIC_LAT, NORDIC_LON, h, m, ASTRO_REAL, 0),
                           "there is still an ordinary sunset at this latitude");
  TEST_ASSERT_FALSE_MESSAGE(getSunriseOrSunset(midsummer, TYPE_SUNDOWN, 0, NORDIC_LAT, NORDIC_LON, h, m, ASTRO_ASTRONOMICAL, 0),
                            "reported an astronomical dusk on a night that never gets that dark");
}

static void test_a_timer_does_not_fire_when_its_twilight_never_arrives() {
  config.geo.latitude = NORDIC_LAT;
  config.geo.longitude = NORDIC_LON;

  s_cfg_timer t{};
  t.type = TYPE_SUNDOWN;
  t.astro_mode = ASTRO_ASTRONOMICAL;
  time_t midsummer = localMoment(2026, 6, 21, 12, 0);

  for (int h = 0; h < 24; h++) {
    for (int m = 0; m < 60; m += 11) {
      TEST_ASSERT_FALSE(checkTimerTrigger(t, midsummer, (uint8_t)h, (uint8_t)m));
    }
  }
}

// A V3 config has neither key, so both read as 0 - and 0 has to mean exactly
// what the firmware did before twilight modes existed.
static void test_the_default_mode_matches_the_previous_behaviour() {
  time_t now = localMoment(2026, 3, 20, 12, 0);
  uint8_t h = 0, m = 0;
  TEST_ASSERT_TRUE(getSunriseOrSunset(now, TYPE_SUNDOWN, 0, LAT, LON, h, m, ASTRO_REAL, 0));
  int explicitReal = h * 60 + m;

  // the same call through a default-constructed timer, as a migrated config
  // would produce
  s_cfg_timer t{};
  t.type = TYPE_SUNDOWN;
  TEST_ASSERT_TRUE(checkTimerTrigger(t, now, (uint8_t)(explicitReal / 60), (uint8_t)(explicitReal % 60)));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_time_parsing_accepts_a_valid_value);
  RUN_TEST(test_time_parsing_rejects_malformed_values);
  RUN_TEST(test_weekday_flags_map_to_the_right_days);
  RUN_TEST(test_fixed_time_fires_on_the_minute_and_only_then);
  RUN_TEST(test_a_malformed_fixed_time_never_fires);
  RUN_TEST(test_sunrise_is_plausible_for_the_configured_place);
  RUN_TEST(test_summer_and_winter_sunrise_differ_as_expected);
  RUN_TEST(test_offset_shifts_the_event);
  RUN_TEST(test_polar_day_reports_no_event);
  RUN_TEST(test_polar_night_reports_no_event);
  RUN_TEST(test_a_timer_does_not_fire_when_there_is_no_event);
  RUN_TEST(test_minimum_time_clamps_to_the_limit_exactly);
  RUN_TEST(test_maximum_time_clamps_to_the_limit_exactly);
  RUN_TEST(test_a_malformed_limit_is_ignored);
  RUN_TEST(test_a_limit_that_does_not_bind_leaves_the_event_alone);
  RUN_TEST(test_group_command_carries_the_configured_mask);

  RUN_TEST(test_zenith_angles_match_the_standard_definitions);
  RUN_TEST(test_an_obstruction_reduces_the_zenith_angle);
  RUN_TEST(test_horizon_value_is_clamped);
  RUN_TEST(test_dusk_is_progressively_later_for_darker_definitions);
  RUN_TEST(test_dawn_is_progressively_earlier_for_darker_definitions);
  RUN_TEST(test_a_hill_to_the_west_brings_sunset_forward);
  RUN_TEST(test_astronomical_twilight_does_not_occur_at_midsummer_up_north);
  RUN_TEST(test_a_timer_does_not_fire_when_its_twilight_never_arrives);
  RUN_TEST(test_the_default_mode_matches_the_previous_behaviour);
  return UNITY_END();
}
