// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "registration-steps.h"

int registration_register_steps(void) {
    if (step_register(&build_lidar_rasters_def) != 0) {
        return -1;
    }
    if (step_register(&compute_ndsm_def) != 0) {
        return -1;
    }
    if (step_register(&compute_building_mask_def) != 0) {
        return -1;
    }
    if (step_register(&coregister_ortho_lidar_def) != 0) {
        return -1;
    }
    if (step_register(&rasterize_cadastre_edges_def) != 0) {
        return -1;
    }
    if (step_register(&fit_cadastre_to_lidar_def) != 0) {
        return -1;
    }
    return 0;
}
