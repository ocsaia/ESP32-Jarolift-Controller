/*
 * Native unit tests for configuration persistence.
 *
 * configSaveToFile() and configLoadFromFile() are four hundred lines that have
 * to agree with each other field for field - about two hundred of them, spread
 * over sixteen channels, twenty-four timers and nine subsystems. A field
 * written under one key and read back under another compiles perfectly, passes
 * review easily, and shows up months later as "my calibration disappeared after
 * a reboot". Nothing in the firmware checks the two halves against each other.
 *
 * The other half of the risk is upgrading. configInitValue() runs only when the
 * file cannot be parsed at all, so a device coming from older firmware never
 * sees a default: every field added since is read from a key that is not in the
 * file. That is fine exactly as long as the zero value means something sensible
 * for each one, which is a property nothing has ever checked.
 *
 * What is NOT covered here: LittleFS itself, flash wear, power loss during a
 * write, and the real AES password round trip - see test/shim for what stands
 * in for those and why.
 */

#include <unity.h>

#include <LittleFS.h>
#include <config.h>

/* T E S T   D O U B L E S ****************************************************/

FakeLittleFS LittleFS;
uint32_t testMillis = 0;
bool testLogEcho = false;
int testPinMode[64] = {0};

s_logdata logData;
void setLogLevel(uint8_t level) { (void)level; }

// muTimer is vendored in lib/ and config.cpp uses it for change detection. It is
// not header-only, so it is compiled in here rather than linked - the suite stays
// a single translation unit.
#include "../../lib/muTimer/src/muTimer.cpp"

// The module under test is compiled straight into this translation unit so the
// test binary does not have to link the rest of the firmware.
#include "../../src/config.cpp"

/* H E L P E R S **************************************************************/

/*
 * Wipe the config the same way the firmware does.
 *
 * s_config carries in-class initialisers, so it is not trivially copyable and
 * plain memset draws -Wclass-memaccess. The cast is how configInitValue()
 * already does it, and the zeroing matters: the round-trip comparison below is
 * a byte compare, so both sides have to start from the same padding.
 */
static void zeroConfig() { memset((void *)&config, 0, sizeof(config)); }

/*
 * Put a distinct, non-zero value in every field.
 *
 * Distinct matters: if two fields hold the same value, a save/load pair that
 * crosses them over still compares equal, and the test passes on a real bug.
 */
