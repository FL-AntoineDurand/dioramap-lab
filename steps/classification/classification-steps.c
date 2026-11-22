// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "classification-steps.h"

int classification_register_steps(void) {
    if (step_register(&build_tile_ndvi_def) != 0) return -1;
    if (step_register(&compute_normal_scatter_def) != 0) return -1;
    if (step_register(&compute_near_building_mask_def) != 0) return -1;
    if (step_register(&classify_vegetation_def) != 0) return -1;
    return 0;
}
