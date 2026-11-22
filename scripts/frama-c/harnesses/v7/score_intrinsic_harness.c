// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl harness for v7-score-intrinsic-impl.c. Same shape as the
// grow-refit-merge / normal-cascade-seed harnesses: bypass the bench /
// step IO by injecting bounded abstract buffers, drive
// v7_score_intrinsic_compute / pack / unpack directly.
//
// EVA proof goal: no sure alarm (no out-of-bounds read, no NULL deref,
// no divide-by-zero, no NaN-arithmetic blow-up) over the full input
// domain expressed below. The bounded loop on `owner` keeps any
// plane-index dereference to a valid slot in `planes[]`.

#include "v7-score-intrinsic-impl.h"

#include <__fc_builtin.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define N_PTS    8
#define N_PLANES 3

int main(void) {
    double            positions[N_PTS * 3];
    int16_t           owner    [N_PTS];
    v7_score_plane_t  planes   [N_PLANES];
    v7_score_intrinsic_result_t r;
    v7_score_intrinsic_result_t r2;
    uint8_t           buf[V7_SCORE_INTRINSIC_RECORD_BYTES];

    Frama_C_make_unknown((char*)positions, sizeof(positions));
    Frama_C_make_unknown((char*)owner,     sizeof(owner));
    Frama_C_make_unknown((char*)planes,    sizeof(planes));

    // Bound owner to [-1, N_PLANES-1]. The impl defensively skips any
    // owner outside [0, n_planes), but tightening the abstract domain
    // helps EVA prove the planes[] dereference is in-bounds without
    // ambiguity. -1 (unassigned) stays valid.
    for (size_t i = 0; i < N_PTS; i++) {
        if (owner[i] < (int16_t)-1 || owner[i] >= (int16_t)N_PLANES) {
            owner[i] = (int16_t)-1;
        }
    }

    // Bound positions / plane equations to a finite box so EVA can
    // prove no NaN/Inf arises from the signed_distance arithmetic. The
    // impl's `isfinite(sd)` guard catches a NaN result AFTER the FP
    // operation has been performed, so the abstract NaN-from-unbounded
    // doubles still trips a sure alarm on the multiply / add itself.
    // Same precedent as the polygon_primitives harness (see
    // docs/quality/FRAMA-C-FALSE-POSITIVES.md §5). ±1000 m covers any
    // realistic building bbox; normals are unit so [-1, 1].
    for (size_t i = 0; i < N_PTS * 3; i++) {
        if (positions[i] < -1000.0 || positions[i] > 1000.0
            || positions[i] != positions[i] /* NaN check w/o <math.h> */) {
            positions[i] = 0.0;
        }
    }
    for (size_t i = 0; i < N_PLANES; i++) {
        for (int k = 0; k < 3; k++) {
            double v = planes[i].normal[k];
            if (v < -1.0 || v > 1.0 || v != v) planes[i].normal[k] = 0.0;
        }
        double d = planes[i].d;
        if (d < -1000.0 || d > 1000.0 || d != d) planes[i].d = 0.0;
    }

    // ── Happy path ──
    (void)v7_score_intrinsic_compute(positions, owner, N_PTS,
                                       planes, N_PLANES, &r);
    (void)v7_score_intrinsic_pack(&r, buf);
    (void)v7_score_intrinsic_unpack(buf, sizeof(buf), &r2);

    // ── NULL / degenerate-input paths (impl returns -1 cleanly) ──
    (void)v7_score_intrinsic_compute(/*positions=*/(double*)0,
                                       owner, N_PTS, planes, N_PLANES, &r);
    (void)v7_score_intrinsic_compute(positions, /*owner=*/(int16_t*)0,
                                       N_PTS, planes, N_PLANES, &r);
    (void)v7_score_intrinsic_compute(positions, owner, N_PTS,
                                       /*planes=*/(v7_score_plane_t*)0,
                                       /*n_planes=*/0, &r);
    (void)v7_score_intrinsic_compute(positions, owner, /*n_pts=*/0,
                                       planes, N_PLANES, &r);
    (void)v7_score_intrinsic_compute(positions, owner, N_PTS,
                                       planes, N_PLANES, /*out=*/(void*)0);

    // ── Short-buffer unpack ──
    (void)v7_score_intrinsic_unpack(buf, /*len=*/0, &r2);

    return 0;
}
