// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl Frama-C harness for v7-wall-detect-impl.c.
//
// Drives v7_wd_compute directly with a small valid post-classify graph
// (the step's bench loads bench-v2 files, so bench-as-harness doesn't fit).
// Construction mirrors core/tests/v7_wall_detect_test.c: a roof plane, two
// corners (finite coords — the geometry driver), one perimeter edge with an
// owned plane_ids buffer, a perim-tag carrying outward_xz, a couple of
// witness points, and an in/out owner buffer.
//
// Exercises BOTH beam paths: beam_directions == NULL (nadir fallback) and
// a finite beam array. RTE goal: no UB (OOB on the corner/edge/perim_tag/
// positions arrays + the owner_inout write-back, the xz-strip search, the
// beam-downness math) for a finite small input.
//
// Build: not built by CMake. Driven via:
//   scripts/frama-c/audit-module.sh v7_wd_impl

#include "v7-wall-detect-impl.h"

#include <__fc_builtin.h>     // Frama_C_double_interval
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define N_CORNERS 2
#define N_PTS     3

static double fin(void) { return Frama_C_double_interval(-100.0, 100.0); }

int main(void) {
    // One flat roof plane.
    v7_wd_input_plane_t planes[1];
    memset(planes, 0, sizeof(planes));
    planes[0].id = 0; planes[0].tilt_deg = 0.0;
    planes[0].normal[0] = 0.0; planes[0].normal[1] = 1.0; planes[0].normal[2] = 0.0;
    planes[0].d = -5.0;

    // Corners at finite positions.
    v7_wd_input_corner_t corners[N_CORNERS];
    memset(corners, 0, sizeof(corners));
    for (uint32_t i = 0; i < N_CORNERS; i++) {
        corners[i].id = i;
        corners[i].point[0] = fin();
        corners[i].point[1] = fin();
        corners[i].point[2] = fin();
    }

    // One perimeter edge spanning the two corners; owned plane_ids buffer.
    uint32_t pids[1] = { 0 };
    v7_wd_input_edge_t edges[1];
    memset(edges, 0, sizeof(edges));
    edges[0].id = 100; edges[0].corner_a = 0; edges[0].corner_b = 1;
    edges[0].plane_ids = pids; edges[0].n_plane_ids = 1;

    // Perim tag for that edge, carrying the outward_xz the compute requires.
    v7_wd_input_perim_tag_t tags[1];
    memset(tags, 0, sizeof(tags));
    tags[0].edge_id = 100; tags[0].tag = V7_BCLASS_TAG_PERIMETER;
    tags[0].has_outward_xz = true;
    tags[0].outward_xz[0] = 0.0f; tags[0].outward_xz[1] = -1.0f;

    // Witness points + in/out owner buffer (finite, concrete).
    float positions[3 * N_PTS] = { 1.0f, 0.0f, 0.5f,  3.0f, 0.0f, 0.5f,  2.0f, 0.1f, 0.5f };
    float beams[3 * N_PTS]     = { 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f };
    int32_t owner[N_PTS] = { 0, 0, 0 };

    v7_wd_params_t params;
    v7_wd_params_default(&params);
    params.up_axis = 2;

    // Path 1: beam_directions == NULL (nadir fallback).
    v7_wd_outputs_t out1;
    (void)v7_wd_compute(planes, 1, corners, N_CORNERS, edges, 1, tags, 1,
                        positions, N_PTS, /*beam_directions=*/NULL,
                        owner, /*point_plane_dist_threshold=*/0.1,
                        &params, &out1);
    v7_wd_outputs_free(&out1);

    // Path 2: explicit (finite) beam directions.
    int32_t owner2[N_PTS] = { 0, 0, 0 };
    v7_wd_outputs_t out2;
    (void)v7_wd_compute(planes, 1, corners, N_CORNERS, edges, 1, tags, 1,
                        positions, N_PTS, beams,
                        owner2, 0.1, &params, &out2);
    v7_wd_outputs_free(&out2);

    return 0;
}
