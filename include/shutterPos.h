#pragma once

#include <stdint.h>

/*
 * Time-based position tracking for Jarolift TDEF shutters.
 *
 * The receivers give no feedback whatsoever, so a position can only ever be an
 * estimate: the firmware measures how long a full travel takes, then works out
 * how long the motor has to run to land on a requested percentage and sends an
 * RF STOP at that moment.
 *
 * Position convention is Home Assistant's, which is the inverse of what this
 * firmware used before:
 *
 *   0   = fully closed (bottom end-stop)
 *   100 = fully open   (top end-stop)
 *
 * The estimate is deliberately NOT persisted. After a reboot the real position
 * is unknown anyway - a physical remote may have moved the shutter while the
 * controller was down - and writing it to config.json would cost a full file
 * rewrite per movement, because configCyclic() saves the whole struct whenever
 * its hash changes.
 */

// returned by shutterPosGet() when the shutter has not been driven to an
// end-stop since boot, so no estimate exists yet
#define SHUTTER_POS_UNKNOWN (-1)

void shutterPosSetup();
void shutterPosCyclic();

// Called from processJaroCommands() at the moment the RF telegram is actually
// transmitted, and from the remote-control receive path. The queue delay and
// the transmit time must not count towards the travel, so the stopwatch starts
// here rather than where the command was accepted.
void shutterPosNotifyUp(uint8_t channel);
void shutterPosNotifyDown(uint8_t channel);
void shutterPosNotifyStop(uint8_t channel);

// Drive to an absolute position. Returns false if the channel is out of range
// or has no calibrated travel time.
bool shutterPosSetTarget(uint8_t channel, uint8_t targetPct);

int8_t shutterPosGet(uint8_t channel);
bool shutterPosIsCalibrated(uint8_t channel);
bool shutterPosIsMoving(uint8_t channel);
