// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl Frama-C harness for v7-boundary-classify-impl.c.
//
// Drives v7_bclass_compute directly with a small, valid post-merge
// roof-graph (the step's bench loads bench-v2 files, so the bench-as-
// harness path doesn't fit — same as bp2). Inputs follow the builders in
// core/tests/v7_boundary_classify_test.c: corners carry no plane_ids
// (NULL/0 is legal), edges carry a small owned plane_ids buffer,
// polygon_paths / ridges are NULL/0 (the simplest reachable path).
// Coordinates are finite (Frama_C_double_interval) — the real precondition.
//
// RTE goal: no UB (OOB read on the edge/corner/plane arrays, the t-axis
// coverage math, the PIP probe walk) for a finite small graph. The ACSL
// preconditions on v7_bclass_compute are checked at the (reachable) call.
//
// Build: not built by CMake. Driven via:
//   scripts/frama-c/audit-module.sh v7_boundary_classify

#include "v7-boundary-classify-impl.h"

#include <__fc_builtin.h>     // Frama_C_double_interval
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define N_PLANES  2
#define N_CORNERS 4
#define N_EDGES   3

static double fin(void) { return Frama_C_double_interval(-100.0, 100.0); }

int main(void) {
    // One roof plane (flat) + one wall plane — exercises the wall-tilt gate.
    v7_bclass_input_plane_t planes[N_PLANES];
    memset(planes, 0, sizeof(planes));
    planes[0].id = 0; planes[0].tilt_deg = 0.0;
    planes[0].normal[0] = 0.0; planes[0].normal[1] = 1.0; planes[0].normal[2] = 0.0;
    planes[1].id = 1; planes[1].tilt_deg = 90.0;
    planes[1].normal[0] = 1.0; planes[1].normal[1] = 0.0; planes[1].normal[2] = 0.0;

    // Corners at finite positions; no plane_ids (NULL/0 is legal).
    v7_bclass_input_corner_t corners[N_CORNERS];
    memset(corners, 0, sizeof(corners));
    for (uint32_t i = 0; i < N_CORNERS; i++) {
        corners[i].id = i;
        corners[i].point[0] = fin();
        corners[i].point[1] = fin();
        corners[i].point[2] = fin();
    }

    // Edges, each owning a small plane_ids buffer (1-2 planes per edge).
    uint32_t pid_a[1] = { 0 };
    uint32_t pid_b[2] = { 0, 1 };
    v7_bclass_input_edge_t edges[N_EDGES];
    memset(edges, 0, sizeof(edges));
    edges[0].id = 100; edges[0].corner_a = 0; edges[0].corner_b = 1;
    edges[0].plane_ids = pid_a; edges[0].n_plane_ids = 1;
    edges[1].id = 101; edges[1].corner_a = 1; edges[1].corner_b = 2;
    edges[1].plane_ids = pid_b; edges[1].n_plane_ids = 2;
    edges[2].id = 102; edges[2].corner_a = 2; edges[2].corner_b = 3;
    edges[2].plane_ids = pid_a; edges[2].n_plane_ids = 1;

    v7_bclass_params_t params;
    v7_bclass_params_default(&params);
    params.up_axis = 2;

    // Primary path: planes + corners + edges, no polygon_paths / ridges.
    v7_bclass_outputs_t out;
    (void)v7_bclass_compute(planes, N_PLANES, corners, N_CORNERS,
                            edges, N_EDGES, NULL, 0, NULL, 0, &params, &out);
    v7_bclass_outputs_free(&out);

    // Second path with a polygon path + a ridge (PIP + interior-shared gates).
    uint32_t cyc[4] = { 0, 1, 2, 3 };
    v7_bclass_input_polygon_path_t paths[1];
    memset(paths, 0, sizeof(paths));
    paths[0].plane_id = 0; paths[0].corner_ids = cyc; paths[0].n_corners = 4;
    v7_bclass_input_ridge_t ridges[1] = { { .id = 0, .plane_a = 0, .plane_b = 1 } };

    v7_bclass_outputs_t out2;
    (void)v7_bclass_compute(planes, N_PLANES, corners, N_CORNERS,
                            edges, N_EDGES, paths, 1, ridges, 1, &params, &out2);
    v7_bclass_outputs_free(&out2);

    return 0;
}
