#pragma once

#include <Arduino.h>
#include <config.h>
#include <time.h>

#define TYPE_FIXED_TIME 0
#define TYPE_SUNRISE 1
#define TYPE_SUNDOWN 2

/*
 * Which definition of "sunrise" an astro timer uses. The sun has to reach a
 * given angle relative to the horizon, and the further below it the target is,
 * the later dawn and the earlier dusk - but also the more often it does not
 * happen at all. Astronomical twilight, for instance, does not occur at
 * mid-northern latitudes for several weeks around midsummer; a timer set to it
 * then correctly does not fire, rather than firing at some substitute time.
 *
 * ASTRO_HORIZON is for a view that is not the true horizon: a hill or a building
 * hides the sun while it is still above the true horizon, so sunset comes early
 * and sunrise late. horizon_value is that obstruction in degrees; a negative
 * value is the opposite case, a vantage point high enough to see past the
 * surroundings.
 */
#define ASTRO_REAL 0
#define ASTRO_CIVIL 1
#define ASTRO_NAUTICAL 2
#define ASTRO_ASTRONOMICAL 3
#define ASTRO_HORIZON 4

// a view blocked by more than this is not a shutter timing problem any more
#define ASTRO_HORIZON_MIN (-10)
#define ASTRO_HORIZON_MAX 30

// Solar zenith angle for a mode, in degrees. Exposed for tests.
float astroZenith(uint8_t astroMode, int8_t horizonValue);

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
bool getSunriseOrSunset(time_t now, uint8_t type, int16_t offset, float latitude, float longitude, uint8_t &hour, uint8_t &minute,
                        uint8_t astroMode = ASTRO_REAL, int8_t horizonValue = 0);

// Exposed for tests: minutes since midnight from "HH:MM", or -1 if the value is
// not a valid time.
int timeToMinutes(const char *time_value);

bool isDayEnabled(const s_cfg_timer &timer, int day);
bool checkTimerTrigger(const s_cfg_timer &timer, time_t now, uint8_t currentHour, uint8_t currentMinute);
