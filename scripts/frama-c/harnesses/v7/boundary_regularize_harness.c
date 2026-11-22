// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl Frama-C harness for v7-boundary-regularize-impl.c.
//
// Drives v7_br_compute directly with a small valid post-classify graph
// (the step's bench loads bench-v2 files, so bench-as-harness doesn't fit).
// Construction mirrors core/tests/v7_boundary_regularize_test.c: a few
// planes + corners carrying owned plane_ids buffers (1/2/3 planes → the
// 2/1/0-DOF tagging branches), finite coords, witness positions + owner.
//
// Coverage map across the three Paths below:
//   Path 1 — R.0 (DOF tag) + R.1 (seeded mulberry32 nudge loop), one
//            ring, no ridges. Confirms the PRNG path has no UB.
//   Path 2 — adds a polygon path + residual array. Same R.0/R.1 scope.
//   Path 3 — adds ridges + edges + ridge_id-tagged corners. THIS is
//            the only path that reaches the make-shared cascade (T4
//            SNAP_IN, T5 MERGE_CORNER, T6 DROP, T7 SPLIT, L1 orphan-
//            kill). The bench-as-harness (audit-v7-bench.sh) over
//            v7-boundary-regularize-only-bench.c CAN'T reach these
//            ops — that bench passes (NULL, 0) for ridges/edges — so
//            this per-impl harness is the only audit signal on them.
//            See task #93.
//
// max_iterations is small but NON-zero and joint_move_seed is fixed, so
// the harness exercises R.0 (DOF tagging) AND R.1 (the seeded mulberry32
// nudge loop) — confirming the PRNG path has no UB. Determinism comes from
// the seed; there is no global/time state.
//
// RTE goal: no UB (OOB on the corner/plane/position arrays + plane_ids
// buffers, the id-map writes, the nudge/cost math) for a finite input.
//
// Build: not built by CMake. Driven via:
//   scripts/frama-c/audit-module.sh v7_br_impl

#include "v7-boundary-regularize-impl.h"

#include <__fc_builtin.h>     // Frama_C_double_interval
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define N_PLANES  3
#define N_CORNERS 3
#define N_PTS     4

static double fin(void) { return Frama_C_double_interval(-100.0, 100.0); }

