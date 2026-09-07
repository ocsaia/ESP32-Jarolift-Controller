#pragma once

#include <Arduino.h>
#include <config.h>
#include <time.h>

#define TYPE_FIXED_TIME 0
#define TYPE_SUNRISE 1
#define TYPE_SUNDOWN 2

void timerCyclic();

// Returns false when the sun does not cross the horizon on that day (polar day
// or polar night) or when type is neither TYPE_SUNRISE nor TYPE_SUNDOWN.
// hour/minute are set to 0 in that case and must not be used as a time -
// Dusk2Dawn reports "no event" out of band, and running that value through the
// modulo wrap used to turn it into 23:59.
//
// The moment is passed in rather than read from the clock so the result depends
// only on the arguments. That is what makes the astro maths testable off-device
// for a chosen date and latitude, including the polar cases that cannot be
// reproduced on demand at the machine's own date.
bool getSunriseOrSunset(time_t now, uint8_t type, int16_t offset, float latitude, float longitude, uint8_t &hour, uint8_t &minute);

// Exposed for tests: minutes since midnight from "HH:MM", or -1 if the value is
// not a valid time.
int timeToMinutes(const char *time_value);

bool isDayEnabled(const s_cfg_timer &timer, int day);
bool checkTimerTrigger(const s_cfg_timer &timer, time_t now, uint8_t currentHour, uint8_t currentMinute);
