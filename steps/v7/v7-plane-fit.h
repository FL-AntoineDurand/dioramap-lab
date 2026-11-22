// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-plane-fit — area-weighted least-squares plane fit + axis-aware
// orientation helpers. Direct C port of bench-v2 lib/geometry/pca.js's
// fitPlaneWeighted + lib/geometry/primitives.js's tilt/azimuth/distance.
//
// Reused by v7-normal-cascade-seed, v7-grow-refit-merge, v7-density-
// voids-2 — all fit planes from point-index lists during their inner
// loops.
//
// WEIGHTING IS EXPLICIT in the pointer: `point_weights == NULL` fits
// UNWEIGHTED (every point equal); non-NULL fits area-weighted by
// point_weights[gi]. There is no silent fallback — NULL *means* unweighted.
// v7 fits planes UNWEIGHTED by default: call sites pass the weights through
// `v7_fit_weights()` (below), the single policy switch, which returns NULL
// unless V7_PLANE_FIT_AREA_WEIGHTED is defined. See v7_fit_weights for why
// (ts-weight is a tangential area, irrelevant to a perpendicular fit) and
// how to flip the policy back for A/B.
//
// Convention: v2 hardcodes y-up. v3 production runs z-up (LAZ). The
// `up_axis` parameter (1 = y, 2 = z) selects the "flip up" component
// for the normal sign convention and the projection axis for tilt /
// azimuth. Set `up_axis = 1` from the bench harness to byte-validate
// against v2 dumps; `2` from the production v3 step wrapper.

#ifndef V3_STEPS_V7_PLANE_FIT_H
#define V3_STEPS_V7_PLANE_FIT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef V7_PI
#define V7_PI 3.14159265358979323846
#endif

typedef struct v7_plane_fit {
    double normal[3];        // unit, oriented so normal[up_axis] >= 0
    double d;                // n·p + d = 0
    double centroid[3];
    double rms;              // sqrt(smallest eigenvalue) — variance along normal
    double eigenvalues[3];   // descending: λ1 ≥ λ2 ≥ λ3
} v7_plane_fit_t;

// Fit a plane to a subset of `positions` (F64×3 interleaved) selected
// by `indices` (length `n_indices`). `up_axis` is 1 (y-up) or 2 (z-up).
// `point_weights == NULL` → unweighted; non-NULL → area-weighted (length
// covers max(indices)+1, or the global point count for safety). Returns 0
// on success, -1 if degenerate (n_indices < 3 or fully-spherical covariance).
//
// ACSL note: the contract treats `positions` and `point_weights` as
// nullable + valid-when-not-NULL for at least one element. The TRUE
// access bound is max(indices[])+1, which we can't express without an
// extra param; callers are expected to ensure indices[] are in range
// (this is a recurring v3 idiom — see the artifact_view.h `data + bytes`
// pattern). EVA will still catch a sure overrun via abstract values
// from the concrete harness in
// scripts/frama-c/harnesses/v7/normal_cascade_seed_impl_harness.c.
/*@ requires positions == \null || \valid_read(positions);
    requires point_weights == \null || \valid_read(point_weights);
    requires indices == \null || \valid_read(indices + (0 .. n_indices - 1));
    requires up_axis == 1 || up_axis == 2;
    requires out == \null || \valid(out);
    assigns *out \from positions, point_weights, indices, n_indices, up_axis;
    ensures \result == 0 || \result == -1;
*/
int v7_plane_fit_lsq(const double* positions,
                     const float*  point_weights,
                     const uint32_t* indices,
                     size_t n_indices,
                     int up_axis,
                     v7_plane_fit_t* out);

// Fit-weighting policy, in ONE place. v7 fits planes UNWEIGHTED: this
// returns NULL (→ v7_plane_fit_lsq ignores the weights), so a call site
// reads `v7_plane_fit_lsq(pos, v7_fit_weights(ts_w), ...)` — the wrapper
// announces "weights subject to the fit policy", not "weighted".
//
// Rationale: ts-weight is a *tangential* Voronoi area (how much roof a point
// covers); a plane fit measures *perpendicular* deviation, so area shouldn't
// sway it — and a sparse swath-edge point (large weight) is the LEAST
// reliable. Define V7_PLANE_FIT_AREA_WEIGHTED (cmake
// -DDIORAMAP_PLANE_FIT_AREA_WEIGHTED=ON) to restore the area-weighted fit for
// an A/B or to revert the decision — the only line that changes.
static inline const float* v7_fit_weights(const float* ts_weights) {
#ifdef V7_PLANE_FIT_AREA_WEIGHTED
    return ts_weights;       // area-weighted fit
#else
    (void)ts_weights;
    return NULL;             // unweighted fit (default)
#endif
}

// Signed point-to-plane distance.
/*@ requires \valid_read(normal + (0 .. 2));
    requires \valid_read(pt + (0 .. 2));
    assigns \nothing;
*/
static inline double v7_signed_distance_to_plane(const double normal[3],
                                                  double d,
                                                  const double pt[3]) {
    return normal[0] * pt[0] + normal[1] * pt[1] + normal[2] * pt[2] + d;
}

// Tilt (degrees) — angle between `normal` and the up-axis. 0° =
// horizontal, 90° = vertical wall. Unsigned (uses |n[up_axis]|).
/*@ requires \valid_read(n + (0 .. 2));
    requires up_axis == 1 || up_axis == 2;
    assigns \nothing;
    ensures 0.0 <= \result <= 90.0;
*/
static inline double v7_tilt_from_normal(const double n[3], int up_axis) {
    double v = fabs(n[up_axis]);
    if (v > 1.0) v = 1.0;
    return acos(v) * (180.0 / V7_PI);
}

// Azimuth (degrees, 0 = north of horizontal frame, clockwise from +H1
// toward +H0). H0 / H1 are the two non-up components in axis order:
// up_axis=1 → (H0,H1)=(x,z) — matches v2 atan2(nx, nz).
// up_axis=2 → (H0,H1)=(x,y) — z-up: atan2(nx, ny).
/*@ requires \valid_read(n + (0 .. 2));
    requires up_axis == 1 || up_axis == 2;
    assigns \nothing;
*/
double v7_azimuth_from_normal(const double n[3], int up_axis);

#ifdef __cplusplus
}
#endif
#endif
