/*
 * Native unit tests for received-frame integrity, with real KeeLoq.
 *
 * Every frame here is built the way a transmitter builds it and taken apart
 * the way the receiver does - key derived from the serial with the master key,
 * hop code encrypted and decrypted with the KeeLoq library the firmware uses.
 * Nothing about the cipher is faked, because the check under test is a property
 * of what decryption produces.
 *
 * The misaligned frames reproduce what the live device logged: a pulse decoder
 * one position out of step turns serial 0x1a4a06 into 0x34940c and UP into 0x0.
 *
 * The master key below is a test value, not any installation's.
 */

#include <unity.h>

#include <FrameCheck.h>
#include <KeeloqLib.h>

uint32_t testMillis = 0;
bool testLogEcho = false;
int testPinMode[64] = {0};

#include "../../lib/JaroliftController/KeeloqLib.cpp"

/* H E L P E R S **************************************************************/

static const unsigned long TEST_MASTER_MSB = 0x5A3C1E0FUL;
static const unsigned long TEST_MASTER_LSB = 0xC3A59687UL;

struct DeviceKey {
  uint32_t msb;
  uint32_t lsb;
};

// As rxKeyGen() and generateKey() do it. unsigned long is 64 bits on this host
// and 32 on the ESP32; the firmware keeps the result in uint32_t, which drops
// the bits KeeLoq decryption shifts above bit 31 here - so does this.
static DeviceKey deriveKey(uint32_t serial, unsigned long masterMsb = TEST_MASTER_MSB, unsigned long masterLsb = TEST_MASTER_LSB) {
  Keeloq k(masterMsb, masterLsb);
  DeviceKey d;
  d.lsb = (uint32_t)k.decrypt(serial | 0x20000000UL);
  d.msb = (uint32_t)k.decrypt(serial | 0x60000000UL);
  return d;
}

struct Frame {
  uint32_t hop;
  uint32_t serial; // 28 bit
  uint8_t fn;      // 4 bit
};

// What a transmitter sends: channel byte << 24 | serial low byte << 16 | counter,
// encrypted with the device key - generateEncrypted() on this firmware's side.
static Frame transmit(uint32_t serial, uint8_t fn, uint8_t channelByte, uint16_t counter, unsigned long masterMsb = TEST_MASTER_MSB,
                      unsigned long masterLsb = TEST_MASTER_LSB) {
  DeviceKey key = deriveKey(serial, masterMsb, masterLsb);
  Keeloq k(key.msb, key.lsb);
  uint32_t plain = ((uint32_t)channelByte << 24) | ((serial & 0xFF) << 16) | counter;
  Frame f;
  f.hop = (uint32_t)k.encrypt(plain);
  f.serial = serial & 0x0FFFFFFF;
  f.fn = fn & 0x0F;
  return f;
}

// What processRxData() does with it: key from the received serial, decrypt.
static uint32_t receiveDecoded(const Frame &f) {
  DeviceKey key = deriveKey(f.serial);
  Keeloq k(key.msb, key.lsb);
  return (uint32_t)k.decrypt(f.hop);
}

static bool accepted(const Frame &f) { return FrameCheck::valid(f.serial, f.fn, receiveDecoded(f)); }

/*
 * The frame as a decoder one pulse out of step reads it: an extra pulse after
 * the sync moves every bit up one index. Hop bit 0 becomes that stray pulse,
 * serial bit 0 becomes the last hop bit, function bit 0 the top serial bit.
 */
static Frame misaligned(const Frame &f, uint32_t strayBit) {
  Frame m;
  m.hop = (f.hop << 1) | (strayBit & 1);
  m.serial = ((f.serial << 1) | ((f.hop >> 31) & 1)) & 0x0FFFFFFF;
  m.fn = (uint8_t)(((f.fn << 1) | ((f.serial >> 27) & 1)) & 0x0F);
  return m;
}

void setUp(void) {}
void tearDown(void) {}

/* T E S T S ******************************************************************/

static void test_a_genuine_frame_is_accepted() {
  TEST_ASSERT_TRUE(accepted(transmit(0x1a4a06, 0x8, 0x40, 5368)));
}

static void test_genuine_frames_across_serials_channels_and_counters_are_accepted() {
  const uint32_t serials[] = {0x1a4a00, 0x1a4a06, 0x1a5000, 0x0000001, 0x0FFFFFF0};
  const uint8_t channels[] = {0x01, 0x40, 0x80};
  const uint16_t counters[] = {0, 1, 5368, 0xFFFF};
  for (uint32_t s : serials) {
    for (uint8_t ch : channels) {
      for (uint16_t c : counters) {
        TEST_ASSERT_TRUE(accepted(transmit(s, 0x4, ch, c)));
      }
    }
  }
}

