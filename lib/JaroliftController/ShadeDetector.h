#pragma once

#include <stdint.h>

/*
 * Long-press detection: a Jarolift remote has no SHADE button. Holding STOP
 * makes the receiver drive to its stored shade position, and all the remote
 * sends is STOP, over and over. This turns such a run into a single SHADE.
 *
 * It is decided by how long STOP has been held, not by how many frames arrived.
 * It used to be a frame count - the eleventh STOP frame - calibrated against a
 * loop() that stalled about 250 ms after every decoded frame, so eleven frames
 * happened to be roughly three seconds. Once that stall went, frames decode at
 * the remote's own repeat rate and a count would fire in about a second and a
 * half. Time does not depend on how fast anything downstream runs.
 *
 * Kept free of Arduino and the radio so it can be tested on the host.
 */
struct ShadeDetector {
  static constexpr uint8_t kFnStop = 0x4;
  static constexpr uint8_t kFnShade = 0x3;

  // How long STOP has to be held before it counts as SHADE. Roughly what the
  // frame count used to amount to, so the report does not move relative to the
  // receiver's own behaviour.
  static constexpr uint32_t kHoldMs = 3000;
  // A pause longer than this ends the run: two separate STOP presses must not
  // add up to a long one.
  static constexpr uint32_t kGapMs = 1500;

  uint32_t serial = 0;
  uint8_t channel = 0;
  uint32_t startMs = 0;
  uint32_t lastMs = 0;
  bool active = false;
  bool reported = false;

  /**
   * Feed one decoded frame, get back the function to report.
   *
   * Returns SHADE exactly once per run, on the first STOP frame at or after
   * kHoldMs; every other frame keeps its own function, so the STOP frames that
   * keep arriving after the SHADE are still reported as STOP.
   *
   * channel is the low byte of the decoded channel mask only. The high byte
   * comes from the last eight pulses of the frame, which are currently decoded
   * before they arrive and change from frame to frame of the same press -
   * comparing it would break every run apart.
   */
  uint8_t update(uint32_t frameSerial, uint8_t frameChannel, uint8_t function, uint32_t nowMs) {
    if (function != kFnStop) {
      active = false; // any other button ends the run
      return function;
    }
    if (!active || frameSerial != serial || frameChannel != channel || (nowMs - lastMs) > kGapMs) {
      active = true;
      serial = frameSerial;
      channel = frameChannel;
      startMs = nowMs;
      reported = false;
    }
    lastMs = nowMs;
    if (!reported && (nowMs - startMs) >= kHoldMs) {
      reported = true;
      return kFnShade;
    }
    return kFnStop;
  }
};
