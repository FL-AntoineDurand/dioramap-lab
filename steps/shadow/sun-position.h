// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_SHADOW_SUN_POSITION_H
#define V3_STEPS_SHADOW_SUN_POSITION_H

// SunCalc-equivalent sun position routines, ported 1:1 from
// https://github.com/mourner/suncalc (BSD 2-Clause license,
// Copyright (c) Volodymyr Agafonkin — see LICENSES/BSD-2-Clause-suncalc.txt).
// Only the `getPosition` path is ported — the moon and rise/set
// machinery is not needed by the viewer-port pipeline.
//
// Algorithm reference: http://aa.quae.nl/en/reken/zonpositie.html (the
// same source SunCalc.js cites). Accuracy is ~1 arcminute, more than
// adequate for shadow-casting on a 0.5m grid.

// Sun position in horizontal coordinates (radians).
//
//   azimuth  : measured from south toward west, +ve clockwise viewed from
//              above (SunCalc.js convention — kept identical so behaviour
//              is bit-comparable with the JS bench).
//   altitude : angle above the horizon (negative below horizon).
typedef struct sun_position {
    double azimuth;
    double altitude;
} sun_position_t;

// Compute the sun position for a UTC instant at (lat, lon) in degrees.
// `unix_seconds` is the UTC Unix timestamp (seconds since epoch).
sun_position_t sun_position_at(double unix_seconds, double lat_deg, double lon_deg);

// Convenience: convert a UTC Y/M/D h:m to a Unix timestamp (no leap
// seconds, no timezone — the input is treated as UTC).
double sun_unix_from_utc(int year, int month, int day, int hour, int minute);

// Convert a sun position to a Lambert-93 unit vector pointing TOWARD the
// sun. Axes: x=east, y=north, z=up. Matches the JS bench's sunDirL93
// helper exactly:
//   L93_east  = -sin(azimuth) * cos(altitude)
//   L93_north = -cos(azimuth) * cos(altitude)
//   L93_up    =  sin(altitude)
// Below-horizon altitudes return a downward vector (z < 0).
void sun_dir_l93(sun_position_t pos, double out[3]);

#endif // V3_STEPS_SHADOW_SUN_POSITION_H
