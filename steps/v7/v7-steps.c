// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "v7-steps.h"

int v7_register_steps(void) {
    if (step_register(&v7_canary_def) != 0) return -1;
    if (step_register(&v7_derive_thresholds_def) != 0) return -1;
    if (step_register(&v7_tile_signals_def) != 0) return -1;
    if (step_register(&v7_load_real_test_case_def) != 0) return -1;
    if (step_register(&build_building_kd_trees_def) != 0) return -1;
    if (step_register(&v7_hemisphere_segment_def) != 0) return -1;
    if (step_register(&v7_normal_cascade_seed_def) != 0) return -1;
    if (step_register(&v7_density_voids_2_def) != 0) return -1;
    if (step_register(&v7_grow_refit_merge_def) != 0) return -1;
    if (step_register(&v7_beam_incidence_def) != 0) return -1;
    if (step_register(&v7_boundary_topology_def) != 0) return -1;
    if (step_register(&v7_boundary_signals_def) != 0) return -1;
    if (step_register(&v7_boundary_corners_def) != 0) return -1;
    // v7-boundary-polygons (bp1) retired in task #71 — superseded by
    // v7-boundary-polygons-2 (bp2, contour arc-length ordering). bp1
    // impl files survive only as a link-time dependency of the legacy
    // v7-boundary-merge-bench v2-parity gate (task #76 retires that).
    if (step_register(&v7_boundary_polygons_2_def) != 0) return -1;
    // v7-boundary-merge retired in task #76 (v2-divergence-only step; not
    // in any production pipeline since the Path-B reorder of 0ca5a27).
    // v7-boundary-merge-impl.h survives as a types-only stub for
    // classify / wall-detect / regularize input records.
    if (step_register(&v7_boundary_classify_def) != 0) return -1;
    if (step_register(&v7_boundary_regularize_def) != 0) return -1;
    if (step_register(&v7_wall_detect_def) != 0) return -1;
    if (step_register(&v7_score_intrinsic_def) != 0) return -1;
    if (step_register(&v7_score_intrinsic_postwall_def) != 0) return -1;
    return 0;
}
