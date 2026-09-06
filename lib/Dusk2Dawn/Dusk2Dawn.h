/*  Dusk2Dawn.h
 *  Get estimate time of sunrise and sunset given a set of coordinates.
 *  Created by DM Kishi <dm.kishi@gmail.com> on 2017-02-01.
 *  <https://github.com/dmkishi/Dusk2Dawn>
 */

#ifndef Dusk2Dawn_h
#define Dusk2Dawn_h

  #include "Arduino.h"
  #include <math.h>

  /* Returned by sunrise() and sunset() when the sun does not cross the horizon
   * on the requested date - polar day or polar night.
   *
   * It has to be a value no legal result can take. sunriseSet() does NOT
   * normalise its result into [0, 1440): an event shortly before local midnight
   * comes out as a small negative minute count, which happens for real at high
   * latitude (at 70.4N / 31.1E, sunrise at the polar-day boundary is minute -2)
   * and whenever the configured time zone lies west of the configured
   * longitude. Bounding the arithmetic over longitude +/-180 and time zones
   * -12..+14 puts every legal value inside roughly [-1456, 3076]. The library
   * used to report "no event" as -1, which is inside that range, so callers
   * could not tell the two apart and wrapping the value with
   * (x + 1440) % 1440 turned "no event" into 23:59.
   */
  #define DUSK2DAWN_NO_EVENT (-30000)

  class Dusk2Dawn {
    public:
      Dusk2Dawn(float, float, float);
      /* Both return the minutes elapsed since local midnight, or
       * DUSK2DAWN_NO_EVENT when there is no such event on that date. Test for
       * the sentinel first; the value is otherwise NOT range-checked, so
       * normalise it with ((x % 1440) + 1440) % 1440 before use.
       */
      int sunrise(int, int, int, bool);
      int sunset(int, int, int, bool);
      static bool min2str(char*, int);
    private:
      float _latitude, _longitude;
      /* Though most time zones are offset by whole hours, there are a few zones
       * offset by 30 or 45 minutes (UTC+05:30, UTC+05:45, UTC-03:30), which is
       * why the constructor already takes a float. Storing the offset in an int
       * truncated it towards zero, so sunrise and sunset were reported up to 45
       * minutes off for every user in such a zone.
       */
      float _timezone;
      int   sunriseSet(bool, int, int, int, bool);
      float sunriseSetUTC(bool, float, float, float);
      float equationOfTime(float);
      float meanObliquityOfEcliptic(float);
      float eccentricityEarthOrbit(float);
      float sunDeclination(float);
      float sunApparentLong(float);
      float sunTrueLong(float);
      float sunEqOfCenter(float);
      float hourAngleSunrise(float, float);
      float obliquityCorrection(float);
      float geomMeanLongSun(float);
      float geomMeanAnomalySun(float);
      float jDay(int, int, int);
      float fractionOfCentury(float);
      float radToDeg(float);
      float degToRad(float);
      static bool zeroPadTime(char*, byte);
  };

#endif
