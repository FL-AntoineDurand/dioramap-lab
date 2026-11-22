// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "normals-steps.h"

int normals_register_steps(void) {
    if (step_register(&compute_normals_def) != 0) {
        return -1;
    }
    return 0;
}
