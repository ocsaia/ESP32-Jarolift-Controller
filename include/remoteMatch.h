#pragma once

#include <stdint.h>

/*
 * Which configured remote a received frame belongs to, and whether it is a new
 * button press or the same press repeating.
 *
 * Kept free of the radio and of MQTT so it can be tested on the host - the
 * lookup decides whether a wall remote reaches the position tracker at all, and
 * the first version of it could never match the table a real installation had.
 */

// Frames of one held button arrive a few hundred milliseconds apart. A gap
// longer than this ends the press, so pressing the same button again later is
// a new press.
#define REMOTE_REPEAT_GAP_MS 1000u

// Function codes as the radio library reports them.
#define REMOTE_FN_DOWN 0x2
#define REMOTE_FN_SHADE 0x3
#define REMOTE_FN_STOP 0x4
#define REMOTE_FN_UP 0x8

/**
 * Index 0..15 of the enabled remote a frame with this serial belongs to, or -1.
 *
 * Two forms are accepted, exact first:
 *  - the full serial, exactly as the log prints it ("serial: 0x001a4a06" is
 *    entered as 1a4a06) - one entry per transmitter, which is also how a
 *    multi-channel handset with a serial per channel has to be entered;
 *  - upstream's form, the serial without its lowest byte (serial >> 8), for
 *    configurations written against upstream's comparison.
 */
int remoteFind(uint32_t serial);

/**
 * True if this frame continues the press already reported: same serial, same
 * function, and less than REMOTE_REPEAT_GAP_MS since the previous frame of that
 * press. A held STOP that the radio library has turned into SHADE keeps
 * arriving as STOP afterwards; those frames belong to the SHADE press too.
 *
 * Call it for every frame of a configured remote - each call extends a press
 * that is still being held.
 */
bool remoteIsRepeat(uint32_t serial, int8_t function, uint32_t nowMs);

// forget the press in progress - for tests, and after anything that should
// make the next frame count as new
void remoteRepeatReset();