static void fillConfig() {
  zeroConfig();

  config.version = CFG_VERSION;
  config.lang = 1;

  config.wifi.enable = true;
  snprintf(config.wifi.ssid, sizeof(config.wifi.ssid), "wifi-ssid-value");
  snprintf(config.wifi.password, sizeof(config.wifi.password), "wifi-password-value");
  snprintf(config.wifi.hostname, sizeof(config.wifi.hostname), "wifi-hostname");
  config.wifi.static_ip = true;
  snprintf(config.wifi.ipaddress, sizeof(config.wifi.ipaddress), "10.10.132.64");
  snprintf(config.wifi.subnet, sizeof(config.wifi.subnet), "255.255.255.0");
  snprintf(config.wifi.gateway, sizeof(config.wifi.gateway), "10.10.132.1");
  snprintf(config.wifi.dns, sizeof(config.wifi.dns), "10.10.132.2");

  config.eth.enable = true;
  snprintf(config.eth.hostname, sizeof(config.eth.hostname), "eth-hostname");
  config.eth.static_ip = true;
  snprintf(config.eth.ipaddress, sizeof(config.eth.ipaddress), "10.10.133.64");
  snprintf(config.eth.subnet, sizeof(config.eth.subnet), "255.255.254.0");
  snprintf(config.eth.gateway, sizeof(config.eth.gateway), "10.10.133.1");
  snprintf(config.eth.dns, sizeof(config.eth.dns), "10.10.133.2");
  config.eth.gpio_sck = 11;
  config.eth.gpio_mosi = 12;
  config.eth.gpio_miso = 13;
  config.eth.gpio_cs = 14;
  config.eth.gpio_irq = 15;
  config.eth.gpio_rst = 16;

  config.mqtt.enable = true;
  snprintf(config.mqtt.server, sizeof(config.mqtt.server), "mqtt-server");
  snprintf(config.mqtt.user, sizeof(config.mqtt.user), "mqtt-user");
  snprintf(config.mqtt.password, sizeof(config.mqtt.password), "mqtt-password-value");
  snprintf(config.mqtt.topic, sizeof(config.mqtt.topic), "jarolift/base");
  config.mqtt.port = 8883;
  config.mqtt.ha_enable = true;
  snprintf(config.mqtt.ha_topic, sizeof(config.mqtt.ha_topic), "homeassistant");
  snprintf(config.mqtt.ha_device, sizeof(config.mqtt.ha_device), "ha-device");

  config.ntp.enable = true;
  snprintf(config.ntp.server, sizeof(config.ntp.server), "hu.pool.ntp.org");
  snprintf(config.ntp.tz, sizeof(config.ntp.tz), "CET-1CEST,M3.5.0,M10.5.0/3");

  config.gpio.led_setup = 21;
  config.gpio.gdo0 = 22;
  config.gpio.gdo2 = 23;
  config.gpio.sck = 24;
  config.gpio.mosi = 25;
  config.gpio.miso = 26;
  config.gpio.cs = 27;

  config.auth.enable = true;
  snprintf(config.auth.user, sizeof(config.auth.user), "auth-user");
  snprintf(config.auth.password, sizeof(config.auth.password), "auth-password-value");

  config.log.enable = true;
  config.log.level = 4;
  config.log.order = 1;

  config.jaro.masterMSB = 0x12345678UL;
  config.jaro.masterLSB = 0x9ABCDEF0UL;
  config.jaro.learn_mode = true;
  config.jaro.serial = 0x001A4A00;
  for (int i = 0; i < 16; i++) {
    snprintf(config.jaro.ch_name[i], sizeof(config.jaro.ch_name[i]), "channel-%d", i);
    config.jaro.ch_enable[i] = (i % 2) == 0;
    config.jaro.ch_travel_down[i] = 20000u + (uint32_t)i;
    config.jaro.ch_travel_up[i] = 30000u + (uint32_t)i;
    snprintf(config.jaro.remote_name[i], sizeof(config.jaro.remote_name[i]), "remote-%d", i);
    config.jaro.remote_serial[i] = 0x00A00000u + (uint32_t)i;
    config.jaro.remote_enable[i] = (i % 3) == 0;
    config.jaro.remote_mask[i] = (uint16_t)(0x0101u + i);
  }
  for (int i = 0; i < 6; i++) {
    snprintf(config.jaro.grp_name[i], sizeof(config.jaro.grp_name[i]), "group-%d", i);
    config.jaro.grp_enable[i] = (i % 2) == 1;
    config.jaro.grp_mask[i] = (uint16_t)(0x000Fu + i);
  }

  for (int i = 0; i < TIMER_COUNT; i++) {
    config.timer[i].enable = (i % 2) == 0;
    config.timer[i].type = (uint8_t)(i % 4);
    snprintf(config.timer[i].time_value, sizeof(config.timer[i].time_value), "%02d:%02d", i % 24, i % 60);
    config.timer[i].offset_value = (int16_t)(i - 12);
    config.timer[i].cmd = (uint8_t)(i % 3);
    config.timer[i].monday = (i % 2) == 0;
    config.timer[i].tuesday = (i % 3) == 0;
    config.timer[i].wednesday = (i % 4) == 0;
    config.timer[i].thursday = (i % 5) == 0;
    config.timer[i].friday = (i % 6) == 0;
    config.timer[i].saturday = (i % 7) == 0;
    config.timer[i].sunday = (i % 8) == 0;
    config.timer[i].grp_mask = (uint16_t)(0x1000u + i);
    config.timer[i].use_min_time = (i % 2) == 1;
    snprintf(config.timer[i].min_time_value, sizeof(config.timer[i].min_time_value), "06:%02d", i % 60);
    config.timer[i].use_max_time = (i % 3) == 1;
    snprintf(config.timer[i].max_time_value, sizeof(config.timer[i].max_time_value), "22:%02d", i % 60);
    config.timer[i].astro_mode = (uint8_t)(i % 5);
    config.timer[i].horizon_value = (int8_t)(i - 10);
  }

  config.geo.latitude = 47.5f;
  config.geo.longitude = 19.25f;
}

