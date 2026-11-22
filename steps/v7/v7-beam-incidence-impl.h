// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-beam-incidence — pure compute. Direct port of bench-v2
// lib/pipeline/steps/beam-incidence.js (meta.version=5), restricted
// to the V7-DAG-load-bearing path.
//
// Drops vs v2 (per Phase 5.6 audit):
//   - Phase 4: subsampled beam edges (viz only)
//   - Phase 5: sun-beam edges + lat/lon + sun-position calc
//   - psid-colors output (RGB per point, viz only)
//   - beam-incidence-meta output (per-strip diagnostics)
//   - lidar-metadata, lambert93-origin reads (only Phase 5 used them)
//
// Keeps Phases 1, 2a, 2b, 3b → produces the single load-bearing
// `beam-directions` output (F32×N×3 unit vectors from aircraft toward
// ground). Consumer: v7-boundary-signals' nearby-projection step
// (line 637 of v2's boundary-signals.js).

#ifndef V3_STEPS_V7_BEAM_INCIDENCE_IMPL_H
#define V3_STEPS_V7_BEAM_INCIDENCE_IMPL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_beam_params {
    int32_t min_points_per_strip;   // default 30 — drop tiny strips
    double  min_band_elongation;    // default 3.0 — eigenvalue ratio for
                                    //   PCA band to count as a clear line
    int32_t mr_min_pairs;           // default 5 — min consecutive
                                    //   return-1 → return-2 pairs in a
                                    //   strip to apply MR correction
    double  mr_max_angle_jitter;    // default 0.1 (degrees) — pairs must
                                    //   have nearly-identical scan angles
    double  mr_min_pair_dist;       // default 0.05 (m) — pair separation
                                    //   floor (drop noise pairs)
    int32_t up_axis;                // 1 = y-up (bench), 2 = z-up (LAZ)
} v7_beam_params_t;

void v7_beam_params_default(v7_beam_params_t* p);

// Output buffer is ALWAYS sized n_pts × 3, filled with the default
// straight-down vector (0, -1, 0 in y-up; 0, 0, -1 in z-up) for points
// that don't get a strip fit. Caller owns the buffer.
//
// Returns 0 on success, -1 on alloc failure.
int v7_beam_compute(const double* positions,         // F64×3·n_pts
                    const float*  scan_angles,       // F32×n_pts (degrees)
                    const uint16_t* psids,           // U16×n_pts
                    const uint8_t*  return_numbers,  // U8×n_pts (NULL → no MR correction)
                    const uint8_t*  number_of_returns, // U8×n_pts (NULL → no MR correction)
                    size_t n_pts,
                    const uint32_t* building_indices, // U32×n_bldg — points
                                                      //   to use for per-strip
                                                      //   flight-direction PCA
                    size_t n_bldg,
                    const v7_beam_params_t* params,
                    float* out_beam_dirs);            // pre-allocated F32×3·n_pts

#ifdef __cplusplus
}
#endif
#endif
