// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-plane-fit — see header.

#include "v7-plane-fit.h"

#include <math.h>

#include "../normals/pca.h"

int v7_plane_fit_lsq(const double* positions,
                     const float*  point_weights,
                     const uint32_t* indices,
                     size_t n_indices,
                     int up_axis,
                     v7_plane_fit_t* out) {
    if (!out || n_indices < 3) return -1;

    // Weighting is the caller's choice, stated honestly via the pointer:
    //   point_weights == NULL → UNWEIGHTED (every point counts equally)
    //   point_weights != NULL → area-weighted by point_weights[gi]
    // v7 fits planes UNWEIGHTED by default — call sites pass
    // v7_fit_weights(...) (→ NULL unless V7_PLANE_FIT_AREA_WEIGHTED). The
    // ts-weight is a *tangential* Voronoi area, irrelevant to a *perpendicular*
    // plane fit, and area-weighting lets sparse swath-edge points dominate the
    // tilt/offset. See v7-plane-fit.h / v7_fit_weights for the policy + how to
    // flip it back. (Area-weighting still applies where it is an area quantity
    // by nature, on OTHER code paths: hemisphere size gates/KDE, the seed
    // curvature gate, the GRM connectivity radius — none use this primitive.)

    // Centroid: c = Σ(w·p) / Σw.
    double cx = 0, cy = 0, cz = 0;
    double w_sum = 0;
    for (size_t i = 0; i < n_indices; i++) {
        const uint32_t gi = indices[i];
        const double w = point_weights ? (double)point_weights[gi] : 1.0;
        const double* p = positions + (size_t)gi * 3;
        cx += w * p[0]; cy += w * p[1]; cz += w * p[2];
        w_sum += w;
    }
    if (w_sum < 1e-15) return -1;
    const double inv_w = 1.0 / w_sum;
    cx *= inv_w; cy *= inv_w; cz *= inv_w;

    // Covariance accumulators (upper triangle).
    double c00 = 0, c01 = 0, c02 = 0;
    double c11 = 0, c12 = 0;
    double c22 = 0;
    for (size_t i = 0; i < n_indices; i++) {
        const uint32_t gi = indices[i];
        const double w = point_weights ? (double)point_weights[gi] : 1.0;
        const double* p = positions + (size_t)gi * 3;
        const double dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
        c00 += w * dx * dx; c01 += w * dx * dy; c02 += w * dx * dz;
        c11 += w * dy * dy; c12 += w * dy * dz;
        c22 += w * dz * dz;
    }
    // Match v2 fitPlaneWeighted: divide by Σw (weighted population covariance).
    double cov[9];
    cov[0] = c00 * inv_w; cov[1] = c01 * inv_w; cov[2] = c02 * inv_w;
    cov[3] = c01 * inv_w; cov[4] = c11 * inv_w; cov[5] = c12 * inv_w;
    cov[6] = c02 * inv_w; cov[7] = c12 * inv_w; cov[8] = c22 * inv_w;

    double evs[3];
    pca_cardano_eigenvalues(cov, evs);

    double n[3];
    if (pca_smallest_eigenvector(cov, n) != 0) return -1;

    // Orient normal "up" along selected axis. v2 always flips on
    // normal.y; here `up_axis` parameterises it.
    if (n[up_axis] < 0.0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }

    out->normal[0] = n[0]; out->normal[1] = n[1]; out->normal[2] = n[2];
    out->centroid[0] = cx; out->centroid[1] = cy; out->centroid[2] = cz;
    out->d = -(n[0] * cx + n[1] * cy + n[2] * cz);
    out->eigenvalues[0] = evs[0];
    out->eigenvalues[1] = evs[1];
    out->eigenvalues[2] = evs[2];
    out->rms = sqrt(evs[2] > 0 ? evs[2] : 0);
    return 0;
}

double v7_azimuth_from_normal(const double n[3], int up_axis) {
    // Pick the two non-up components in canonical order. For y-up
    // (up_axis=1) this is (x, z) → atan2(nx, nz), matching v2.
    int h0 = 0, h1 = 0;
    if (up_axis == 1)      { h0 = 0; h1 = 2; }
    else if (up_axis == 2) { h0 = 0; h1 = 1; }
    else                   { h0 = 1; h1 = 2; }
    const double a = n[h0];
    const double b = n[h1];
    if (fabs(a) < 1e-10 && fabs(b) < 1e-10) return 0.0;
    double az = atan2(a, b) * (180.0 / V7_PI);
    if (az < 0.0) az += 360.0;
    return az;
}