// Report the first byte that differs, as an offset into the struct, so a
// failure points at a field instead of only saying the two are not equal.
static void assertConfigsEqual(const s_config &expected, const s_config &actual) {
  const unsigned char *a = (const unsigned char *)&expected;
  const unsigned char *b = (const unsigned char *)&actual;
  for (size_t i = 0; i < sizeof(s_config); i++) {
    if (a[i] != b[i]) {
      char msg[160];
      snprintf(msg, sizeof(msg), "config differs at byte offset %u of %u (expected 0x%02X, got 0x%02X)", (unsigned)i, (unsigned)sizeof(s_config),
               a[i], b[i]);
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

/*
 * Build the config file an older firmware would have left behind.
 *
 * Rather than hand-write a fixture that drifts the moment a key is renamed,
 * this saves a current config and then removes exactly what did not exist at
 * the requested version. The key names therefore always match the ones
 * configSaveToFile() really writes.
 *
 *   V3 added jaro.ch_travel_down / ch_travel_up
 *   V4 added timer[].astro_mode / horizon_value
 *   V5 raised the timer count from 6 to TIMER_COUNT
 */
static void plantOlderConfigFile(int version) {
  configSaveToFile();
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, fakeFsRead("/config.json")));

  doc["version"] = version;

  if (version < 5) {
    JsonArray timers = doc["timer"].as<JsonArray>();
    while (timers.size() > 6) {
      timers.remove(timers.size() - 1);
    }
  }
  if (version < 4) {
    for (JsonObject timer : doc["timer"].as<JsonArray>()) {
      timer.remove("astro_mode");
      timer.remove("horizon_value");
    }
  }
  if (version < 3) {
    doc["jaro"].as<JsonObject>().remove("ch_travel_down");
    doc["jaro"].as<JsonObject>().remove("ch_travel_up");
  }

  std::string out;
  serializeJson(doc, out);
  fakeFsWrite("/config.json", out);
}

void setUp(void) {
  fakeFsClear();
  setupMode = false;
  zeroConfig();
}

void tearDown(void) {}

/* R O U N D   T R I P ********************************************************/

// The one that earns the suite: every field written has to come back. A key
// written under one name and read under another is invisible to the compiler.
static void test_every_field_survives_a_save_and_load() {
  fillConfig();
  s_config expected = config;

  configSaveToFile();

  zeroConfig();
  configLoadFromFile();

  assertConfigsEqual(expected, config);
}

static void test_the_saved_file_is_valid_json_at_the_current_version() {
  fillConfig();
  configSaveToFile();

  std::string raw = fakeFsRead("/config.json");
  TEST_ASSERT_TRUE_MESSAGE(raw.size() > 100, "nothing was written to the config file");

  JsonDocument doc;
  TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, raw), "the config file is not valid JSON");
  TEST_ASSERT_EQUAL_INT(CFG_VERSION, doc["version"].as<int>());
}

static void test_the_wifi_password_is_not_written_in_clear() {
  fillConfig();
  configSaveToFile();

  TEST_ASSERT_TRUE_MESSAGE(fakeFsRead("/config.json").find("wifi-password-value") == std::string::npos,
                           "the WiFi password is stored in clear text");
}

// A save rewrites rather than appends. The firmware removes the file first for
// exactly this reason; without that, a config that shrinks leaves the tail of
// the previous one behind and the result does not parse.
static void test_saving_twice_does_not_append_to_the_previous_file() {
  fillConfig();
  configSaveToFile();
  size_t first = fakeFsRead("/config.json").size();

  configSaveToFile();
  size_t second = fakeFsRead("/config.json").size();

  TEST_ASSERT_EQUAL_size_t_MESSAGE(first, second, "the second save did not replace the first");
}

/* U P G R A D I N G   F R O M   A N   O L D E R   F I L E ********************/

static void test_a_v2_config_keeps_every_setting_it_carried() {
  fillConfig();
  plantOlderConfigFile(2);

  zeroConfig();
  configLoadFromFile();

  // a sample from each subsystem that existed at V2
  TEST_ASSERT_EQUAL_STRING("wifi-ssid-value", config.wifi.ssid);
  TEST_ASSERT_EQUAL_STRING("wifi-password-value", config.wifi.password);
  TEST_ASSERT_EQUAL_STRING("jarolift/base", config.mqtt.topic);
  TEST_ASSERT_EQUAL_UINT16(8883, config.mqtt.port);
  TEST_ASSERT_EQUAL_STRING("channel-0", config.jaro.ch_name[0]);
  TEST_ASSERT_EQUAL_STRING("channel-15", config.jaro.ch_name[15]);
  TEST_ASSERT_EQUAL_UINT32(0x001A4A00, config.jaro.serial);
  TEST_ASSERT_EQUAL_INT(22, config.gpio.gdo0);
  TEST_ASSERT_TRUE(config.timer[0].enable);
  TEST_ASSERT_EQUAL_STRING("00:00", config.timer[0].time_value);
  TEST_ASSERT_EQUAL_FLOAT(47.5f, config.geo.latitude);
}

