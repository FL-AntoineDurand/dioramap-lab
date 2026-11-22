// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl Frama-C harness for v7-boundary-polygons-2-impl.c (bp2).
//
// bp2's top-level v7_bp2_compute consumes a v7_bp2_inputs_t assembled
// from JSON + grid-contour output (file/parse-heavy), so — like the
// grid-contour harness — we bypass it and drive the pure geometry / cost
// kernels and the TSP `best_cycle` directly with bounded, finite,
// non-deterministic data. A concrete orthonormal basis is used (the
// projection frame is always well-formed in the pipeline) so Eva isn't
// chasing arbitrary-basis float noise; the polygon/anchor coordinates and
// weights are the fuzzed inputs.
//
// RTE goal: no UB (OOB read, integer div/mod by zero, signed overflow) in
// the polygon predicates, the WSD cost, and Heap's-algorithm permutation
// enumeration in best_cycle, for any finite small input.
//
// Build: not built by CMake. Driven via:
//   scripts/frama-c/audit-module.sh bp2
//
// See docs/quality/README.md.

#include "v7-boundary-polygons-2-impl.h"

#include <__fc_builtin.h>     // Frama_C_double_interval, Frama_C_*_interval
#include <stddef.h>
#include <stdint.h>

#define NV 4    // polygon vertices / anchors

static double fin(void)  { return Frama_C_double_interval(-100.0, 100.0); }
static float  finf(void) { return (float)Frama_C_double_interval(0.0, 10.0); }

int main(void) {
    // ── bounded polygon in UV ──
    double polyU[NV], polyV[NV];
    for (int i = 0; i < NV; i++) { polyU[i] = fin(); polyV[i] = fin(); }
    double pu = fin(), pv = fin();

    volatile bool   pin  = v7_bp2_pt_in_poly(pu, pv, polyU, polyV, NV);
    volatile double ped  = v7_bp2_pt_edge_dist(pu, pv, polyU, polyV, NV);
    volatile double area = v7_bp2_polygon_area_uv(polyU, polyV, NV);
    volatile double per  = v7_bp2_polygon_perimeter_uv(polyU, polyV, NV);
    volatile double cmp  = v7_bp2_polygon_compactness_uv(polyU, polyV, NV);
    volatile bool   si   = v7_bp2_polygon_self_intersects_uv(polyU, polyV, NV);
    volatile double spk  = v7_bp2_spike_penalty_uv(polyU, polyV, NV, 30.0);
    (void)pin; (void)ped; (void)area; (void)per; (void)cmp; (void)si; (void)spk;

    // ── concrete projection frame ──
    v7_bp2_basis_t basis = {
        .e1     = { 1.0, 0.0, 0.0 },
        .e2     = { 0.0, 1.0, 0.0 },
        .normal = { 0.0, 0.0, 1.0 },
    };

    // ── anchors + a valid cycle through all of them ──
    v7_bp2_anchor_t anchors[NV];
    for (int i = 0; i < NV; i++) {
        anchors[i].id       = (uint32_t)i;
        anchors[i].point[0] = fin();
        anchors[i].point[1] = fin();
        anchors[i].point[2] = fin();
        anchors[i].source   = (uint8_t)Frama_C_unsigned_int_interval(1, 8);
    }
    uint32_t cycle[NV] = { 0, 1, 2, 3 };

    volatile double cg = v7_bp2_cost_geometric(anchors, cycle, NV, &basis,
                                               0.1, 0.1, 30.0);
    (void)cg;

    // ── WSD + functional cost on small bounded point sets ──
    double group_pts[3 * 3], nearby_pts[3 * 2];
    float  group_w[3], nearby_w[2];
    for (int i = 0; i < 3 * 3; i++) group_pts[i]  = fin();
    for (int i = 0; i < 3 * 2; i++) nearby_pts[i] = fin();
    for (int i = 0; i < 3; i++) group_w[i]  = finf();
    for (int i = 0; i < 2; i++) nearby_w[i] = finf();

    volatile double wsd = v7_bp2_wsd_multi(group_pts, 3, group_w,
                                           nearby_pts, 2, nearby_w,
                                           &basis,
                                           polyU, polyV, NV,
                                           NULL, NULL, NULL, 0,
                                           true);
    (void)wsd;

    volatile double cf = v7_bp2_cost_functional(anchors, cycle, NV, &basis,
                                                group_pts, 3, group_w,
                                                nearby_pts, 2, nearby_w,
                                                NULL, NULL, NULL, 0,
                                                true, 0.8, 0.1, 0.1, 30.0);
    (void)cf;

    // ── TSP best cycle (full-enum path: n_idx ≤ FULL_ENUM_MAX) ──
    uint32_t subset[NV] = { 0, 1, 2, 3 };
    uint32_t out_cycle[NV];
    double   out_cost;
    v7_bp2_cost_ctx_t ctx = {
        .anchors             = anchors,
        .basis               = basis,
        .weight_compactness  = 0.1,
        .weight_spike        = 0.1,
        .spike_threshold_deg = 30.0,
        .group_pts = NULL, .n_group = 0, .group_w = NULL,
        .nearby_pts = NULL, .n_nearby = 0, .nearby_w = NULL,
        .holesU = NULL, .holesV = NULL, .holes_n = NULL, .n_holes = 0,
        .include_foreign_in = false,
        .weight_wsd = 0.8,
    };
    (void)v7_bp2_best_cycle(anchors, subset, NV, V7_BP2_MODE_GEOMETRIC,
                            &ctx, out_cycle, &out_cost);

    return 0;
}