// The device's own capture: 0x1a4a06 read one pulse late gives 0x34940c and a
// function of 0x0. The prediction of the misalignment model has to match what
// was logged, or the model is wrong.
static void test_the_misalignment_model_reproduces_the_capture() {
  Frame genuine = transmit(0x1a4a06, 0x8, 0x40, 5743);
  Frame read = misaligned(genuine, 0);
  TEST_ASSERT_EQUAL_HEX32(0x34940c | ((genuine.hop >> 31) & 1), read.serial);
  TEST_ASSERT_EQUAL_HEX8(0x0, read.fn);
}

static void test_a_misaligned_frame_is_rejected() {
  for (uint32_t stray = 0; stray <= 1; stray++) {
    for (uint16_t c = 5740; c < 5750; c++) {
      TEST_ASSERT_FALSE(accepted(misaligned(transmit(0x1a4a06, 0x8, 0x40, c), stray)));
    }
  }
}

// Why the function-code check alone is not enough: when the serial's top bit is
// set, a misaligned UP reads as 0x1 - a legitimate learn code. Only the serial
// byte in the decrypted word catches it.
static void test_a_misaligned_frame_with_a_valid_looking_function_is_still_rejected() {
  Frame genuine = transmit(0x08A1B2C3, 0x8, 0x40, 1234);
  Frame read = misaligned(genuine, 1);
  TEST_ASSERT_EQUAL_HEX8(0x1, read.fn);
  TEST_ASSERT_TRUE(FrameCheck::knownFunction(read.fn));
  TEST_ASSERT_FALSE(accepted(read));
}

// Random misaligned frames pass the serial-byte check about once in 256.
static void test_misaligned_frames_pass_only_about_one_in_256() {
  uint32_t lcg = 12345;
  int passed = 0;
  const int trials = 4096;
  for (int i = 0; i < trials; i++) {
    lcg = lcg * 1664525u + 1013904223u;
    uint32_t serial = (lcg >> 4) & 0x0FFFFFFF;
    lcg = lcg * 1664525u + 1013904223u;
    Frame f = misaligned(transmit(serial, 0x8, (uint8_t)(lcg >> 24), (uint16_t)lcg), lcg & 1);
    if (FrameCheck::serialByteMatches(f.serial, receiveDecoded(f))) {
      passed++;
    }
  }
  // expected 16; allow generous slack either way, a systematic flaw would be far off
  TEST_ASSERT_TRUE_MESSAGE(passed < 48, "far more misaligned frames pass than chance allows");
}

// A remote paired to somebody else's installation decrypts to noise here.
static void test_a_frame_under_another_master_key_is_rejected() {
  for (uint16_t c = 0; c < 8; c++) {
    Frame foreign = transmit(0x1a4a06, 0x8, 0x40, c, 0x11111111UL, 0x22222222UL);
    TEST_ASSERT_FALSE(accepted(foreign));
  }
}

static void test_unknown_function_codes_are_rejected() {
  const uint8_t unknown[] = {0x0, 0x3, 0x5, 0x6, 0x7, 0x9, 0xB, 0xC, 0xD, 0xE, 0xF};
  for (uint8_t fn : unknown) {
    TEST_ASSERT_FALSE(accepted(transmit(0x1a4a06, fn, 0x40, 100)));
  }
}

static void test_every_code_the_firmware_transmits_is_accepted() {
  const uint8_t known[] = {0x1, 0x2, 0x4, 0x8, 0xA};
  for (uint8_t fn : known) {
    TEST_ASSERT_TRUE(accepted(transmit(0x1a4a06, fn, 0x40, 100)));
  }
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_a_genuine_frame_is_accepted);
  RUN_TEST(test_genuine_frames_across_serials_channels_and_counters_are_accepted);
  RUN_TEST(test_the_misalignment_model_reproduces_the_capture);
  RUN_TEST(test_a_misaligned_frame_is_rejected);
  RUN_TEST(test_a_misaligned_frame_with_a_valid_looking_function_is_still_rejected);
  RUN_TEST(test_misaligned_frames_pass_only_about_one_in_256);
  RUN_TEST(test_a_frame_under_another_master_key_is_rejected);
  RUN_TEST(test_unknown_function_codes_are_rejected);
  RUN_TEST(test_every_code_the_firmware_transmits_is_accepted);

  return UNITY_END();
}
