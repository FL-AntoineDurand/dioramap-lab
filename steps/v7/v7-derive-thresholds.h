// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-derive-thresholds — per-building threshold record layout.
//
// Single source of truth for the density / spacing / distThreshold /
// z_noise values that v3's per-building v7 chain consumes. Mirrors
// v2's tile-wide `lidar-density` + `lidar-z-noise` artifacts, but
// scoped per-building because v3's extract-buildings architecture
// gives GRM and downstream steps a per-bldg subset of tile points
// rather than the full tile.
//
// Computed once by v7-derive-thresholds, read everywhere a v2 step
// reads `lidar-density` or `lidar-z-noise`. Each downstream step
// stops doing its own auto-compute / hardcoded-default.

#ifndef V3_STEPS_V7_DERIVE_THRESHOLDS_H
#define V3_STEPS_V7_DERIVE_THRESHOLDS_H

#include <stdint.h>

// Fixed-stride record per building (ARRAY_INLINE stride=48).
typedef struct v7_bldg_thresholds {
    double density;                 // n_pts / horiz-bbox-area (pts/m²)
    double spacing;                 // = 1 / sqrt(density)
    double z_noise;                 // = 0.02 (matches v2 tile-signals const)
    double dist_threshold_seed;     // = max(0.05, 4.5 · z_noise) = 0.09
    double dist_threshold_grm;      // = 0.12 (v2 hardcoded — strip-offset tolerance)
    double _reserved;               // pad to 48 bytes (multiple of 8)
} v7_bldg_thresholds_t;

#define V7_BLDG_THRESHOLDS_BYTES 48

#endif
