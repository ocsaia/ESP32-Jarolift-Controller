#pragma once

/*
 * Minimal Arduino/ESP-IDF stand-in for native unit tests.
 *
 * The firmware cannot run off-device, but a few modules are pure logic with a
 * very small surface: a clock, the config struct and a handful of functions
 * they call out to. This header supplies that surface so those modules can be
 * compiled and exercised on the host, with a clock the test drives itself.
 *
 * It is deliberately not a general Arduino emulation. Add to it only what a
 * module under test actually needs, so it stays obvious what is real and what
 * is faked.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

// Arduino type aliases that vendored libraries expect. Deliberately the same
// definitions the real core uses, not std::byte - Dusk2Dawn casts to them.
typedef uint8_t byte;
typedef bool boolean;
typedef unsigned int word;

// Same value the Arduino core defines. Dusk2Dawn converts between degrees and
// radians with it.
#define PI 3.1415926535897932384626433832795

// ---------------------------------------------------------------------------
// Fake clock. Tests move time explicitly - nothing here advances on its own,
// so a test that expects a stop after 12 s does not have to wait 12 s.
// ---------------------------------------------------------------------------
extern uint32_t testMillis;

inline uint32_t millis() { return testMillis; }
// muTimer offers a microsecond mode. Nothing under test uses it, but the class
// compiles it, so the clock has to answer in the same units the tests drive.
inline uint32_t micros() { return testMillis * 1000; }
inline void delay(uint32_t ms) { testMillis += ms; }

// ---------------------------------------------------------------------------
// GPIO. Nothing here drives a pin - config.cpp validates the pin numbers it was
// given and then configures them, and it is the validation that is worth
// testing. The calls are recorded so a test can assert what was configured
// without any of it meaning anything electrically.
// ---------------------------------------------------------------------------
#define LED_BUILTIN 2
#define INPUT 0x0
#define OUTPUT 0x3
#define INPUT_PULLUP 0x5
#define LOW 0x0
#define HIGH 0x1

extern int testPinMode[64];

inline void pinMode(int pin, int mode) {
  if (pin >= 0 && pin < 64) {
    testPinMode[pin] = mode;
  }
}
inline void digitalWrite(int pin, int value) {
  (void)pin;
  (void)value;
}
inline int digitalRead(int pin) {
  (void)pin;
  return 0;
}

// ---------------------------------------------------------------------------
// Logging: silent by default. Set testLogEcho to see the module's own reasoning
// while debugging a failing test.
// ---------------------------------------------------------------------------
extern bool testLogEcho;

#define TEST_LOG(level, tag, fmt, ...)                                                                                                     \
  do {                                                                                                                                     \
    if (testLogEcho) {                                                                                                                     \
      std::printf("[%s] " fmt "\n", tag, ##__VA_ARGS__);                                                                                   \
    }                                                                                                                                      \
  } while (0)

#define ESP_LOGE(tag, fmt, ...) TEST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) TEST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) TEST_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) TEST_LOG("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) TEST_LOG("V", tag, fmt, ##__VA_ARGS__)
