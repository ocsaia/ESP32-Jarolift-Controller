#pragma once

#include <stdint.h>

/*
 * Turn one captured frame - a low and a high duration per pulse - into its
 * fields.
 *
 * Layout by pulse index: 0 is the sync, then one pulse per bit, least
 * significant first: 1..32 the hop code, 33..60 the 28-bit serial, 61..64 the
 * function, 65..72 the group byte (the high byte of the channel mask, channels
 * 9-16). A full frame is therefore 73 pulses. A pulse whose low part is shorter
 * than its high part is a 0, anything else a 1.
 *
 * The group byte is read only when all of it was captured. Frames used to be
 * taken the moment they reached 65 pulses, before those eight had arrived, and
 * the empty buffer behind them decoded as 1s - the live device reported the
 * same button as 11111111, 00000000 and 00000001 in the high byte. A frame that
 * genuinely ends after the function nibble has no group byte, and that is 0.
 *
 * Kept free of Arduino and the radio so it can be tested on the host.
 */
struct FrameDecoder {
  static constexpr unsigned kHopFirst = 1;
  static constexpr unsigned kSerialFirst = 33;
  static constexpr unsigned kFunctionFirst = 61;
  static constexpr unsigned kGroupFirst = 65;
  static constexpr unsigned kFullFrame = 73; // sync + 72 bits

  struct Fields {
    uint32_t hop;
    uint32_t serial;
    uint8_t function;
    uint8_t group;
    bool groupPresent;
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

  // pulses: how many were captured, sync included; must be at least 65
  static Fields decode(const uint16_t *low, const uint16_t *high, unsigned pulses) {
    Fields f;
    f.hop = bits(low, high, kHopFirst, 32);
    f.serial = bits(low, high, kSerialFirst, 28);
    f.function = (uint8_t)bits(low, high, kFunctionFirst, 4);
    f.groupPresent = pulses >= kFullFrame;
    f.group = f.groupPresent ? (uint8_t)bits(low, high, kGroupFirst, 8) : 0;
    return f;
  }
};
