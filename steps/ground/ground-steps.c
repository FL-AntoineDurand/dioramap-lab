// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "ground-steps.h"

int ground_register_steps(void) {
    if (step_register(&build_lidar_ground_def) != 0) return -1;
    if (step_register(&build_parcel_lines_def) != 0) return -1;
    return 0;
}
