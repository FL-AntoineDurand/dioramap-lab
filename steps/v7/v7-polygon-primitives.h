// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Shared 2D polygon primitives used by v7-boundary-polygons and
// v7-boundary-polygons-2. Single source of truth — do not re-implement
// pt_in_poly / pt_edge_dist locally in any new step.

#ifndef V3_STEPS_V7_POLYGON_PRIMITIVES_H
#define V3_STEPS_V7_POLYGON_PRIMITIVES_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Ray-cast point-in-polygon. Polygon is parallel polyU[]/polyV[] in UV.
bool   v7_pt_in_poly(double pu, double pv,
                      const double* polyU, const double* polyV, int n);

// Closest distance from point (pu,pv) to any polygon edge.
double v7_pt_edge_dist(double pu, double pv,
                        const double* polyU, const double* polyV, int n);

#ifdef __cplusplus
}
#endif
#endif
