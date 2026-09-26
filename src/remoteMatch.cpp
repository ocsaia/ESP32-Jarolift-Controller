#include <config.h>
#include <remoteMatch.h>

/* D E C L A R A T I O N S ****************************************************/

// the press currently being held, if any
static bool pressValid = false;
static uint32_t pressSerial = 0;
static int8_t pressFunction = 0;
static uint32_t pressLastMs = 0;

/**
 * *******************************************************************
 * @brief   find the configured remote a received serial belongs to
 * @details Exact matches are tried across the whole table before any prefix
 *          match, so an entry holding the full serial of one transmitter can
 *          never lose to a prefix entry that happens to sit earlier.
 *
 *          The two forms cannot be confused with each other by construction.
 *          A KeeLoq serial is 28 bits, so serial >> 8 is at most 20 bits
 *          (0xFFFFF). An entry above that can only ever match exactly; an
 *          entry at or below it could in principle match a different
 *          transmitter both ways, but only one whose top eight serial bits are
 *          all zero - and the exact pass runs first regardless.
 *
 *          Upstream compared serial >> 8 only, and nothing in the WebUI said
 *          so. The serial the log prints is the full one, so a user who copied
 *          it - the only serial they are ever shown - configured a remote that
 *          could never match, and every press was silently reported as coming
 *          from an unknown remote.
 * @param   serial  28 bit serial from the decoded frame
 * @return  0..15, or -1 if no enabled remote matches
 * *******************************************************************/
int remoteFind(uint32_t serial) {
  for (int i = 0; i < 16; i++) {
    if (config.jaro.remote_enable[i] && config.jaro.remote_serial[i] == serial) {
      return i;
    }
  }
  for (int i = 0; i < 16; i++) {
    if (config.jaro.remote_enable[i] && config.jaro.remote_serial[i] == (serial >> 8)) {
      return i;
    }
  }
  return -1;
}

/**
 * *******************************************************************
 * @brief   is this frame the same button press, still being held?
 * @details A remote repeats its frame for as long as the button is held,
 *          two to three decoded frames a second on the live device. Passing
 *          every one of them to the position tracker made it re-base its
 *          estimate on each: the position is banked in whole percent every
 *          time, so a five second hold lost several percent, and with an
 *          unknown start the end-stop timer restarted on every frame, pushing
 *          the "reached the end" report out to a full travel time after the
 *          button was released.
 *
 *          SHADE is the one function that changes mid-press: the radio library
 *          reports the eleventh STOP frame of a long press as SHADE, and the
 *          frames after it as STOP again. Treating those as a new STOP would
 *          settle the tracker at its old position and publish that over the
 *          shade position that was just reported.
 * @param   serial, function, nowMs
 * @return  true if the frame continues the press already reported
 * *******************************************************************/
bool remoteIsRepeat(uint32_t serial, int8_t function, uint32_t nowMs) {
  bool samePress = pressValid && serial == pressSerial && (nowMs - pressLastMs) < REMOTE_REPEAT_GAP_MS &&
                   (function == pressFunction || (pressFunction == REMOTE_FN_SHADE && function == REMOTE_FN_STOP));

  if (samePress) {
    pressLastMs = nowMs; // a held button keeps the press alive
    return true;
  }

  pressValid = true;
  pressSerial = serial;
  pressFunction = function;
  pressLastMs = nowMs;
  return false;
}

void remoteRepeatReset() { pressValid = false; }
