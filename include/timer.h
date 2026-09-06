#pragma once

#include <Arduino.h>

#define TYPE_FIXED_TIME 0
#define TYPE_SUNRISE 1
#define TYPE_SUNDOWN 2

void timerCyclic();

// Returns false when the sun does not cross the horizon on the current day
// (polar day / polar night) or when type is neither TYPE_SUNRISE nor
// TYPE_SUNDOWN. hour/minute are set to 0 in that case and must not be used as a
// time - Dusk2Dawn reports "no event" out of band, and running that value
// through the modulo wrap used to turn it into 23:59.
bool getSunriseOrSunset(uint8_t type, int16_t offset, float latitude, float longitude, uint8_t &hour, uint8_t &minute);
