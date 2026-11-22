// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Shared 2D polygon primitives — implementation.
//
// History: this code lived as `static` copies in v7-boundary-polygons-impl.c
// and v7-boundary-polygons-2-impl.c. Centralised here on 2026-05-21 so
// the next new step doesn't get its own copy.

#include "v7-polygon-primitives.h"

#include <math.h>

bool v7_pt_in_poly(double pu, double pv,
                    const double* polyU, const double* polyV, int n) {
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((polyV[i] > pv) != (polyV[j] > pv)) &&
            (pu < (polyU[j] - polyU[i]) * (pv - polyV[i]) / (polyV[j] - polyV[i]) + polyU[i])) {
            inside = !inside;
        }
    }
    return inside;
}

double v7_pt_edge_dist(double pu, double pv,
                        const double* polyU, const double* polyV, int n) {
    double min_d = INFINITY;
    for (int k = 0; k < n; k++) {
        int k1 = (k + 1) % n;
        double eu = polyU[k1] - polyU[k];
        double ev = polyV[k1] - polyV[k];
        double l2 = eu * eu + ev * ev;
        if (l2 < 1e-12) continue;
        double t = ((pu - polyU[k]) * eu + (pv - polyV[k]) * ev) / l2;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        double dx = pu - (polyU[k] + t * eu);
        double dy = pv - (polyV[k] + t * ev);
        double d = sqrt(dx * dx + dy * dy);
        if (d < min_d) min_d = d;
    }
    return min_d;
}
