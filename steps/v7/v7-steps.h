// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_V7_H
#define V3_STEPS_V7_H

#include "v3/step.h"

// Phase 5 of the roof-recon evaluation: port bench-v2's V7 pipeline
// (33 steps, 26 on the data path) to v3 step by step. Goal is faithful
// re-implementation with output validated against v2 reference dumps
// at every stage. See v3/docs/plan/PHASE-5-V7-PORT.md.
//
// Unlike Phase 2/3/4 (PolyFit / KSR / Roofer — third-party tools we
// vendor and wrap), this phase ports OUR OWN algorithms; the goal is
// to bring the v2 work into v3's typed-step / typed-artifact /
// composable-viz architecture without losing the algorithmic content.

extern const step_def_t v7_canary_def;
extern const step_def_t v7_derive_thresholds_def;
extern const step_def_t v7_tile_signals_def;
extern const step_def_t v7_load_real_test_case_def;
extern const step_def_t build_building_kd_trees_def;
extern const step_def_t v7_hemisphere_segment_def;
extern const step_def_t v7_normal_cascade_seed_def;
extern const step_def_t v7_density_voids_2_def;
extern const step_def_t v7_grow_refit_merge_def;
extern const step_def_t v7_beam_incidence_def;
extern const step_def_t v7_boundary_topology_def;
extern const step_def_t v7_boundary_signals_def;
extern const step_def_t v7_boundary_corners_def;
// v7_boundary_polygons_def — retired in task #71; bp2 is the survivor.
extern const step_def_t v7_boundary_polygons_2_def;
// v7_boundary_merge_def — retired in task #76 (v2-divergence-only step).
extern const step_def_t v7_boundary_classify_def;
extern const step_def_t v7_boundary_regularize_def;
extern const step_def_t v7_wall_detect_def;
extern const step_def_t v7_score_intrinsic_def;
extern const step_def_t v7_score_intrinsic_postwall_def;

int v7_register_steps(void);

#endif
