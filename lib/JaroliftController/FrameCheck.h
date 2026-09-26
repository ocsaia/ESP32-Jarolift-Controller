#pragma once

#include <stdint.h>

/*
 * Is a decoded frame a genuine one, or noise that happened to have the right
 * number of pulses?
 *
 * Nothing used to ask. A frame that the pulse decoder read one position out of
 * step - one extra pulse after the sync - still decrypted to something, and was
 * reported: the live device logged "serial: 0x0034940c | cmd: 0x0" in the middle
 * of a held button, and 0x34940c is the real serial 0x1a4a06 shifted left by
 * exactly one bit. Such a frame looked like a different remote, broke the press
 * it landed in, and the next genuine frame was reported as a new press - the
 * position tracker was told UP four times during one five second hold.
 *
 * Two checks, both cheap:
 *
 *  - The function code has to be one a Jarolift transmitter sends. Every
 *    damaged frame in the capture carried 0x0.
 *  - The decrypted word has to carry the serial's low byte. This firmware's own
 *    transmit side builds that word as
 *        channel byte << 24 | serial low byte << 16 | counter
 *    and the receiving side already takes its channel from the top byte of the
 *    same word, which works - so remotes build it the same way. A misaligned
 *    frame is decrypted with a key derived from the wrong serial, and passes
 *    this by chance once in 256, whatever its function nibble says.
 *
 * Kept free of Arduino and the radio so it can be tested on the host.
 */
struct FrameCheck {
  static bool knownFunction(uint8_t fn) {
    // the codes this firmware transmits itself: UP, DOWN, STOP, and the two
    // learn codes (UP+DOWN, and the legacy 0x1)
    return fn == 0x1 || fn == 0x2 || fn == 0x4 || fn == 0x8 || fn == 0xA;
  }

  static bool serialByteMatches(uint32_t serial, uint32_t decoded) { return ((decoded >> 16) & 0xFF) == (serial & 0xFF); }

  static bool valid(uint32_t serial, uint8_t fn, uint32_t decoded) { return knownFunction(fn) && serialByteMatches(serial, decoded); }
};
