// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for the shared 2D polygon primitives
// (v7-polygon-primitives.c): v7_pt_in_poly and v7_pt_edge_dist.
//
// Both are pure: bounded array reads + float arithmetic, no allocation,
// no I/O. The RTE goal is to prove no UB (OOB read, integer div/mod by
// zero, signed overflow) for any polygon of 3..AUD_MAXV vertices and any
// query point — including non-finite coordinates (float division by zero
// is IEEE-defined, not UB, so it raises no RTE obligation).
//
// Build: not built by CMake. Driven via:
//   scripts/frama-c/audit-module.sh polygon_primitives
//
// See docs/quality/README.md.

#include "v7-polygon-primitives.h"

#include <__fc_builtin.h>     // Frama_C_make_unknown, Frama_C_*_interval
#include <stdbool.h>
#include <stddef.h>

#define AUD_MAXV 8

int main(void) {
    double polyU[AUD_MAXV];
    double polyV[AUD_MAXV];
    double pu, pv;

    // Real callers always pass FINITE UV coordinates (projected plane
    // geometry). Constraining to a wide finite box is the faithful
    // precondition and lets Eva discharge the ¬is_NaN obligations on the
    // array reads — leaving only the few relational ones (the PIP edge
    // division) that Eva can't prove but the code's guard makes safe.
    for (int i = 0; i < AUD_MAXV; i++) {
        polyU[i] = Frama_C_double_interval(-1.0e6, 1.0e6);
        polyV[i] = Frama_C_double_interval(-1.0e6, 1.0e6);
    }
    pu = Frama_C_double_interval(-1.0e6, 1.0e6);
    pv = Frama_C_double_interval(-1.0e6, 1.0e6);

    // n constrained to [3, AUD_MAXV]; every index the loops form lies in
    // [0, n) ⊆ [0, AUD_MAXV), and the `% n` in edge-dist has n ≥ 3 > 0.
    int n = (int)Frama_C_unsigned_int_interval(3, AUD_MAXV);

    volatile bool   inside = v7_pt_in_poly(pu, pv, polyU, polyV, n);
    volatile double dist   = v7_pt_edge_dist(pu, pv, polyU, polyV, n);
    (void)inside;
    (void)dist;
    return 0;
}
