#pragma once

#include <stdint.h>

/*
 * Turn one captured frame - a low and a high duration per pulse - into its
 * fields.
 *
 * Layout by pulse index: 0 is the sync, then one pulse per bit, least
 * significant first: 1..32 the hop code, 33..60 the 28-bit serial, 61..64 the
 * function, 65..72 the group byte (the high byte of the channel mask, channels
 * 9-16). A pulse whose low half is shorter than its high half is a 0, anything
 * else a 1: 400/800 us is a 1, 800/400 a 0.
 *
 * Why a genuine frame is captured as 72 pulses, not 73. The receiver sees the
 * transmitter's line inverted: the sync the ISR measures as a 3880 us low is
 * the transmitter's 380 us preamble high plus its 3500 us pause. Each bit is
 * stored when its low half ends, and the low half of the last bit - the top
 * group bit, channel 16 - runs straight into the 16 ms gap before the next
 * frame, far past the 1000 us a data pulse may last. It is never stored. Its
 * high half does arrive, one slot past the last stored pulse, and on its own it
 * still tells the bit: a 400 us high is a 1, an 800 us high a 0.
 *
 * So: a frame of 72 pulses has seven group bits in pairs and the eighth in its
 * high half; 73 (a frame with an extra pulse, or a transmitter whose timing
 * lets the last low end) has all eight in pairs; fewer than 72 has no usable
 * group byte, and it reads as 0. Frames used to be taken at 65 pulses, before
 * any group bit had arrived, and the cleared buffer behind them decoded as 1s -
 * the live device reported one button as 11111111, 00000000 and 00000001.
 *
 * Kept free of Arduino and the radio so it can be tested on the host.
 */
struct FrameDecoder {
  static constexpr unsigned kHopFirst = 1;
  static constexpr unsigned kSerialFirst = 33;
  static constexpr unsigned kFunctionFirst = 61;
  static constexpr unsigned kGroupFirst = 65;
  static constexpr unsigned kGroupLast = 72;   // index of the top group bit
  static constexpr unsigned kFullFrame = 73;   // every bit as a complete pulse
  static constexpr unsigned kCapturedFrame = 72; // what a genuine frame arrives as

  // A lone high half: shorter than the midpoint of 400 and 800 us is a 1. Outside
  // 300..1000 us, the range the ISR stores at all, it is not a half.
  static constexpr uint16_t kHalfMin = 300;
  static constexpr uint16_t kHalfMax = 1000;
  static constexpr uint16_t kHalfSplit = 600;

  struct Fields {
    uint32_t hop;
    uint32_t serial;
    uint8_t function;
    uint8_t group;
    bool groupPresent;   // at least the seven paired group bits were captured
    bool groupTopBitKnown; // the eighth came from a pair or a valid high half
  };

  static uint32_t bits(const uint16_t *low, const uint16_t *high, unsigned first, unsigned count) {
    uint32_t v = 0;
    for (unsigned i = 0; i < count; i++) {
      if (low[first + i] >= high[first + i]) {
        v |= (uint32_t)1 << i;
      }
    }
    return v;
  }

  // pulses: how many were captured, sync included; must be at least 65. The
  // arrays must hold at least kFullFrame entries.
  static Fields decode(const uint16_t *low, const uint16_t *high, unsigned pulses) {
    Fields f;
    f.hop = bits(low, high, kHopFirst, 32);
    f.serial = bits(low, high, kSerialFirst, 28);
    f.function = (uint8_t)bits(low, high, kFunctionFirst, 4);
    f.group = 0;
    f.groupPresent = pulses >= kCapturedFrame;
    f.groupTopBitKnown = false;

    if (pulses >= kFullFrame) {
      f.group = (uint8_t)bits(low, high, kGroupFirst, 8);
      f.groupTopBitKnown = true;
    } else if (pulses == kCapturedFrame) {
      f.group = (uint8_t)bits(low, high, kGroupFirst, 7);
      uint16_t half = high[kGroupLast];
      if (half >= kHalfMin && half < kHalfMax) {
        if (half < kHalfSplit) {
          f.group |= 0x80;
        }
        f.groupTopBitKnown = true;
      }
    }
    return f;
  }
};