/*
 * The upgrade path nothing else checks.
 *
 * configInitValue() does not run here - the file parsed - so every field added
 * after V2 is read from a key that does not exist. This asserts that the zero
 * each one lands on is the value the firmware actually wants, which is what
 * makes the silence safe rather than merely quiet.
 */
static void test_fields_added_after_v2_land_on_their_documented_sentinel() {
  fillConfig();
  plantOlderConfigFile(2);

  zeroConfig();
  configLoadFromFile();

  for (int i = 0; i < 16; i++) {
    // V3. include/config.h: "0 = not calibrated", and shutterPos treats an
    // uncalibrated channel as one that settles at an end stop instead of
    // interpolating - so zero here is a working shutter, not a broken one.
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, config.jaro.ch_travel_down[i], "travel time should read as uncalibrated");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, config.jaro.ch_travel_up[i], "travel time should read as uncalibrated");
  }

  for (int i = 0; i < 6; i++) {
    // V4. 0 is ASTRO_REAL with no horizon offset, which is exactly what these
    // timers did before twilight modes existed.
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, config.timer[i].astro_mode, "astro mode should read as the plain sunrise/sunset");
    TEST_ASSERT_EQUAL_INT8_MESSAGE(0, config.timer[i].horizon_value, "horizon offset should read as none");
  }

  for (int i = 6; i < TIMER_COUNT; i++) {
    // V5. The slots the old file never had must be off, not enabled with a
    // zeroed schedule - an enabled timer at 00:00 would move shutters at
    // midnight on the first night after an upgrade.
    TEST_ASSERT_FALSE_MESSAGE(config.timer[i].enable, "a timer slot the old config never had came up enabled");
  }
}

static void test_an_upgrade_rewrites_the_file_at_the_current_version() {
  fillConfig();
  plantOlderConfigFile(2);

  zeroConfig();
  configLoadFromFile();

  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, fakeFsRead("/config.json")));
  TEST_ASSERT_EQUAL_INT_MESSAGE(CFG_VERSION, doc["version"].as<int>(), "the config file was not migrated on disk");
  TEST_ASSERT_EQUAL_INT_MESSAGE(TIMER_COUNT, (int)doc["timer"].as<JsonArray>().size(), "the migrated file does not carry every timer slot");
}

// Load, then load again from what the first load wrote. Anything the migration
// mangles shows up as a difference between the two, and this is the state the
// device actually boots into from the second boot onwards.
static void test_a_migrated_config_is_stable_across_a_second_load() {
  fillConfig();
  plantOlderConfigFile(2);

  zeroConfig();
  configLoadFromFile();
  s_config afterUpgrade = config;

  zeroConfig();
  configLoadFromFile();

  // The version field is the one legitimate difference: the first load reports
  // the version it read from the old file, the second reads the migrated one.
  afterUpgrade.version = config.version;
  assertConfigsEqual(afterUpgrade, config);
}

static void test_a_v3_config_still_gets_the_later_defaults() {
  fillConfig();
  plantOlderConfigFile(3);

  zeroConfig();
  configLoadFromFile();

  // V3 had travel times, so those must survive rather than reset
  TEST_ASSERT_EQUAL_UINT32(20000u, config.jaro.ch_travel_down[0]);
  TEST_ASSERT_EQUAL_UINT32(30015u, config.jaro.ch_travel_up[15]);
  TEST_ASSERT_EQUAL_UINT8(0, config.timer[0].astro_mode);
  TEST_ASSERT_FALSE(config.timer[TIMER_COUNT - 1].enable);
}

/* B R O K E N   A N D   M I S S I N G   F I L E S ****************************/

static void test_no_config_file_falls_back_to_defaults_and_setup_mode() {
  configLoadFromFile();

  TEST_ASSERT_TRUE_MESSAGE(setupMode, "a device with no config must come up in setup mode");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1883, config.mqtt.port, "defaults were not applied");
}

