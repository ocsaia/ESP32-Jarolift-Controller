/*
 * Native unit tests for frame decoding.
 *
 * Frames used to be taken the moment the pulse count reached 65, before the
 * frame's last eight pulses - the group byte, channels 9 to 16 - had arrived.
 * The empty buffer behind them decoded as ones, so the live device reported
 * the same button with a high byte of 11111111, 00000000 and 00000001. These
 * tests build pulse sequences the way the ISR stores them and check that every
 * field comes out exactly, and that a missing group byte reads as zero.
 */

#include <string.h>

#include <unity.h>

#include <FrameDecoder.h>

/* H E L P E R S **************************************************************/

static uint16_t low[76];
static uint16_t high[76];

// A 1 is a pulse whose low part is not shorter than its high part.
static void putBit(unsigned index, bool one) {
  low[index] = one ? 800 : 400;
  high[index] = one ? 400 : 800;
}

static void putField(unsigned first, uint32_t value, unsigned bits) {
  for (unsigned i = 0; i < bits; i++) {
    putBit(first + i, (value >> i) & 1);
  }
}

// Build a frame and leave everything from `pulses` on as the ISR leaves a
// buffer it has just been cleared: zeros.
static void buildFrame(uint32_t hop, uint32_t serial, uint8_t fn, uint8_t group, unsigned pulses) {
  memset(low, 0, sizeof(low));
  memset(high, 0, sizeof(high));
  low[0] = 3900; // sync
  putField(FrameDecoder::kHopFirst, hop, 32);
  putField(FrameDecoder::kSerialFirst, serial, 28);
  putField(FrameDecoder::kFunctionFirst, fn, 4);
  putField(FrameDecoder::kGroupFirst, group, 8);
  for (unsigned i = pulses; i < 76; i++) {
    low[i] = 0;
    high[i] = 0;
  }
}

void setUp(void) {}
void tearDown(void) {}

/* T E S T S ******************************************************************/

static void test_a_full_frame_decodes_every_field() {
  buildFrame(0xDEADBEEF, 0x001a4a06, 0x8, 0x00, 73);
  FrameDecoder::Fields f = FrameDecoder::decode(low, high, 73);
  TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, f.hop);
  TEST_ASSERT_EQUAL_HEX32(0x001a4a06, f.serial);
  TEST_ASSERT_EQUAL_HEX8(0x8, f.function);
  TEST_ASSERT_TRUE(f.groupPresent);
  TEST_ASSERT_EQUAL_HEX8(0x00, f.group);
}

static void test_the_group_byte_is_read_when_it_was_captured() {
  buildFrame(0x12345678, 0x0ABCDEF, 0x4, 0xA5, 73);
  TEST_ASSERT_EQUAL_HEX8(0xA5, FrameDecoder::decode(low, high, 73).group);
  TEST_ASSERT_EQUAL_HEX8(0xA5, FrameDecoder::decode(low, high, 75).group);
}

// The capture's fault. Taken at 65 pulses, the group byte comes from the
// cleared buffer: 0 < 0 is false, so every bit read as 1. It has to read as
// absent instead.
static void test_a_frame_that_ends_before_the_group_byte_has_none() {
  buildFrame(0xCAFEF00D, 0x001a4a06, 0x8, 0x5A, 65);
  FrameDecoder::Fields f = FrameDecoder::decode(low, high, 65);
  TEST_ASSERT_FALSE(f.groupPresent);
  TEST_ASSERT_EQUAL_HEX8(0x00, f.group);
  // what the old decoder made of the same buffer
  TEST_ASSERT_EQUAL_HEX32(0xFF, FrameDecoder::bits(low, high, FrameDecoder::kGroupFirst, 8));
  // the fields before it are unaffected
  TEST_ASSERT_EQUAL_HEX32(0xCAFEF00D, f.hop);
  TEST_ASSERT_EQUAL_HEX32(0x001a4a06, f.serial);
  TEST_ASSERT_EQUAL_HEX8(0x8, f.function);
}

// A partly captured group byte is not a group byte either.
static void test_a_partial_group_byte_reads_as_none() {
  buildFrame(0x11111111, 0x0222222, 0x2, 0xFF, 70);
  FrameDecoder::Fields f = FrameDecoder::decode(low, high, 70);
  TEST_ASSERT_FALSE(f.groupPresent);
  TEST_ASSERT_EQUAL_HEX8(0x00, f.group);
}

// The top hop bit has to come out whole. (The old loops shifted a signed 1 by
// 31; that is defined since C++14, which this project builds as, so it was not
// a fault - the unsigned shift here is only the plainer way to write it.)
static void test_the_top_bit_of_the_hop_code_survives() {
  buildFrame(0x80000000, 0x0000001, 0x8, 0x00, 73);
  TEST_ASSERT_EQUAL_HEX32(0x80000000, FrameDecoder::decode(low, high, 73).hop);
}

// Equal halves decode as 1, the same rule the original decoder applied.
static void test_equal_low_and_high_decode_as_one() {
  memset(low, 0, sizeof(low));
  memset(high, 0, sizeof(high));
  for (unsigned i = 1; i <= 32; i++) {
    low[i] = 600;
    high[i] = 600;
  }
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, FrameDecoder::bits(low, high, 1, 32));
}

static void test_every_serial_bit_lands_in_its_place() {
  for (unsigned b = 0; b < 28; b++) {
    uint32_t serial = (uint32_t)1 << b;
    buildFrame(0, serial, 0x4, 0, 73);
    TEST_ASSERT_EQUAL_HEX32(serial, FrameDecoder::decode(low, high, 73).serial);
  }
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_a_full_frame_decodes_every_field);
  RUN_TEST(test_the_group_byte_is_read_when_it_was_captured);
  RUN_TEST(test_a_frame_that_ends_before_the_group_byte_has_none);
  RUN_TEST(test_a_partial_group_byte_reads_as_none);
  RUN_TEST(test_the_top_bit_of_the_hop_code_survives);
  RUN_TEST(test_equal_low_and_high_decode_as_one);
  RUN_TEST(test_every_serial_bit_lands_in_its_place);

  return UNITY_END();
}