int main(void) {
    // One flat + two slope planes (concrete frames).
    v7_br_input_plane_t planes[N_PLANES];
    memset(planes, 0, sizeof(planes));
    planes[0].id = 0; planes[0].tilt_deg = 0.0;
    planes[0].normal[1] = 1.0; planes[0].d = -5.0;
    planes[1].id = 1; planes[1].tilt_deg = 45.0;
    planes[1].normal[1] = 0.707; planes[1].normal[2] = 0.707; planes[1].d = -5.0;
    planes[2].id = 2; planes[2].tilt_deg = 45.0;
    planes[2].normal[0] = 0.707; planes[2].normal[1] = 0.707; planes[2].d = -5.0;

    // plane_ids buffers: 3-plane (0 DOF), 2-plane (1 DOF), 1-plane (2 DOF).
    uint32_t p3[3] = { 0, 1, 2 };
    uint32_t p2[2] = { 0, 1 };
    uint32_t p1[1] = { 0 };
    v7_br_input_corner_t corners[N_CORNERS];
    memset(corners, 0, sizeof(corners));
    corners[0].id = 100; corners[0].plane_ids = p3; corners[0].n_plane_ids = 3;
    corners[1].id = 101; corners[1].plane_ids = p2; corners[1].n_plane_ids = 2;
    corners[2].id = 102; corners[2].plane_ids = p1; corners[2].n_plane_ids = 1;
    for (int i = 0; i < N_CORNERS; i++) {
        corners[i].point[0] = fin();
        corners[i].point[1] = fin();
        corners[i].point[2] = fin();
    }

    // Witness points + owner (finite, concrete).
    float positions[3 * N_PTS] = {
        0.0f, 0.0f, 0.0f,  1.0f, 0.1f, 0.0f,  2.0f, 0.0f, 1.0f,  0.5f, 0.2f, 0.5f
    };
    int16_t owner[N_PTS] = { 0, 0, 1, 2 };
    float residual[N_PTS] = { 0.01f, 0.02f, 0.0f, 0.05f };

    v7_br_params_t params;
    v7_br_params_default(&params);
    params.up_axis = 1;            // impl is hardcoded y-up
    // Keep R.1 tractable for Eva: ONE iteration with ONE PRNG sample per
    // edge still exercises the full nudge + mulberry32 path (the loop body
    // is what matters for RTE), without the default 30-sample × multi-iter
    // blow-up that EVA can't finish.
    params.max_iterations = 1;
    params.joint_samples_per_edge = 1;
    params.joint_move_seed = 12345u;

    // Path 1: minimal — planes + corners + positions + owner; rest NULL.
    v7_br_outputs_t out1; v7_br_outputs_init(&out1);
    (void)v7_br_compute(planes, N_PLANES, corners, N_CORNERS,
                        NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        positions, N_PTS, owner,
                        /*residual=*/NULL, 0.12, &params, &out1);
    v7_br_outputs_free(&out1);

    // Path 2: add a polygon path (corner cycle) + a residual array.
    uint32_t cyc[3] = { 100, 101, 102 };
    v7_br_input_polygon_path_t paths[1];
    memset(paths, 0, sizeof(paths));
    paths[0].plane_id = 0; paths[0].corner_ids = cyc; paths[0].n_corners = 3;

    v7_br_outputs_t out2; v7_br_outputs_init(&out2);
    (void)v7_br_compute(planes, N_PLANES, corners, N_CORNERS,
                        NULL, 0, paths, 1, NULL, 0, NULL, 0,
                        positions, N_PTS, owner,
                        residual, 0.12, &params, &out2);
    v7_br_outputs_free(&out2);

    // ── Path 3: drive the make-shared cascade (T0..T8, task #93) ───
    // The base harness above only covers R.0 (DOF tag) + R.1 (PRNG +
    // single nudge) on three corners, NO ridges, NO edges, ONE ring.
    // That leaves the five named ops added in the make-shared refactor
    // (T4 SNAP_IN, T5 MERGE_CORNER, T6 DROP_REDUNDANT_ON_RIDGE,
    // T7 SPLIT_TO_SHARE_CORNER, L1 orphan-kill) UNREACHED — Eva counts
    // their code as "unreachable" (= Valid by default) and gives us no
    // signal if a future change breaks them.
    //
    // This path adds:
    //   * 2 ridges, 1 allow-listed type=ridge + 1 excluded type=flashing
    //     (exercises RIDGE_TYPE_ALLOWED tag-gating, §9 / I1).
    //   * 2 edges with plane_ids cycles (paths-derived membership reads).
    //   * 4 corners, 2 of them tagged with ridge_ids[] (snap-in tag-driven
    //     code path) and pre-positioned so cs[].ridge_ids reality-intersect
    //     against rg[] clipped segments can hit a coplanarity τ_in branch.
    //   * 2 polygon paths (cross-plane membership), so planes_of(ci) at
    //     R.0 reaches the multi-plane path-scan branch.
    //   * residual = NULL for this path → trust=1 everywhere (covers the
    //     NULL-branch in use_trust_weighting).
    uint32_t r10_a[1] = { 10 };     // corner 200's ridge tag
    uint32_t r10_b[1] = { 10 };     // corner 201's ridge tag (paired with 200)
    uint32_t p_a[1]   = { 1 };      // corner 200: single-plane on plane 1
    uint32_t p_b[1]   = { 2 };      // corner 201: single-plane on plane 2
                                     // → cross-plane pair on ridge 10 (T5 candidate)
    uint32_t p_ridge_junc[2] = { 1, 2 };  // corner 202: existing 2-plane junction on ridge
    uint32_t p_plain[1] = { 0 };    // corner 203: single-plane on plane 0 (no ridge tag)

    v7_br_input_corner_t corners3[4];
    memset(corners3, 0, sizeof(corners3));
    corners3[0].id = 200; corners3[0].plane_ids = p_a;
    corners3[0].n_plane_ids = 1;   corners3[0].ridge_ids = r10_a; corners3[0].n_ridge_ids = 1;
    corners3[1].id = 201; corners3[1].plane_ids = p_b;
    corners3[1].n_plane_ids = 1;   corners3[1].ridge_ids = r10_b; corners3[1].n_ridge_ids = 1;
    corners3[2].id = 202; corners3[2].plane_ids = p_ridge_junc;
    corners3[2].n_plane_ids = 2;   /* no ridge_ids — DOF-1 by topology */
    corners3[3].id = 203; corners3[3].plane_ids = p_plain;
    corners3[3].n_plane_ids = 1;
    for (int i = 0; i < 4; i++) {
        corners3[i].point[0] = fin();
        corners3[i].point[1] = fin();
        corners3[i].point[2] = fin();
    }

    // Ridges. r10 is allow-listed (type=1 ridge); r11 is excluded
    // (type=4 flashing) so the RIDGE_TYPE_ALLOWED gate's REJECT branch
    // also gets exercised.
    v7_br_input_ridge_t rgs[2];
    memset(rgs, 0, sizeof(rgs));
    rgs[0].id = 10; rgs[0].plane_a = 1; rgs[0].plane_b = 2; rgs[0].type = 1; // ridge
    rgs[0].from[0] = fin(); rgs[0].from[1] = fin(); rgs[0].from[2] = fin();
    rgs[0].to[0]   = fin(); rgs[0].to[1]   = fin(); rgs[0].to[2]   = fin();
    rgs[1].id = 11; rgs[1].plane_a = 0; rgs[1].plane_b = 1; rgs[1].type = 4; // flashing (excluded)
    rgs[1].from[0] = fin(); rgs[1].from[1] = fin(); rgs[1].from[2] = fin();
    rgs[1].to[0]   = fin(); rgs[1].to[1]   = fin(); rgs[1].to[2]   = fin();

    // Edges (with plane_ids cycles — paths reconstruction reads these).
    uint32_t e0_pi[2] = { 1, 2 };
    uint32_t e1_pi[1] = { 0 };
    v7_br_input_edge_t edges3[2];
    memset(edges3, 0, sizeof(edges3));
    edges3[0].id = 1000; edges3[0].corner_a = 200; edges3[0].corner_b = 201;
    edges3[0].plane_ids = e0_pi; edges3[0].n_plane_ids = 2; edges3[0].length = 1.0f;
    edges3[1].id = 1001; edges3[1].corner_a = 202; edges3[1].corner_b = 203;
    edges3[1].plane_ids = e1_pi; edges3[1].n_plane_ids = 1; edges3[1].length = 1.0f;

    // Two polygon paths (rings):
    //   path[0] plane 1: 200 → 202 → 203 (closes via implicit wrap)
    //   path[1] plane 2: 201 → 202 → 203
    // These are pathological in size (n=3) but enough to drive
    // planes_of(ci) into the multi-path branch and the n<3 collapse
    // check (L1 orphan-kill path).
    uint32_t cyc3a[3] = { 200, 202, 203 };
    uint32_t cyc3b[3] = { 201, 202, 203 };
    v7_br_input_polygon_path_t paths3[2];
    memset(paths3, 0, sizeof(paths3));
    paths3[0].plane_id = 1; paths3[0].corner_ids = cyc3a; paths3[0].n_corners = 3;
    paths3[1].plane_id = 2; paths3[1].corner_ids = cyc3b; paths3[1].n_corners = 3;

    int16_t owner3[N_PTS] = { 1, 1, 2, 0 };

    v7_br_outputs_t out3; v7_br_outputs_init(&out3);
    (void)v7_br_compute(planes, N_PLANES, corners3, 4,
                        edges3, 2, paths3, 2, rgs, 2, NULL, 0,
                        positions, N_PTS, owner3,
                        /*residual=*/NULL, 0.12, &params, &out3);
    v7_br_outputs_free(&out3);

    return 0;
}
