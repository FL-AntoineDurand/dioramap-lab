// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// SunCalc.js getPosition() port. See sun-position.h for the algorithm
// reference and license note. Routines below mirror the JS function names
// for easy cross-checking against the original.

#include "sun-position.h"

#include <math.h>

static const double PI_LOCAL = 3.14159265358979323846;
static const double DAY_MS = 86400000.0;
static const double J1970 = 2440588.0;
static const double J2000 = 2451545.0;
static const double E_OBLIQUITY = 0.40909262920459006; // 23.4397° in radians

// ---- Date <-> Julian helpers ----------------------------------------------

static inline double to_julian_from_unix(double unix_seconds) {
    // SunCalc: date.valueOf() / dayMs - 0.5 + J1970, where valueOf is ms.
    return (unix_seconds * 1000.0) / DAY_MS - 0.5 + J1970;
}

static inline double to_days_from_unix(double unix_seconds) {
    return to_julian_from_unix(unix_seconds) - J2000;
}

// ---- Trig helpers exactly mirroring suncalc.js ----------------------------

static inline double right_ascension(double l, double b) {
    return atan2(sin(l) * cos(E_OBLIQUITY) - tan(b) * sin(E_OBLIQUITY), cos(l));
}

static inline double declination(double l, double b) {
    return asin(sin(b) * cos(E_OBLIQUITY) + cos(b) * sin(E_OBLIQUITY) * sin(l));
}

static inline double azimuth_calc(double H, double phi, double dec) {
    return atan2(sin(H), cos(H) * sin(phi) - tan(dec) * cos(phi));
}

static inline double altitude_calc(double H, double phi, double dec) {
    return asin(sin(phi) * sin(dec) + cos(phi) * cos(dec) * cos(H));
}

static inline double sidereal_time(double d, double lw) {
    const double rad = PI_LOCAL / 180.0;
    return rad * (280.16 + 360.9856235 * d) - lw;
}

static inline double solar_mean_anomaly(double d) {
    const double rad = PI_LOCAL / 180.0;
    return rad * (357.5291 + 0.98560028 * d);
}

static inline double ecliptic_longitude(double M) {
    const double rad = PI_LOCAL / 180.0;
    double C = rad * (1.9148 * sin(M) + 0.02 * sin(2.0 * M) + 0.0003 * sin(3.0 * M));
    double P = rad * 102.9372;
    return M + C + P + PI_LOCAL;
}

// Returns (declination, right_ascension) packed into out[2].
static inline void sun_coords(double d, double out[2]) {
    double M = solar_mean_anomaly(d);
    double L = ecliptic_longitude(M);
    out[0] = declination(L, 0.0);
    out[1] = right_ascension(L, 0.0);
}

// ---- Public API ----------------------------------------------------------

sun_position_t sun_position_at(double unix_seconds, double lat_deg, double lon_deg) {
    const double rad = PI_LOCAL / 180.0;
    double lw = rad * (-lon_deg);
    double phi = rad * lat_deg;
    double d = to_days_from_unix(unix_seconds);

    double c[2];
    sun_coords(d, c);
    double dec = c[0];
    double ra = c[1];
    double H = sidereal_time(d, lw) - ra;

    sun_position_t out = {
        .azimuth = azimuth_calc(H, phi, dec),
        .altitude = altitude_calc(H, phi, dec),
    };
    return out;
}

double sun_unix_from_utc(int year, int month, int day, int hour, int minute) {
    // Pure proleptic-Gregorian calendar conversion (no leap seconds, no
    // timezone — input is treated as UTC). Days-from-civil routine adapted
    // from Howard Hinnant's date library, public domain. Handles the full
    // proleptic Gregorian range we'll ever query (1900–2200 fits trivially).
    int y = year - (month <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                       // [0, 399]
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1); // [0, 365]
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
    long long days_since_epoch = (long long)era * 146097 + (long long)doe - 719468;
    return (double)days_since_epoch * 86400.0 + (double)hour * 3600.0 + (double)minute * 60.0;
}

void sun_dir_l93(sun_position_t pos, double out[3]) {
    double cosA = cos(pos.altitude);
    out[0] = -sin(pos.azimuth) * cosA;  // east
    out[1] = -cos(pos.azimuth) * cosA;  // north
    out[2] = sin(pos.altitude);          // up
}
