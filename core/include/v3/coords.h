// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_COORDS_H
#define V3_COORDS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Thin wrapper around PROJ for the specific conversions we need. Keeps
// PROJ's context + transformer pool off the step surface — all steps see
// is a pair of free functions.

// WGS84 (EPSG:4326, lon/lat in degrees) → Lambert-93 (EPSG:2154, metres).
// Returns 0 on success, -1 on conversion failure. Thread-safe.
/*@ requires out_x == \null || \valid(out_x);
    requires out_y == \null || \valid(out_y);
    assigns *out_x, *out_y;
    ensures \result == 0 || \result == -1;
*/
int wgs84_to_l93(double lon_deg, double lat_deg, double* out_x, double* out_y);

// Inverse: L93 → WGS84 (lon, lat in degrees).
/*@ requires out_lon_deg == \null || \valid(out_lon_deg);
    requires out_lat_deg == \null || \valid(out_lat_deg);
    assigns *out_lon_deg, *out_lat_deg;
    ensures \result == 0 || \result == -1;
*/
int l93_to_wgs84(double x, double y, double* out_lon_deg, double* out_lat_deg);

#ifdef __cplusplus
}
#endif
#endif
