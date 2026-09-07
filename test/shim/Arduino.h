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

// ---------------------------------------------------------------------------
// Fake clock. Tests move time explicitly - nothing here advances on its own,
// so a test that expects a stop after 12 s does not have to wait 12 s.
// ---------------------------------------------------------------------------
extern uint32_t testMillis;

inline uint32_t millis() { return testMillis; }
inline void delay(uint32_t ms) { testMillis += ms; }

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