static void test_a_corrupt_config_file_falls_back_to_defaults_and_setup_mode() {
  fakeFsWrite("/config.json", "{\"wifi\": {\"ssid\": \"trunca");

  configLoadFromFile();

  TEST_ASSERT_TRUE_MESSAGE(setupMode, "an unparseable config must come up in setup mode");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1883, config.mqtt.port, "defaults were not applied");
}

static void test_an_empty_config_file_falls_back_to_defaults() {
  fakeFsWrite("/config.json", "");

  configLoadFromFile();

  TEST_ASSERT_TRUE(setupMode);
  TEST_ASSERT_EQUAL_INT(1883, config.mqtt.port);
}

/*
 * A file that claims more timers than the firmware has room for.
 *
 * The load loop is bounded by both timers.size() and TIMER_COUNT. If either
 * bound were dropped this would write past the end of config.timer, which is
 * the kind of thing that corrupts a neighbouring field rather than crashing -
 * under the sanitizers the suite runs with, it fails here instead.
 */
static void test_a_file_with_more_timers_than_the_firmware_supports_does_not_overflow() {
  fillConfig();
  configSaveToFile();

  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, fakeFsRead("/config.json")));
  JsonArray timers = doc["timer"].as<JsonArray>();
  for (int i = 0; i < 40; i++) {
    JsonObject extra = timers.add<JsonObject>();
    extra["enable"] = true;
    extra["type"] = 1;
    extra["time_value"] = "03:00";
    extra["grp_mask"] = 0xFFFF;
  }
  std::string out;
  serializeJson(doc, out);
  fakeFsWrite("/config.json", out);

  zeroConfig();
  configLoadFromFile();

  TEST_ASSERT_EQUAL_STRING("00:00", config.timer[0].time_value);
  TEST_ASSERT_EQUAL_UINT16(0x1000u + TIMER_COUNT - 1, config.timer[TIMER_COUNT - 1].grp_mask);
}

/*
 * A string key that is absent leaves the destination alone.
 *
 * EspStrUtil::readJSONstring returns without touching dest when the JSON value
 * is missing, so a field is not cleared - it keeps whatever the struct already
 * held. On a real device that is a zeroed global and the effect is invisible,
 * which is why it is worth pinning down: it means a field can only be relied on
 * to be empty if nothing wrote to it earlier, and a future load-into-populated-
 * struct would silently inherit instead of resetting.
 */
static void test_a_missing_string_key_keeps_whatever_was_already_there() {
  fillConfig();
  configSaveToFile();

  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, fakeFsRead("/config.json")));
  doc["mqtt"].as<JsonObject>().remove("topic");
  std::string out;
  serializeJson(doc, out);
  fakeFsWrite("/config.json", out);

  snprintf(config.mqtt.topic, sizeof(config.mqtt.topic), "left-over-value");
  configLoadFromFile();

  TEST_ASSERT_EQUAL_STRING_MESSAGE("left-over-value", config.mqtt.topic, "a missing key should leave the destination untouched");
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_every_field_survives_a_save_and_load);
  RUN_TEST(test_the_saved_file_is_valid_json_at_the_current_version);
  RUN_TEST(test_the_wifi_password_is_not_written_in_clear);
  RUN_TEST(test_saving_twice_does_not_append_to_the_previous_file);

  RUN_TEST(test_a_v2_config_keeps_every_setting_it_carried);
  RUN_TEST(test_fields_added_after_v2_land_on_their_documented_sentinel);
  RUN_TEST(test_an_upgrade_rewrites_the_file_at_the_current_version);
  RUN_TEST(test_a_migrated_config_is_stable_across_a_second_load);
  RUN_TEST(test_a_v3_config_still_gets_the_later_defaults);

  RUN_TEST(test_no_config_file_falls_back_to_defaults_and_setup_mode);
  RUN_TEST(test_a_corrupt_config_file_falls_back_to_defaults_and_setup_mode);
  RUN_TEST(test_an_empty_config_file_falls_back_to_defaults);
  RUN_TEST(test_a_file_with_more_timers_than_the_firmware_supports_does_not_overflow);
  RUN_TEST(test_a_missing_string_key_keeps_whatever_was_already_there);

  return UNITY_END();
}
